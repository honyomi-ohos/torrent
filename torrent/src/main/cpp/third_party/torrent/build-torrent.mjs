#!/usr/bin/env node
// ============================================================================
// One-click rebuild of the prebuilt libtorrent-rasterbar static libraries.
//
// Usage:
//   node build-torrent.mjs            rebuild arm64-v8a + x86_64
//   node build-torrent.mjs arm64      rebuild arm64-v8a only
//   node build-torrent.mjs x64        rebuild x86_64 only
//
// Requirements:
//   - DevEco Studio OHOS SDK, located via standard env vars in this order:
//     OHOS_SDK (native SDK dir) / DEVECO_SDK_HOME (SDK root) /
//     OHOS_SDK_HOME (SDK root), falling back to the standard install dir
//   - libtorrent git submodule (git submodule update --init --recursive)
//
// Boost 1.86 headers are fetched automatically by fetch-boost.mjs (cached in
// %TEMP%/mihon-torrent). The vendored OpenSSL prebuilts under prebuilt/ are
// linked as-is; rebuild them with build-openssl.mjs when updating OpenSSL.
//
// Outputs are copied to: prebuilt/<abi>/libtorrent-rasterbar.a
// Only Node.js is required (bundled with the HarmonyOS SDK / DevEco Studio).
// ============================================================================
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const VENDORDIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)));
const WORK = path.join(os.tmpdir(), 'mihon-torrent');
const SRC = path.join(VENDORDIR, 'libtorrent');

const TARGET = (process.argv[2] ?? 'all').toLowerCase();

function die(msg) {
  console.error(`ERROR: ${msg}`);
  process.exit(1);
}

function isSdkDir(p) {
  return fs.existsSync(path.join(p, 'build', 'cmake', 'ohos.toolchain.cmake'));
}

// ---- 1. Locate the OHOS native SDK -----------------------------------------
// Resolution order (standard DevEco conventions, no machine-specific paths):
//   1. OHOS_SDK        - native SDK dir (contains build/cmake/ohos.toolchain.cmake)
//   2. DEVECO_SDK_HOME - DevEco Studio SDK root (contains default/openharmony)
//   3. OHOS_SDK_HOME   - same layout as DEVECO_SDK_HOME
//   4. Standard install locations per platform
function locateSdk() {
  const cands = [];
  if (process.env.OHOS_SDK) cands.push(process.env.OHOS_SDK);
  for (const root of [process.env.DEVECO_SDK_HOME, process.env.OHOS_SDK_HOME]) {
    if (root) cands.push(path.join(root, 'default', 'openharmony', 'native'));
  }
  if (process.platform === 'win32') {
    cands.push(path.join(process.env.ProgramFiles ?? 'C:\\Program Files', 'Huawei', 'DevEco Studio', 'sdk', 'default', 'openharmony', 'native'));
  } else {
    cands.push(path.join(os.homedir(), 'Library', 'OpenHarmony', 'Sdk', 'default', 'openharmony', 'native'));
    cands.push('/Applications/DevEco Studio.app/Contents/sdk/default/openharmony/native');
  }
  for (const c of cands) if (isSdkDir(c)) return c;
  // Fallback probes: the env roots in alternative layouts.
  for (const root of [process.env.DEVECO_SDK_HOME, process.env.OHOS_SDK_HOME]) {
    if (!root) continue;
    for (const sub of ['native', path.join('default', 'openharmony'), '']) {
      const t = path.join(root, sub);
      if (isSdkDir(t)) return t;
    }
  }
  return null;
}

const SDK_REAL = locateSdk();
if (!SDK_REAL) {
  die('OHOS native SDK not found.\n' +
    '       Set DEVECO_SDK_HOME to the DevEco Studio SDK root (the dir containing\n' +
    '       default/openharmony), or OHOS_SDK to the native SDK dir (containing\n' +
    '       build/cmake/ohos.toolchain.cmake), then retry.');
}
console.log(`Using SDK: ${SDK_REAL}`);

