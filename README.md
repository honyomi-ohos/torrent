# honyomi-torrent

Honyomi 的 BT 下载引擎模块：libtorrent-rasterbar 2.0.15 + OpenSSL 3.5.9（双 ABI 预编译静态库），
经 NAPI 暴露为 `libtorrent.so`，ArkTS 侧封装为 `TorrentManager`（worker 线程内跑 libtorrent session）。

本仓库是**独立 HarmonyOS 工程**，产出**集成态 HSP**（`integratedHsp: true`）的 `.tgz` 包；
宿主应用（mihon）经 `entry/libs/torrent-default.tgz` 依赖消费，随宿主包按需分发
（`deliveryWithInstall: false` + `moduleInstallManager`）。

## 构建

前置：已安装 DevEco Studio 并设置 `DEVECO_HOME` 环境变量。

```powershell
node build.mjs
```

产物：`torrent/build/**.tgz`，自动拷贝为 `../mihon/entry/libs/torrent-default.tgz`。

## 与宿主（mihon）的同步纪律

集成态 HSP 集成时由工具链自动替换 bundleName 并重签名，但以下字段**必须与宿主保持一致**，
否则安装报 10024：

| 字段 | 本仓库位置 | mihon 位置 |
|---|---|---|
| versionCode | `AppScope/app.json5` | `AppScope/app.json5` |
| minAPIVersion / compatibleSdkVersion | 根 `build-profile.json5` products | 根 `build-profile.json5` products |

任一侧调整后须同步另一侧。

## 模块结构

- `torrent/src/main/ets/TorrentManager.ets` — 主线程门面（Promise 封装 + 1s 进度/告警推送）
- `torrent/src/main/ets/workers/TorrentWorker.ets` — worker 线程（libtorrent session 轮询）
- `torrent/src/main/cpp/torrent_bridge.cpp` — NAPI 桥（JSON 协议）
- `torrent/src/main/cpp/third_party/` — libtorrent/OpenSSL 预编译静态库与源码

## 按需分发说明

- `module.json5`：`type: shared` + `deliveryWithInstall: false` — 应用市场首装不含本模块，
  首次使用 BT 时宿主经 `moduleInstallManager`（`@kit.AppGalleryKit`）下载安装，再动态
  `import('torrent')` 加载。
- 模块名固定为 `torrent`（module.json5 `name`），宿主 `moduleInstallManager` 接口以该名为准。
