/*
 * Copyright (c) 2026 honyomi
 *
 * TorrentManager — BT 下载引擎主线程门面。
 *
 * 所有 libtorrent 调用都在 TorrentWorker.ets 独立线程内执行（libtorrent
 * 的 session/alert 轮询会长期占用线程，且磁盘/网络线程池数量可观），主
 * 线程仅通过消息与 Promise 封装交互：
 *   - 命令按自增 id 关联 ack（{id, ok, result, error}）
 *   - worker 每 1s 主动推送 alerts（metadata/finished/error/...）与各任
 *     务的进度快照，通过 onAlert/onProgress 订阅
 *
 * 生命周期：首次 start() 惰性创建 worker 并常驻；shutdown() 仅停引擎不
 * 销毁线程（可再次 start）；worker 异常退出后置 broken，后续调用立即抛
 * 错（进程内自愈不可靠，与 AvifConverter 同策略）。
 */

import { ErrorEvent, MessageEvents, worker } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';

const COMMAND_TIMEOUT_MS: number = 30000;

/**
 * 内置公共 tracker 列表（对齐 animeko/anitorrent 的 builtinTrackers）。
 * 每个新任务自动叠加：源站 magnet 自带的 tracker 常失效或被墙，公开列表是
 * peer 发现的主力通道；tier 0 + announce_to_all_trackers 并行上报，native
 * 侧对已有 URL 去重。追加失败仅告警不阻断（种子自身 tracker 与 DHT 仍可用）。
 */
export const BUILTIN_TRACKERS: string[] = [
  'http://1337.abcvg.info:80/announce',
  'http://bt1.archive.org:6969/announce',
  'http://bt2.archive.org:6969/announce',
  'http://ipv4announce.sktorrent.eu:6969/announce',
  'http://nyaa.tracker.wf:7777/announce',
  'http://torrentsmd.com:8080/announce',
  'http://tracker.bt4g.com:2095/announce',
  'http://tracker.dhitechnical.com:6969/announce',
  'http://tracker.mywaifu.best:6969/announce',
  'http://tracker.renfei.net:8080/announce',
  'http://tracker.waaa.moe:6969/announce',
  'http://tracker810.xyz:11450/announce',
  'http://www.wareztorrent.com:80/announce',
  'https://004430.xyz:443/announce',
  'https://1337.abcvg.info:443/announce',
  'https://ht.therarbg.to:443/announce',
  'https://t.213891.xyz:443/announce',
  'https://tr.abiir.top:443/announce',
  'https://tr.abir.ga:443/announce',
  'https://tr.zukizuki.org:443/announce',
  'https://tracker.7471.top:443/announce',
  'https://tracker.bt4g.com:443/announce',
  'https://tracker.foreverpirates.co:443/announce',
  'https://tracker.gcrenwp.top:443/announce',
  'https://tracker.kuroy.me:443/announce',
  'https://tracker.leechshield.link:443/announce',
  'https://tracker.linvk.com:443/announce',
  'https://tracker.nekomi.cn:443/announce',
  'https://tracker.pmman.tech:443/announce',
  'https://tracker.yemekyedim.com:443/announce',
  'https://tracker.zhuqiy.com:443/announce',
  'https://tracker1.520.jp:443/announce',
  'udp://bittorrent-tracker.e-n-c-r-y-p-t.net:1337/announce',
  'udp://evan.im:6969/announce',
  'udp://explodie.org:6969/announce',
  'udp://ipv6.govt.hu:6969/announce',
  'udp://mail.segso.net:6969/announce',
  'udp://martin-gebhardt.eu:25/announce',
  'udp://ns575949.ip-51-222-82.net:6969/announce',
  'udp://obey.torrentonline.cc:42069/announce',
  'udp://open.demonii.com:1337/announce',
  'udp://open.ftorrent.com:443/announce',
  'udp://open.stealth.si:80/announce',
  'udp://open.tracker.ink:6969/announce',
  'udp://opentor.org:2710/announce',
  'udp://p4p.arenabg.com:1337/announce',
  'udp://retracker.hotplug.ru:2710/announce',
  'udp://t.overflow.biz:6969/announce',
  'udp://torrent.tracker.durukanbal.com:6969/announce',
  'udp://torrentclub.online:1984/announce',
  'udp://torrentclub.online:54123/announce',
  'udp://tr.btube3.com:2010/announce',
  'udp://tr3.ysagin.top:2715/announce',
  'udp://tracker-udp.gbitt.info:80/announce',
  'udp://tracker.0x7c0.com:6969/announce',
  'udp://tracker.aruku.ovh:8081/announce',
  'udp://tracker.auctor.tv:6969/announce',
  'udp://tracker.breizh.pm:6969/announce',
  'udp://tracker.cn.nyaa.net:6969/announce',
  'udp://tracker.corpscorp.online:80/announce',
  'udp://tracker.cynma.tv:6969/announce',
  'udp://tracker.dler.com:6969/announce',
  'udp://tracker.dler.org:6969/announce',
  'udp://tracker.farted.net:6969/announce',
  'udp://tracker.gmi.gd:6969/announce',
  'udp://tracker.ilibr.org:6969/announce',
  'udp://tracker.k.vu:6969/announce',
  'udp://tracker.nexusstream.eu:6969/announce',
  'udp://tracker.nyaa.net:6969/announce',
  'udp://tracker.opentrackr.com:6969/announce',
  'udp://tracker.opentrackr.org:1337/announce',
  'udp://tracker.peerfect.org:6969/announce',
  'udp://tracker.publictracker.xyz:6969/announce',
  'udp://tracker.qu.ax:6969/announce',
  'udp://tracker.skyts.net:6969/announce',
  'udp://tracker.teambelgium.net:6969/announce',
  'udp://tracker.torrent.eu.org:451/announce',
  'udp://whybother.torrentonline.cc:42069/announce',
  'udp://zer0day.ch:1337/announce',
  'wss://tracker.openwebtorrent.com:443/announce'
];