// ---- 2. Space-free link to the SDK ------------------------------------------
// The SDK path usually contains spaces ("DevEco Studio"). A junction
// (Windows) / symlink keeps every -D value passed to cmake space-free.
let SDK = SDK_REAL;
if (process.platform === 'win32' || SDK_REAL.includes(' ')) {
  fs.mkdirSync(WORK, { recursive: true });
  const link = path.join(WORK, 'sdk');
  try { fs.rmdirSync(link); } catch { /* not a junction */ }
  try { fs.rmSync(link, { recursive: true, force: true }); } catch { /* nothing there */ }
  fs.symlinkSync(SDK_REAL, link, process.platform === 'win32' ? 'junction' : 'dir');
  const clang = process.platform === 'win32' ? path.join('llvm', 'bin', 'clang.exe') : path.join('llvm', 'bin', 'clang');
  if (!fs.existsSync(path.join(link, clang))) die(`failed to create link ${link} -> ${SDK_REAL}`);
  SDK = link;
}

const ext = process.platform === 'win32' ? '.exe' : '';
let CMAKE = path.join(SDK, 'build-tools', 'cmake', 'bin', 'cmake' + ext);
if (!fs.existsSync(CMAKE)) CMAKE = 'cmake';
let NINJA = path.join(SDK, 'build-tools', 'cmake', 'bin', 'ninja' + ext);
if (!fs.existsSync(NINJA)) NINJA = 'ninja';
const env = { ...process.env, PATH: path.join(SDK, 'llvm', 'bin') + path.delimiter + (process.env.PATH ?? '') };

// ---- 3. Sources --------------------------------------------------------------
if (!fs.existsSync(path.join(SRC, 'CMakeLists.txt'))) {
  die(`libtorrent source not found at ${SRC}\n` +
    '       Run: git submodule update --init --recursive (from the repository root)');
}
const trySignalOk = fs.existsSync(path.join(SRC, 'deps', 'try_signal', 'try_signal.hpp')) ||
  fs.existsSync(path.join(SRC, 'deps', 'try_signal', 'include', 'try_signal', 'try_signal.hpp'));
if (!trySignalOk) {
  die(`libtorrent deps/try_signal submodule not initialized at ${path.join(SRC, 'deps', 'try_signal')}\n` +
    '       Run: git submodule update --init --recursive');
}

// ---- 3b. OHOS sandbox patches -------------------------------------------------
// patches/*.patch hold the libtorrent modifications required by the OHOS app
// sandbox (netlink bind EACCES in ip_notifier, if_indextoname fallback in
// enum_net). Ensure they are applied to the submodule before building;
// already-applied patches are detected and kept.
const PATCHDIR = path.join(VENDORDIR, 'patches');
if (fs.existsSync(PATCHDIR)) {
  const patchFiles = fs.readdirSync(PATCHDIR).filter(p => p.endsWith('.patch')).sort();
  for (const p of patchFiles) {
    const f = path.join(PATCHDIR, p);
    const chk = spawnSync('git', ['-C', SRC, 'apply', '--check', f], { stdio: 'ignore' });
    if (chk.status === 0) {
      const ap = spawnSync('git', ['-C', SRC, 'apply', f], { stdio: 'inherit' });
      if (ap.status !== 0) die(`failed to apply ${p}`);
      console.log(`-> applied patch ${p}`);
    } else {
      const rev = spawnSync('git', ['-C', SRC, 'apply', '--reverse', '--check', f], { stdio: 'ignore' });
      if (rev.status === 0) {
        console.log(`-> patch ${p} already applied`);
      } else {
        die(`cannot apply ${p}, libtorrent source has conflicting changes.\n` +
          `       Run: git -C "${SRC}" checkout -- src`);
      }
    }
  }
}

// ---- 4. Boost headers + OpenSSL prebuilts ------------------------------------
const r = spawnSync(process.execPath, [path.join(VENDORDIR, 'fetch-boost.mjs')], { stdio: 'inherit' });
if (r.status !== 0) die('fetch-boost.mjs failed');
if (!fs.existsSync(path.join(VENDORDIR, 'openssl-include', 'openssl', 'ssl.h'))) {
  die(`${path.join(VENDORDIR, 'openssl-include', 'openssl', 'ssl.h')} missing`);
}

