/*
 * Copyright (c) 2026 honyomi
 *
 * TorrentWorker — dedicated Worker thread for the libtorrent session.
 *
 * Owns the process-wide BitTorrent engine (libtorrent-rasterbar 2.0.15 via
 * libtorrent.so): every torrent* NAPI call runs here, never on the UI
 * thread. Drives a 1s poll loop that drains libtorrent alerts and snapshots
 * per-torrent progress back to the host (TorrentManager).
 *
 * Message protocol (host -> worker, TorrentCommand):
 *   { id, cmd: 'start', downloadDir, ... } | { id, cmd: 'addMagnet', ... } |
 *   { id, cmd: 'addTorrentFile', bytes } | { id, cmd: 'status'|'files'|... }
 *
 * Message protocol (worker -> host):
 *   { id, ok, result?, error? }   — command ack (result type varies by cmd)
 *   { ev: 'alerts', events: TorrentAlert[] }   — unsolicited alert batch
 *   { ev: 'progress', statuses: string[] }     — unsolicited status snapshots
 */

import { ErrorEvent, MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import torrentEngine, { TorrentAlert, TorrentStatusJson } from 'libtorrent.so';

interface TorrentCommandStart {
  id: number;
  cmd: 'start';
  downloadDir: string;
  listen?: string;
  enableDht?: boolean;
  downloadLimit?: number;
  uploadLimit?: number;
  connectionsLimit?: number;
  userAgent?: string;
}

interface TorrentCommandAddMagnet {
  id: number;
  cmd: 'addMagnet';
  magnetUri: string;
  savePath?: string;
}

interface TorrentCommandAddTorrentFile {
  id: number;
  cmd: 'addTorrentFile';
  bytes: ArrayBuffer;
  savePath?: string;
}

interface TorrentCommandAddTrackers {
  id: number;
  cmd: 'addTrackers';
  key: number;
  trackers: string[];
}

interface TorrentCommandKeyOp {
  id: number;
  cmd: 'shutdown' | 'status' | 'files' | 'pause' | 'resume';
  key?: number;
}

interface TorrentCommandRemove {
  id: number;
  cmd: 'remove';
  key: number;
  deleteFiles: boolean;
}

interface TorrentCommandSetSequential {
  id: number;
  cmd: 'setSequential';
  key: number;
  enabled: boolean;
}

interface TorrentCommandSetLimits {
  id: number;
  cmd: 'setLimits';
  downloadLimit: number;
  uploadLimit: number;
}

interface TorrentCommandSetShareRatio {
  id: number;
  cmd: 'setShareRatio';
  ratio: number;
}

interface TorrentCommandSetIpFilter {
  id: number;
  cmd: 'setIpFilter';
  rules: string[];
}

type TorrentCommand = TorrentCommandStart | TorrentCommandAddMagnet | TorrentCommandAddTorrentFile |
  TorrentCommandAddTrackers | TorrentCommandKeyOp | TorrentCommandRemove | TorrentCommandSetSequential |
  TorrentCommandSetLimits | TorrentCommandSetShareRatio | TorrentCommandSetIpFilter;

interface CommandAck {
  id: number;
  ok: boolean;
  result: string | number | boolean;
  error?: string;
}

interface AlertsEvent {
  ev: 'alerts';
  events: TorrentAlert[];
}

interface ProgressEvent {
  ev: 'progress';
  statuses: string[];
}

type WorkerEvent = CommandAck | AlertsEvent | ProgressEvent;

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;
const activeKeys: number[] = [];
let pollTimer: number = -1;
let started = false;
let tickCount = 0;

// ---------------------------------------------------------------------------
// Diagnostics: every alert event and a 5s status/dht summary go to hilog so a
// stalled download can be diagnosed from device logs alone. Noisy recurring
// events (tracker failures per URL, peer churn) are throttled per key.
// ---------------------------------------------------------------------------
const lastAlertLogAt: Map<string, number> = new Map();

function throttled(key: string, intervalMs: number): boolean {
  const now: number = Date.now();
  const last: number | undefined = lastAlertLogAt.get(key);
  if (last !== undefined && now - last < intervalMs) {
    return false;
  }
  if (lastAlertLogAt.size > 256) {
    lastAlertLogAt.clear();
  }
  lastAlertLogAt.set(key, now);
  return true;
}

function logAlert(ev: TorrentAlert): void {
  try {
    switch (ev.type) {
      case 'metadata':
        hilog.info(0x0002, 'TorrentWorker', 'metadata key=%{public}d name=%{public}s files=%{public}d',
          ev.key, ev.name, ev.numFiles);
        break;
      case 'finished':
        hilog.info(0x0002, 'TorrentWorker', 'finished key=%{public}d', ev.key);
        break;
      case 'state':
        hilog.info(0x0002, 'TorrentWorker', 'state key=%{public}d -> %{public}s', ev.key, ev.state);
        break;
      case 'error':
        hilog.error(0x0002, 'TorrentWorker', 'error key=%{public}d msg=%{public}s', ev.key, ev.message);
        break;
      case 'fileError':
        hilog.error(0x0002, 'TorrentWorker', 'fileError key=%{public}d file=%{public}s msg=%{public}s',
          ev.key, ev.file, ev.message);
        break;
      case 'removed':
        hilog.info(0x0002, 'TorrentWorker', 'removed key=%{public}d', ev.key);
        break;
      case 'trackerError':
        if (throttled(`te:${ev.url}`, 30000)) {
          hilog.info(0x0002, 'TorrentWorker',
            'trackerError key=%{public}d url=%{public}s msg=%{public}s x%{public}d',
            ev.key, ev.url, ev.message, ev.timesInRow);
        }
        break;
      case 'trackerReply':
        if (throttled(`tr:${ev.url}`, 30000)) {
          hilog.info(0x0002, 'TorrentWorker', 'trackerReply key=%{public}d url=%{public}s peers=%{public}d',
            ev.key, ev.url, ev.peers);
        }
        break;
      case 'peerConnect':
        if (throttled(`pc:${ev.endpoint}`, 10000)) {
          hilog.info(0x0002, 'TorrentWorker', 'peerConnect key=%{public}d peer=%{public}s',
            ev.key, ev.endpoint);
        }
        break;
      case 'peerDisconnect':
        if (throttled(`pd:${ev.key}:${ev.message}`, 10000)) {
          hilog.info(0x0002, 'TorrentWorker', 'peerDisconnect key=%{public}d msg=%{public}s',
            ev.key, ev.message);
        }
        break;
      case 'listen':
        hilog.info(0x0002, 'TorrentWorker', 'listening %{public}s:%{public}d (%{public}s)',
          ev.address, ev.port, ev.socket);
        break;
      case 'listenFailed':
        hilog.info(0x0002, 'TorrentWorker', 'listenFailed iface=%{public}s port=%{public}d msg=%{public}s',
          ev.interface, ev.port, ev.message);
        break;
      case 'dhtReady':
        hilog.info(0x0002, 'TorrentWorker', 'DHT bootstrapped');
        break;
      case 'sessionError':
        hilog.error(0x0002, 'TorrentWorker', 'sessionError: %{public}s', ev.message);
        break;
      case 'alert':
        if (throttled(`al:${ev.message}`, 30000)) {
          hilog.info(0x0002, 'TorrentWorker', 'alert: %{public}s', ev.message);
        }
        break;
      default:
        break;
    }
  } catch (e) {
    // diagnostics must never break the poll loop
  }
}

function logStatusSummary(statuses: string[]): void {
  try {
    let dhtNodes = 0;
    try {
      dhtNodes = torrentEngine.torrentSessionDhtNodes();
    } catch (e) {
      // engine already stopped
    }
    hilog.info(0x0002, 'TorrentWorker', 'summary dhtNodes=%{public}d', dhtNodes);
    for (let i = 0; i < statuses.length; i++) {
      try {
        const st: TorrentStatusJson = JSON.parse(statuses[i]) as TorrentStatusJson;
        hilog.info(0x0002, 'TorrentWorker',
          'st key=%{public}d state=%{public}s meta=%{public}d peers=%{public}d seeds=%{public}d done=%{public}d/%{public}d dl=%{public}d',
          st.key, st.state, st.hasMetadata ? 1 : 0, st.numPeers, st.numSeeds,
          st.totalDone, st.totalWanted, st.downloadRate);
      } catch (e) {
        // malformed snapshot: skip
      }
    }
  } catch (e) {
    // diagnostics must never break the poll loop
  }
}

function post(event: WorkerEvent): void {
  try {
    workerPort.postMessage(event);
  } catch (e) {
    // host gone: nothing to do
  }
}

function ack(id: number, result: string | number | boolean): void {
  const message: CommandAck = { id: id, ok: true, result: result };
  post(message);
}

function fail(id: number, message: string): void {
  const failure: CommandAck = { id: id, ok: false, result: '', error: message };
  post(failure);
}

function errorMessage(err: Object): string {
  const text = `${err}`;
  return text === '[object Object]' ? 'torrent_worker_error' : text;
}

function startPolling(): void {
  if (pollTimer >= 0) {
    return;
  }
  pollTimer = setInterval(() => {
    try {
      const events: TorrentAlert[] = JSON.parse(torrentEngine.torrentPollAlerts(128)) as TorrentAlert[];
      for (let i = 0; i < events.length; i++) {
        logAlert(events[i]);
      }
      if (events.length > 0) {
        const message: AlertsEvent = { ev: 'alerts', events: events };
        post(message);
      }
      const statuses: string[] = [];
      for (let i = 0; i < activeKeys.length; i++) {
        try {
          const raw: string = torrentEngine.torrentStatus(activeKeys[i]);
          statuses.push(raw);
        } catch (e) {
          // torrent vanished between snapshot and query: dropped next tick
        }
      }
      if (statuses.length > 0) {
        const progress: ProgressEvent = { ev: 'progress', statuses: statuses };
        post(progress);
        if (tickCount % 5 === 0) {
          logStatusSummary(statuses);
        }
      }
      tickCount++;
    } catch (err) {
      // never let the poll loop die
    }
  }, 1000);
}

function stopPolling(): void {
  if (pollTimer >= 0) {
    clearInterval(pollTimer);
    pollTimer = -1;
  }
}

function forgetKey(key: number): void {
  const index = activeKeys.indexOf(key);
  if (index >= 0) {
    activeKeys.splice(index, 1);
  }
}

workerPort.onmessage = (e: MessageEvents): void => {
  const command = e.data as TorrentCommand;
  if (command === null || command === undefined) {
    return;
  }
  try {
    switch (command.cmd) {
      case 'start': {
        const start = command as TorrentCommandStart;
        torrentEngine.torrentStart(start.downloadDir, start.listen, start.enableDht,
          start.downloadLimit, start.uploadLimit, start.connectionsLimit, start.userAgent);
        started = true;
        activeKeys.length = 0;
        startPolling();
        ack(start.id, true);
        break;
      }
      case 'shutdown': {
        const shutdown = command as TorrentCommandKeyOp;
        stopPolling();
        activeKeys.length = 0;
        started = false;
        torrentEngine.torrentShutdown();
        ack(shutdown.id, true);
        break;
      }
      case 'addMagnet': {
        const add = command as TorrentCommandAddMagnet;
        const key = torrentEngine.torrentAddMagnet(add.magnetUri, add.savePath);
        activeKeys.push(key);
        ack(add.id, key);
        break;
      }
      case 'addTorrentFile': {
        const add = command as TorrentCommandAddTorrentFile;
        const key = torrentEngine.torrentAddTorrentFile(add.bytes, add.savePath);
        activeKeys.push(key);
        ack(add.id, key);
        break;
      }
      case 'addTrackers': {
        const addTr = command as TorrentCommandAddTrackers;
        ack(addTr.id, torrentEngine.torrentAddTrackers(addTr.key, JSON.stringify(addTr.trackers)));
        break;
      }
      case 'status': {
        const status = command as TorrentCommandKeyOp;
        ack(status.id, torrentEngine.torrentStatus(status.key ?? 0));
        break;
      }
      case 'files': {
        const files = command as TorrentCommandKeyOp;
        ack(files.id, torrentEngine.torrentFiles(files.key ?? 0));
        break;
      }
      case 'pause': {
        const pause = command as TorrentCommandKeyOp;
        ack(pause.id, torrentEngine.torrentPause(pause.key ?? 0));
        break;
      }
      case 'resume': {
        const resume = command as TorrentCommandKeyOp;
        ack(resume.id, torrentEngine.torrentResume(resume.key ?? 0));
        break;
      }
      case 'setSequential': {
        const sequential = command as TorrentCommandSetSequential;
        ack(sequential.id, torrentEngine.torrentSetSequential(sequential.key, sequential.enabled));
        break;
      }
      case 'remove': {
        const remove = command as TorrentCommandRemove;
        torrentEngine.torrentRemove(remove.key, remove.deleteFiles);
        forgetKey(remove.key);
        ack(remove.id, true);
        break;
      }
      case 'setLimits': {
        const limits = command as TorrentCommandSetLimits;
        ack(limits.id, torrentEngine.torrentSetLimits(limits.downloadLimit, limits.uploadLimit));
        break;
      }
      case 'setShareRatio': {
        const shareRatio = command as TorrentCommandSetShareRatio;
        ack(shareRatio.id, torrentEngine.torrentSetShareRatioLimit(shareRatio.ratio));
        break;
      }
      case 'setIpFilter': {
        const ipFilter = command as TorrentCommandSetIpFilter;
        ack(ipFilter.id, torrentEngine.torrentSetIpFilter(JSON.stringify(ipFilter.rules)));
        break;
      }
      default:
        break;
    }
  } catch (err) {
    fail(command.id, errorMessage(err));
  }
};

workerPort.onerror = (err: ErrorEvent): void => {
  if (!started) {
    return;
  }
  // Surface worker crashes as alert-style errors so the host can react.
  const events: TorrentAlert[] = [{ type: 'error', key: 0, message: err.message }];
  const message: AlertsEvent = { ev: 'alerts', events: events };
  post(message);
};