/** torrentStart 的可选参数（字段含义见 types/libtorrent/index.d.ts）。 */
export interface TorrentStartOptions {
  /** 监听规格，默认 "0.0.0.0:0"（随机端口）。 */
  listen?: string;
  /** 是否启用 DHT，默认 true。 */
  enableDht?: boolean;
  /** 会话级下载限速（字节/秒，0 = 不限）。 */
  downloadLimit?: number;
  /** 会话级上传限速（字节/秒，0 = 不限）。 */
  uploadLimit?: number;
  /** 全局连接数上限（0 = libtorrent 默认）。 */
  connectionsLimit?: number;
  /** peer/tracker User-Agent，默认 "Honyomi/1.0"。 */
  userAgent?: string;
}

/** torrentStatus() 返回的快照结构。 */
export interface TorrentStatusJson {
  key: number;
  name: string;
  savePath: string;
  state: string;
  progress: number;
  totalWanted: number;
  totalDone: number;
  downloadRate: number;
  uploadRate: number;
  numPeers: number;
  numSeeds: number;
  numPieces: number;
  piecesDone: number;
  isFinished: boolean;
  isSeeding: boolean;
  hasMetadata: boolean;
  infoHash: string;
  error?: string;
}

/** torrentFiles() 返回的单文件进度。 */
export interface TorrentFileJson {
  index: number;
  path: string;
  size: number;
  done: number;
  progress: number;
}

/** libtorrent alert 事件（键含义见 index.d.ts 的 TorrentAlert）。 */
export interface TorrentAlertJson {
  type: string;
  key: number;
  name?: string;
  numFiles?: number;
  state?: string;
  message?: string;
  file?: string;
}

interface TorrentPendingCall {
  resolve: (result: string | number | boolean) => void;
  reject: (error: Error) => void;
  timer: number;
}

/** worker → 主线程消息的宽松联合形态（按 id/ev 字段判别）。 */
interface TorrentWorkerMessage {
  id?: number;
  ok?: boolean;
  result?: string | number | boolean;
  error?: string;
  ev?: string;
  events?: TorrentAlertJson[];
  statuses?: string[];
}

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
  cmd: 'shutdown' | 'status' | 'files' | 'pause' | 'resume' | 'remove';
  key: number;
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

export class TorrentManager {
  private static torrentWorker: worker.ThreadWorker | null = null;
  private static pending: Map<number, TorrentPendingCall> = new Map<number, TorrentPendingCall>();
  private static nextId: number = 1;
  private static broken: boolean = false;
  private static started: boolean = false;
  private static alertListeners: ((alerts: TorrentAlertJson[]) => void)[] = [];
  private static progressListeners: ((statuses: TorrentStatusJson[]) => void)[] = [];