// ---- 5. Build -----------------------------------------------------------------
function run(cmd, args, opts) {
  const res = spawnSync(cmd, args, { stdio: 'inherit', env, ...opts });
  return res.status === 0;
}

function buildAbi(label, arch, subdir) {
  const bdir = path.join(WORK, 'build', `libtorrent-${label}`);
  fs.rmSync(bdir, { recursive: true, force: true });
  console.log(`\n=== libtorrent ${label} (${arch}) ===`);
  // Notes (all verified against the OHOS musl sysroot):
  //   - The OHOS toolchain sets CMAKE_FIND_ROOT_PATH_MODE_* to ONLY, so
  //     find_package(Boost/OpenSSL) never crosses into the sysroot; pass
  //     explicit cache vars instead of *_ROOT_DIR hints.
  //   - _GNU_SOURCE: sync_file_range (fcntl.h) and pthread_setname_np are
  //     only declared under _GNU_SOURCE in musl.
  //   - OHOS musl has no pthread cancellation API; only
  //     boost/core/detail/sp_thread_sleep.hpp references it, shim it to a
  //     no-op macro.
  const ok = run(CMAKE, [
    '-G', 'Ninja', '-S', SRC, '-B', bdir,
    `-DCMAKE_TOOLCHAIN_FILE=${path.join(SDK, 'build', 'cmake', 'ohos.toolchain.cmake')}`,
    `-DOHOS_ARCH=${arch}`, '-DOHOS_PLATFORM=OHOS', '-DOHOS_STL=c++_shared',
    '-DCMAKE_BUILD_TYPE=Release',
    `-DCMAKE_MAKE_PROGRAM=${NINJA}`,
    '-DBUILD_SHARED_LIBS=OFF',
    '-Dstatic_runtime=OFF', '-Ddeprecated-functions=ON',
    '-DCMAKE_CXX_FLAGS=-D_GNU_SOURCE -Dpthread_setcancelstate(a,b)=((void)0)',
    `-DBoost_INCLUDE_DIR=${path.join(VENDORDIR, 'deps', 'boost-1.86')}`,
    '-DBoost_NO_SYSTEM_PATHS=ON',
    `-DOPENSSL_INCLUDE_DIR=${path.join(VENDORDIR, 'openssl-include')}`,
    `-DOPENSSL_CRYPTO_LIBRARY=${path.join(VENDORDIR, 'prebuilt', subdir, 'libcrypto.a')}`,
    `-DOPENSSL_SSL_LIBRARY=${path.join(VENDORDIR, 'prebuilt', subdir, 'libssl.a')}`
  ]);
  if (!ok) die(`cmake configure failed for ${label}`);
  if (!run(NINJA, ['-C', bdir])) die(`build failed for ${label}`);
  const a = path.join(bdir, 'libtorrent-rasterbar.a');
  if (!fs.existsSync(a)) die(`${a} not produced`);
  fs.copyFileSync(a, path.join(VENDORDIR, 'prebuilt', subdir, 'libtorrent-rasterbar.a'));
  const dst = path.join(VENDORDIR, 'prebuilt', subdir, 'libtorrent-rasterbar.a');
  console.log(`-> prebuilt/${subdir}/libtorrent-rasterbar.a  ${fs.statSync(dst).size} bytes`);
}

switch (TARGET) {
  case 'arm64': buildAbi('arm64', 'arm64-v8a', 'arm64-v8a'); break;
  case 'x64': buildAbi('x64', 'x86_64', 'x86_64'); break;
  case 'all':
    buildAbi('arm64', 'arm64-v8a', 'arm64-v8a');
    buildAbi('x64', 'x86_64', 'x86_64');
    break;
  default:
    console.error('usage: node build-torrent.mjs [arm64|x64|all]');
    process.exit(2);
}

console.log(`\nDone. Updated prebuilt/libtorrent-rasterbar.a for target(s): ${TARGET}`);
console.log('Rebuild the HAP to pick up the new binaries.');
