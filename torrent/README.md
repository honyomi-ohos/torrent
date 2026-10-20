# @honyomi/torrent

Honyomi 的 BT 下载能力模块（HAR）：libtorrent-rasterbar 2.0.15 + OpenSSL 3.5.9（双 ABI 预编译静态库）经 NAPI 暴露为独立模块 `libtorrent.so`。

## 结构

- `src/main/cpp/torrent_bridge.cpp` — NAPI 桥（`torrentStart` / `torrentAddMagnet` / … 共 13 个导出，独立 napi_module 注册）
- `src/main/cpp/third_party/torrent/` — 预编译静态库、构建/重建脚本、libtorrent submodule（重编说明见其 `README.md`）
- `src/main/ets/workers/TorrentWorker.ets` — 引擎线程（1s 轮询 alerts 与进度快照，所有 torrent* NAPI 调用都在此线程）
- `src/main/ets/TorrentManager.ets` — 主线程门面（Promise 封装 + `onAlert`/`onProgress` 事件订阅）
- `src/main/cpp/types/libtorrent/` — `libtorrent.so` 的 TS 类型声明

## 构建说明

- Boost 1.86 头文件（~140MB，gitignored）在 CMake configure 阶段自动下载到
  `src/main/cpp/third_party/torrent/deps/boost-1.86`。查找顺序：
  `-DTORRENT_BOOST_TARBALL=<路径>` 显式覆盖 → `deps/downloads/` 已有压缩包 →
  fetch-boost 缓存（`%TEMP%/mihon-torrent/downloads`，macOS/Linux 为 `$TMPDIR`）→ 在线下载。
  离线环境可预先运行 `third_party/torrent/fetch-boost.mjs`。
- ABI：debug 构建 arm64-v8a + x86_64，release 仅 arm64-v8a（见 `build-profile.json5`）。
- libtorrent/OpenSSL 静态库重建：见 `src/main/cpp/third_party/torrent/README.md`。

## 使用（工程内）

```json5
// entry/oh-package.json5
"dependencies": {
  "@honyomi/torrent": "file:../torrent"
}
```

```ts
import { TorrentManager } from '@honyomi/torrent';

await TorrentManager.start(downloadDir);
const key = await TorrentManager.addMagnet(magnetUri);
TorrentManager.onProgress((statuses) => { /* 1s 快照 */ });
TorrentManager.onAlert((alerts) => { /* metadata/finished/error/... */ });
```