  /** 引擎是否已 start（且未 shutdown）。 */
  static isStarted(): boolean {
    return TorrentManager.started;
  }

  /** 启动引擎（幂等；重复调用仅覆盖限速等会话参数由 native 决定）。 */
  static async start(downloadDir: string, options?: TorrentStartOptions): Promise<void> {
    const command: TorrentCommandStart = {
      id: TorrentManager.allocId(),
      cmd: 'start',
      downloadDir: downloadDir,
      listen: options?.listen,
      enableDht: options?.enableDht,
      downloadLimit: options?.downloadLimit,
      uploadLimit: options?.uploadLimit,
      connectionsLimit: options?.connectionsLimit,
      userAgent: options?.userAgent
    };
    await TorrentManager.request(command);
    TorrentManager.started = true;
  }

  /** 停止引擎（worker 保留，可再次 start）。 */
  static async shutdown(): Promise<void> {
    if (!TorrentManager.started) {
      return;
    }
    const command: TorrentCommandKeyOp = { id: TorrentManager.allocId(), cmd: 'shutdown', key: 0 };
    try {
      await TorrentManager.request(command);
    } finally {
      TorrentManager.started = false;
    }
  }

  /** 添加 magnet 链接，返回 torrent key。自动叠加内置公共 tracker（peer 发现主力）。 */
  static async addMagnet(magnetUri: string, savePath?: string): Promise<number> {
    const command: TorrentCommandAddMagnet = {
      id: TorrentManager.allocId(),
      cmd: 'addMagnet',
      magnetUri: magnetUri,
      savePath: savePath
    };
    const key = await TorrentManager.requestNumber(command);
    await TorrentManager.appendBuiltinTrackers(key);
    return key;
  }

  /** 添加 .torrent 文件字节，返回 torrent key。同样叠加内置 tracker。 */
  static async addTorrentFile(bytes: Uint8Array, savePath?: string): Promise<number> {
    // 独立缓冲副本（postMessage 转移会 detach 原缓冲）。显式构造：
    // Uint8Array.buffer 的类型是 ArrayBufferLike 联合，无法进入类型化位置。
    const copy: ArrayBuffer = new ArrayBuffer(bytes.byteLength);
    new Uint8Array(copy).set(bytes);
    const command: TorrentCommandAddTorrentFile = {
      id: TorrentManager.allocId(),
      cmd: 'addTorrentFile',
      bytes: copy,
      savePath: savePath
    };
    const key = await TorrentManager.requestNumber(command);
    await TorrentManager.appendBuiltinTrackers(key);
    return key;
  }

  /** 向任务追加额外 tracker（native 侧对已有 URL 去重），返回实际新增数。 */
  static async addTrackers(key: number, trackers: string[]): Promise<number> {
    const command: TorrentCommandAddTrackers = {
      id: TorrentManager.allocId(),
      cmd: 'addTrackers',
      key: key,
      trackers: trackers
    };
    return TorrentManager.requestNumber(command);
  }

  // 内置 tracker 注入失败不阻断添加流程：种子自带 tracker 与 DHT 仍可工作。
  private static async appendBuiltinTrackers(key: number): Promise<void> {
    try {
      await TorrentManager.addTrackers(key, BUILTIN_TRACKERS);
    } catch (e) {
      hilog.warn(0x0000, 'TorrentManager', 'append builtin trackers fail: %{public}s', `${e}`);
    }
  }

  /** 拉取一次状态快照（正常由 onProgress 推送，此为按需查询）。 */
  static async status(key: number): Promise<TorrentStatusJson> {
    const command: TorrentCommandKeyOp = { id: TorrentManager.allocId(), cmd: 'status', key: key };
    const raw = await TorrentManager.requestString(command);
    try {
      return JSON.parse(raw) as TorrentStatusJson;
    } catch (e) {
      hilog.warn(0x0000, 'TorrentManager', 'status parse fail: %{public}s len=%{public}d raw=%{public}s',
        `${e}`, raw.length, raw.substring(0, 300));
      throw new Error(`bt_status_parse: ${e}`);
    }
  }

