# honyomi-torrent

Honyomi 的 BT 下载引擎模块：libtorrent-rasterbar 2.0.15 + OpenSSL 3.5.9（双 ABI 预编译静态库），
经 NAPI 暴露为 `libtorrent.so`，ArkTS 侧封装为 `TorrentManager`（worker 线程内跑 libtorrent session）。

本仓库以 **git submodule** 形式集成进宿主工程 [mihon](https://github.com/ShueyYuen/honyomi)，
作为**工程内 HSP**（应用内 HSP）随宿主一同构建签名：与宿主同 bundleName、同签名配置，
构建模式（debug/release）与 ABI 过滤跟随各自模块的 `build-profile.json5`：

- debug：`arm64-v8a` + `x86_64`（x86 模拟器可调试）
- release：仅 `arm64-v8a`（应用市场发布包）

按需分发：`module.json5` 的 `type: shared` + `deliveryWithInstall: false` 保持不变，
宿主经 `moduleInstallManager` 首次使用 BT 时下载安装本模块。

## 宿主集成方式（mihon 侧）

```text
git submodule add https://github.com/honyomi-ohos/torrent.git torrent
git submodule update --init --recursive
```

工程 `build-profile.json5` 的 `modules` 注册 `./torrent/torrent`，
entry 的 `oh-package.json5` 经 `dynamicDependencies` 以 `file:../torrent/torrent` 引用。

## libtorrent OHOS 补丁

libtorrent git 子模块（`torrent/src/main/cpp/third_party/torrent/libtorrent`，指向
arvidn/libtorrent v2.0.15）之上需要 2 个 OHOS 沙箱兼容补丁，已导出为
`torrent/src/main/cpp/patches/libtorrent-ohos.patch`（约 100 行）：

- `src/enum_net.cpp` — OHOS 沙箱禁止 `SIOCGIFNAME` ioctl 时，回退用 RTM_GETLINK
  dump 映射路由出接口名，否则 `has_internet_route()` 失败导致 tracker 无法通告。
- `src/ip_notifier.cpp` — OHOS 沙箱拒绝 netlink 组播 bind（EACCES），改为显式
  error_code 打开/绑定，失败时降级为 no-op notifier，避免异常逃逸炸掉整个 session。

**初始化子模块后必须应用补丁**（已应用则跳过）：

```powershell
# PowerShell（在 torrent 仓库根执行）
git -C torrent/src/main/cpp/third_party/torrent/libtorrent `
  apply ../../../patches/libtorrent-ohos.patch
```

```bash
# bash
git -C torrent/src/main/cpp/third_party/torrent/libtorrent \
  apply ../../../patches/libtorrent-ohos.patch
```

重复应用报错即说明已应用。补丁更新时重新导出覆盖该文件并提交。

## 模块结构

- `torrent/src/main/ets/TorrentManager.ets` — 主线程门面（Promise 封装 + 1s 进度/告警推送）
- `torrent/src/main/ets/workers/TorrentWorker.ets` — worker 线程（libtorrent session 轮询）
- `torrent/src/main/cpp/torrent_bridge.cpp` — NAPI 桥（JSON 协议）
- `torrent/src/main/cpp/third_party/` — libtorrent 源码 / OpenSSL 预编译静态库
- `torrent/src/main/cpp/patches/` — libtorrent OHOS 兼容补丁
