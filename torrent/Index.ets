/**
 * @honyomi/torrent — BT 下载能力（HAR）。
 *
 * 用法：
 *   import { TorrentManager } from '@honyomi/torrent';
 *   await TorrentManager.start(downloadDir);
 *   const key = await TorrentManager.addMagnet(magnetUri);
 */
export { TorrentManager } from './src/main/ets/TorrentManager';
export { TorrentStartOptions, TorrentStatusJson, TorrentFileJson, TorrentAlertJson } from './src/main/ets/TorrentManager';