  /** 拉取文件级进度（metadata 就绪前为空数组）。 */
  static async files(key: number): Promise<TorrentFileJson[]> {
    const command: TorrentCommandKeyOp = { id: TorrentManager.allocId(), cmd: 'files', key: key };
    const raw = await TorrentManager.requestString(command);
    return JSON.parse(raw) as TorrentFileJson[];
  }

  static async pause(key: number): Promise<void> {
    const command: TorrentCommandKeyOp = { id: TorrentManager.allocId(), cmd: 'pause', key: key };
    await TorrentManager.request(command);
  }

  static async resume(key: number): Promise<void> {
    const command: TorrentCommandKeyOp = { id: TorrentManager.allocId(), cmd: 'resume', key: key };
    await TorrentManager.request(command);
  }

  /** 顺序下载（流式阅读友好）。 */
  static async setSequential(key: number, enabled: boolean): Promise<void> {
    const command: TorrentCommandSetSequential = {
      id: TorrentManager.allocId(),
      cmd: 'setSequential',
      key: key,
      enabled: enabled
    };
    await TorrentManager.request(command);
  }

  /** 移除任务，可选删除已下载文件。 */
  static async remove(key: number, deleteFiles: boolean): Promise<void> {
    const command: TorrentCommandRemove = {
      id: TorrentManager.allocId(),
      cmd: 'remove',
      key: key,
      deleteFiles: deleteFiles
    };
    await TorrentManager.request(command);
  }

  /** 更新会话级限速（字节/秒，0 = 不限）。 */
  static async setLimits(downloadLimit: number, uploadLimit: number): Promise<void> {
    const command: TorrentCommandSetLimits = {
      id: TorrentManager.allocId(),
      cmd: 'setLimits',
      downloadLimit: downloadLimit,
      uploadLimit: uploadLimit
    };
    await TorrentManager.request(command);
  }

  /** 更新会话级分享率限制（上传/下载比值，如 2.0；达标后停止优先做种）。 */
  static async setShareRatioLimit(ratio: number): Promise<void> {
    const command: TorrentCommandSetShareRatio = {
      id: TorrentManager.allocId(),
      cmd: 'setShareRatio',
      ratio: ratio
    };
    await TorrentManager.request(command);
  }

  /** 替换会话级 IP 过滤规则（每项 "from-to" 或单个 IP；空列表清除过滤）。 */
  static async setIpFilter(rules: string[]): Promise<void> {
    const command: TorrentCommandSetIpFilter = {
      id: TorrentManager.allocId(),
      cmd: 'setIpFilter',
      rules: rules
    };
    await TorrentManager.request(command);
  }

  /** 订阅 alert 事件（metadata/finished/state/error/removed/...）。 */
  static onAlert(listener: (alerts: TorrentAlertJson[]) => void): void {
    TorrentManager.alertListeners.push(listener);
  }

  /** 订阅 1s 周期的全部任务进度快照。 */
  static onProgress(listener: (statuses: TorrentStatusJson[]) => void): void {
    TorrentManager.progressListeners.push(listener);
  }

  // ---------------- 内部实现 ----------------

  private static allocId(): number {
    const id = TorrentManager.nextId;
    TorrentManager.nextId += 1;
    return id;
  }

  private static request(command: TorrentCommandStart | TorrentCommandAddMagnet | TorrentCommandAddTorrentFile |
    TorrentCommandAddTrackers | TorrentCommandKeyOp | TorrentCommandRemove | TorrentCommandSetSequential |
    TorrentCommandSetLimits | TorrentCommandSetShareRatio | TorrentCommandSetIpFilter):
    Promise<string | number | boolean> {
    return new Promise<string | number | boolean>((resolve, reject) => {
      if (TorrentManager.broken) {
        reject(new Error('torrent_worker_broken'));
        return;
      }
      const threadWorker = TorrentManager.ensureWorker();
      if (threadWorker === null) {
        reject(new Error('torrent_worker_unavailable'));
        return;
      }
      const timer = setTimeout(() => {
        const pending = TorrentManager.pending.get(command.id);
        if (pending !== undefined) {
          TorrentManager.pending.delete(command.id);
          reject(new Error('torrent_command_timeout'));
        }
      }, COMMAND_TIMEOUT_MS);
      TorrentManager.pending.set(command.id, { resolve: resolve, reject: reject, timer: timer });
      if (command.cmd === 'addTorrentFile') {
        threadWorker.postMessage(command, [command.bytes]);
      } else {
        threadWorker.postMessage(command);
      }
    });
  }

