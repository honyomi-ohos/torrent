# third_party/torrent

Vendored BitTorrent 引擎栈，供 `torrent_bridge.cpp`（NAPI `torrentStart` /
`torrentAddMagnet` / ... 共 13 个 `torrent*` 导出）使用，由
`TorrentWorker.ets` 独立线程驱动。

| 组件 | 版本 | 来源 | 许可证 |
|---|---|---|---|
| libtorrent-rasterbar（git submodule） | v2.0.15 | https://github.com/arvidn/libtorrent | BSD（见 `prebuilt/libtorrent-LICENSE`） |
| OpenSSL（仅预编译静态库 + 头文件入库） | 3.5.9 | https://www.openssl.org | Apache-2.0（见 `prebuilt/openssl-LICENSE.txt`） |
| Boost（仅头文件，不入库，按需下载） | 1.86.0 | https://www.boost.org | BSL-1.0（见 `prebuilt/boost-LICENSE_1_0.txt`） |

获取全部源码：`git submodule update --init --recursive`（libtorrent 内嵌
`deps/try_signal` 子模块，libtorrent 构建必需）。

## patches/ — OHOS 沙箱兼容补丁

libtorrent 源内两处针对 OHOS 应用沙箱的必要修改，以 `.patch` 文件维护
（submodule 保持干净，补丁由构建脚本在编译前检查并自动应用）：

| 补丁 | 修复 |
|---|---|
| `0001-ip-notifier-ohos-tolerate-eacces.patch` | 沙箱拒绝 netlink 组播 bind（EACCES），上游 asio socket 构造即抛异常导致 session 无法创建。改为显式 `error_code` open/bind，失败降级为 no-op 通知器 |
| `0002-enum-net-ohos-route-name-fallback.patch` | 沙箱禁止 `if_indextoname()` 依赖的 `SIOCGIFNAME` ioctl，路由无设备名 → `has_internet_route()` 失败 → tracker announce 被阻断。先 `RTM_GETLINK` 收集接口表，ioctl 失败时按 if_index 映射兜底 |

`build-torrent.mjs` 在构建前逐
个 `git apply --check`：未应用则应用，已应用（`--reverse --check` 通过）
则跳过，源码有冲突改动则报错退出。升级 libtorrent 版本时需按冲突提示
重做补丁。

## prebuilt/ — 预编译静态库

- libtorrent v2.0.15：`Release` + `BUILD_SHARED_LIBS=OFF`，
  `deprecated-functions=ON`，`static_runtime=OFF`，链接 vendored OpenSSL
  静态库（`TORRENT_USE_OPENSSL`/`TORRENT_SSL_PEERS`）。
- OpenSSL 3.5.9：`no-shared no-tests no-docs`，`linux-aarch64` /
  `linux-x86_64` 目标，OHOS musl sysroot（`-D__MUSL__`）。
- musl 兼容（详见 build 脚本内注释）：
  - `-D_GNU_SOURCE` — musl 的 `sync_file_range`（fcntl.h）与
    `pthread_setname_np` 仅在 `_GNU_SOURCE` 下声明。
  - `-Dpthread_setcancelstate(a,b)=((void)0)` — OHOS musl 无 pthread
    取消 API；全 Boost 1.86 仅 `boost/core/detail/sp_thread_sleep.hpp`
    引用（`PTHREAD_CANCEL_*` 常量存在）。
- OHOS toolchain 设 `CMAKE_FIND_ROOT_PATH_MODE_*` 为 ONLY，
  `find_package` 无法跨入 sysroot，必须传显式 cache 变量
  （`Boost_INCLUDE_DIR` / `OPENSSL_INCLUDE_DIR` / `OPENSSL_CRYPTO_LIBRARY`
  / `OPENSSL_SSL_LIBRARY`），`*_ROOT_DIR` 提示无效。

