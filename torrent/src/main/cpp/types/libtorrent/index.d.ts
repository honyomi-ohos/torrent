/*
 * Copyright (c) 2026 honyomi
 *
 * Typed surface of libtorrent.so — the BitTorrent engine NAPI module
 * (libtorrent-rasterbar 2.0.15 + OpenSSL 3.5.9). All functions are meant
 * to be called from the dedicated TorrentWorker thread, never from the
 * UI thread (see workers/TorrentWorker.ets).
 */

// ============================================================================
// Torrent engine (drive from TorrentWorker.ets)
// ============================================================================

/** Shape of the JSON string returned by torrentStatus(). */
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

/** Entry of the JSON array returned by torrentFiles(). */
export interface TorrentFileJson {
  index: number;
  path: string;
  size: number;
  done: number;
  progress: number;
}

/** Event shapes of the JSON array returned by torrentPollAlerts(). */
export type TorrentAlert =
  | { type: 'metadata'; key: number; name: string; numFiles: number }
  | { type: 'finished'; key: number }
  | { type: 'state'; key: number; state: string }
  | { type: 'error'; key: number; message: string }
  | { type: 'fileError'; key: number; file: string; message: string }
  | { type: 'removed'; key: number }
  | { type: 'trackerError'; key: number; url: string; message: string; timesInRow: number }
  | { type: 'trackerReply'; key: number; url: string; peers: number }
  | { type: 'peerConnect'; key: number; endpoint: string }
  | { type: 'peerDisconnect'; key: number; message: string }
  | { type: 'listen'; address: string; port: number; socket: string }
  | { type: 'listenFailed'; interface: string; port: number; message: string }
  | { type: 'dhtReady' }
  | { type: 'sessionError'; message: string }
  | { type: 'alert'; message: string };

/**
 * Starts the process-wide libtorrent session (idempotent). Call once from
 * TorrentWorker before any other torrent* call.
 * @param downloadDir default save directory (app sandbox path)
 * @param listenInterfaces listen spec, default "0.0.0.0:0" (ephemeral port)
 * @param enableDht enable Mainline DHT (default true)
 * @param downloadLimit session download limit in bytes/s (0 = unlimited)
 * @param uploadLimit session upload limit in bytes/s (0 = unlimited)
 * @param connectionsLimit global connection cap (0 = libtorrent default)
 * @param userAgent peer/tracker user agent (default "Honyomi/1.0")
 * @throws Error when downloadDir is empty
 */
export function torrentStart(downloadDir: string, listenInterfaces?: string, enableDht?: boolean,
  downloadLimit?: number, uploadLimit?: number, connectionsLimit?: number, userAgent?: string): boolean;

/** Stops the session and joins its network/disk threads. Idempotent. */
export function torrentShutdown(): boolean;

/**
 * Adds a magnet URI. Returns a torrent key used by all other torrent calls.
 * @throws Error when the URI is invalid or the session is not started
 */
export function torrentAddMagnet(magnetUri: string, savePath?: string): number;

/**
 * Adds a .torrent file from its raw bytes. Returns a torrent key.
 * @throws Error when the buffer is not a valid bencoded torrent
 */
export function torrentAddTorrentFile(bytes: ArrayBuffer, savePath?: string): number;

/**
 * Appends extra tracker URLs to a torrent (anitorrent/animeko style).
 * All trackers land in tier 0 with no fail limit and are announced in
 * parallel (announce_to_all_trackers is enabled at torrentStart); URLs the
 * torrent already tracks (magnet tr= entries) are skipped.
 * @param key torrent key
 * @param trackers JSON array of tracker URLs,
 *   e.g. ["udp://tracker.opentrackr.org:1337/announce"]
 * @returns number of trackers actually added
 * @throws Error when trackers is not a JSON array of strings
 */
export function torrentAddTrackers(key: number, trackers: string): number;

/** Current status snapshot of one torrent (JSON-encoded TorrentStatusJson). */
export function torrentStatus(key: number): string;

/** File list with byte progress (JSON-encoded TorrentFileJson[]; "[]" before metadata). */
export function torrentFiles(key: number): string;

/** Enables/disables sequential (streaming-friendly) piece download order. */
export function torrentSetSequential(key: number, enabled: boolean): boolean;

/** Pauses a torrent (explicit; auto-manage is disabled at add time). */
export function torrentPause(key: number): boolean;

/** Resumes a paused torrent. */
export function torrentResume(key: number): boolean;

/** Removes a torrent, optionally deleting downloaded files. */
export function torrentRemove(key: number, deleteFiles: boolean): boolean;

/** Updates session-wide rate limits in bytes/s (0 = unlimited). */
export function torrentSetLimits(downloadLimit: number, uploadLimit: number): boolean;

/**
 * Sets the session-wide seed share ratio limit (upload/download as double,
 * e.g. 2.0 = stop prioritizing a torrent after uploading twice what was
 * downloaded; >= 1000 is effectively unlimited).
 */
export function torrentSetShareRatioLimit(ratio: number): boolean;

/**
 * Replaces the session-wide IP filter (peer blocklist).
 * @param rules JSON array of strings; each entry is "from-to" (inclusive
 *   range) or a single address ("ip" blocks exactly that address)
 * @returns number of rules applied
 * @throws Error on malformed JSON or invalid/mixed-family addresses
 */
export function torrentSetIpFilter(rules: string): number;

/** Drains pending libtorrent alerts (JSON-encoded TorrentAlert[]). */
export function torrentPollAlerts(maxCount?: number): string;

/** Current DHT node count (0 when DHT is off or the session is stopped). */
export function torrentSessionDhtNodes(): number;