  private static async requestNumber(command: TorrentCommandAddMagnet | TorrentCommandAddTorrentFile |
    TorrentCommandAddTrackers): Promise<number> {
    const result = await TorrentManager.request(command);
    if (typeof result !== 'number') {
      throw new Error('torrent_bad_ack');
    }
    return result;
  }

  private static async requestString(command: TorrentCommandKeyOp): Promise<string> {
    const result = await TorrentManager.request(command);
    if (typeof result !== 'string') {
      throw new Error('torrent_bad_ack');
    }
    return result;
  }

  private static ensureWorker(): worker.ThreadWorker | null {
    if (TorrentManager.torrentWorker !== null) {
      return TorrentManager.torrentWorker;
    }
    try {
      // useNormalizedOHMUrl=true 下 HAR 内 worker 仅支持相对路径（相对本文件）。
      const threadWorker = new worker.ThreadWorker('./workers/TorrentWorker.ets');
      threadWorker.onmessage = (e: MessageEvents): void => {
        TorrentManager.onWorkerMessage(e.data);
      };
      threadWorker.onerror = (err: ErrorEvent): void => {
        hilog.warn(0x0000, 'TorrentManager', 'worker error: %{public}s', err.message);
      };
      threadWorker.onexit = (_code: number): void => {
        TorrentManager.broken = true;
        TorrentManager.torrentWorker = null;
        TorrentManager.failAllPending('torrent_worker_exit');
      };
      TorrentManager.torrentWorker = threadWorker;
      return threadWorker;
    } catch (e) {
      hilog.warn(0x0000, 'TorrentManager', 'worker unavailable: %{public}s', `${e}`);
      TorrentManager.broken = true;
      return null;
    }
  }

  private static onWorkerMessage(data: Object): void {
    // 兜底：worker 推送的任意坏消息只记录告警，绝不冒泡为未捕获异常杀死应用。
    try {
      const message = data as TorrentWorkerMessage;
      if (message.id !== undefined) {
        const pending = TorrentManager.pending.get(message.id);
        if (pending === undefined) {
          return;
        }
        TorrentManager.pending.delete(message.id);
        clearTimeout(pending.timer);
        if (message.ok === true) {
          const result: string | number | boolean = message.result === undefined ? false : message.result;
          pending.resolve(result);
        } else {
          pending.reject(new Error(message.error ?? 'torrent_command_failed'));
        }
        return;
      }
      if (message.ev === 'alerts') {
        const events = message.events ?? [];
        for (let i = 0; i < TorrentManager.alertListeners.length; i++) {
          TorrentManager.alertListeners[i](events);
        }
        return;
      }
      if (message.ev === 'progress') {
        const rawStatuses = message.statuses ?? [];
        const statuses: TorrentStatusJson[] = [];
        for (let i = 0; i < rawStatuses.length; i++) {
          try {
            statuses.push(JSON.parse(rawStatuses[i]) as TorrentStatusJson);
          } catch (e) {
            // 单条快照损坏：跳过该条，不影响其余任务进度。
            hilog.warn(0x0000, 'TorrentManager',
              'bad status snapshot: %{public}s len=%{public}d', `${e}`, rawStatuses[i].length);
          }
        }
        for (let i = 0; i < TorrentManager.progressListeners.length; i++) {
          TorrentManager.progressListeners[i](statuses);
        }
      }
    } catch (e) {
      hilog.warn(0x0000, 'TorrentManager', 'bad worker message: %{public}s', `${e}`);
    }
  }

  private static failAllPending(reason: string): void {
    const entries: number[] = [];
    TorrentManager.pending.forEach((_pending, id) => {
      entries.push(id);
    });
    for (let i = 0; i < entries.length; i++) {
      const pending = TorrentManager.pending.get(entries[i]);
      if (pending === undefined) {
        continue;
      }
      TorrentManager.pending.delete(entries[i]);
      clearTimeout(pending.timer);
      pending.reject(new Error(reason));
    }
  }
}