| 文件 | 大小 | SHA256 |
|---|---|---|
| `prebuilt/arm64-v8a/libtorrent-rasterbar.a` | 31,428,372 | `3DD47CE6E721A15E5A8820023BE066F9F9B33964674EA82B040775F6C0CD4029` |
| `prebuilt/arm64-v8a/libcrypto.a` | 10,352,702 | `0D301AA123DD394B3382D41A714F9D203245342DEE42815EFFBE9E64CF4B62BE` |
| `prebuilt/arm64-v8a/libssl.a` | 1,875,178 | `D22D644835D0F9A63857433BE5C572014C7C43377529BA0F091B6EA6B5E5368B` |
| `prebuilt/x86_64/libtorrent-rasterbar.a` | 29,703,620 | `6A5DF85818A69651C61C15655EB8B2E9D47F88E2B80E1ADB8987B9B1913A619A` |
| `prebuilt/x86_64/libcrypto.a` | 10,543,786 | `BD345C8ACC168701B1E67384ED341BA658EA6591943EC26B3550DC3BED29BA90` |
| `prebuilt/x86_64/libssl.a` | 1,918,874 | `6955CBD1B5BD78F53849EE4718299D2C6E33ED5D3DAFB46C624C0EDE62BE0A23` |

> 静态库内 ar 元数据不完全确定，重编产物 SHA256 可能微差（尺寸基本一
> 致），属正常。

## openssl-include/ — OpenSSL 3.5.9 公共头文件

与 prebuilt 库同源（arm64 构建树 `include/openssl/`，142 个头文件），
双 ABI 通用；libtorrent 源内 include 的 OpenSSL 头以这里为准。

## deps/ — Boost 1.86 头文件（gitignored，~140MB）

`torrent_bridge.cpp` 与 libtorrent 均需 Boost 头（header-only 使用）。
`fetch-boost.mjs` 从
`github.com/boostorg/boost` release 下载
`boost-1.86.0-b2-nodocs.tar.gz`（缓存在 `%TEMP%\mihon-torrent`），仅提
取 `boost/` 头到 `deps/boost-1.86/boost`。构建 HAP 前必须先跑
（`CMakeLists.txt` 缺失时报 FATAL_ERROR 提示）。

## 一键重编

```
node build-torrent.mjs               # 重编 libtorrent 双 ABI，覆盖 prebuilt/ 下 libtorrent-rasterbar.a
node build-torrent.mjs arm64         # 仅 arm64-v8a
node build-torrent.mjs x64           # 仅 x86_64
node build-openssl.mjs               # 重编 OpenSSL 双 ABI，覆盖 prebuilt/ 下 libcrypto.a / libssl.a 及 openssl-include/
node build-openssl.mjs arm64         # 仅 arm64-v8a
```

- 全部脚本为跨平台 Node.js（DevEco Studio 自带 Node），macOS / Linux /
  Windows 行为一致。
- SDK 定位：`OHOS_SDK` → `DEVECO_SDK_HOME` → `OHOS_SDK_HOME` → 默认
  安装位置。
- OpenSSL 预编译库重编（仅在升级 OpenSSL 时需要）：Windows 上脚本自动
  引导 mini-MSYS（bash + perl + make，无需 MSYS2 安装），macOS / Linux
  使用系统 perl + make。脚本会逐成员校验产物架构（arm64 → `EM_AARCH64`，
  x64 → `EM_X86_64`）后覆盖 `prebuilt/`。
- `build-openssl.mjs` 引导的 mini-MSYS 使用固定版本的 MSYS2 包
  （msys2-runtime 3.6.11-4、bash 5.3.20、perl 5.42.2、make 4.4.1、
  coreutils 8.32 等，pinned 列表见脚本内），缓存于
  `%TEMP%\mihon-torrent\{downloads,msys}`（`$TMPDIR/mihon-torrent`）；
  `--rebuild-msys` 可强制重建。
- 构建缓存在 `%TEMP%\mihon-torrent`（`$TMPDIR/mihon-torrent`），可随时
  删除。

重编后需重新构建 HAP 才会生效。
