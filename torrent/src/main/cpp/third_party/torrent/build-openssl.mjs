#!/usr/bin/env node
// ============================================================================
// One-click rebuild of the vendored OpenSSL 3.5.9 prebuilt static libraries
// (prebuilt/<abi>/libcrypto.a + libssl.a) and the openssl-include/ headers.
//
// Usage:
//   node build-openssl.mjs            # arm64 + x64
//   node build-openssl.mjs arm64      # arm64-v8a only
//   node build-openssl.mjs x64        # x86_64 only
//   node build-openssl.mjs --rebuild-msys   # wipe + re-bootstrap the mini-MSYS
//
// Requirements:
//   - DevEco Studio OHOS SDK (same resolution chain as build-torrent.mjs)
//   - Windows: nothing else (a pinned mini-MSYS with bash/perl/make is
//     bootstrapped into %TEMP%\mihon-torrent\msys on first run)
//   - macOS / Linux: system perl + make
//
// Only needed when upgrading OpenSSL; libtorrent links the prebuilts as-is.
// Outputs are copied to prebuilt/<abi>/ and openssl-include/.
// Only Node.js is required (bundled with the HarmonyOS SDK / DevEco Studio).
// ============================================================================
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import https from 'node:https';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const VENDORDIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)));
const WORK = path.join(os.tmpdir(), 'mihon-torrent');
const DOWNLOADS = path.join(WORK, 'downloads');
const MSYS = path.join(WORK, 'msys');
const VERSION = '3.5.9';
const TARBALL = `openssl-${VERSION}.tar.gz`;

const argv = process.argv.slice(2);
const REBUILD_MSYS = argv.includes('--rebuild-msys');
const TARGET = (argv.find(a => !a.startsWith('--')) ?? 'all').toLowerCase();

// Pinned MSYS2 packages (https://repo.msys2.org/msys/x86_64/, all verified).
// Keep in sync with the versions documented in README.md.
const MSYS_PACKAGES = [
  'msys2-runtime-3.6.11-4',
  'gcc-libs-15.3.0-1',
  'bash-5.3.20-2',
  'coreutils-8.32-5',
  'sed-4.9-1',
  'make-4.4.1-2',
  'perl-5.42.2-1',
  'libxcrypt-4.4.38-1',
  'libcrypt-2.1-5',
  'libiconv-1.19-1',
  'libintl-0.22.5-1'
];

function die(msg) {
  console.error(`ERROR: ${msg}`);
  process.exit(1);
}

function isSdkDir(p) {
  return fs.existsSync(path.join(p, 'build', 'cmake', 'ohos.toolchain.cmake'));
}

// Same resolution chain as build-torrent.mjs.
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
  for (const root of [process.env.DEVECO_SDK_HOME, process.env.OHOS_SDK_HOME]) {
    if (!root) continue;
    for (const sub of ['native', path.join('default', 'openharmony'), '']) {
      const t = path.join(root, sub);
      if (isSdkDir(t)) return t;
    }
  }
  return null;
}

// Download via Node https with redirect following.
function downloadViaHttps(url, dst, redirects) {
  return new Promise((resolve, reject) => {
    if (redirects > 5) return reject(new Error('too many redirects'));
    const req = https.get(url, res => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        res.resume();
        return resolve(downloadViaHttps(new URL(res.headers.location, url).toString(), dst, redirects + 1));
      }
      if (res.statusCode !== 200) {
        res.resume();
        return reject(new Error(`HTTP ${res.statusCode} for ${url}`));
      }
      const out = fs.createWriteStream(dst);
      res.pipe(out);
      out.on('error', reject);
      out.on('finish', () => out.close(() => resolve()));
    });
    req.on('error', err => {
      try { fs.unlinkSync(dst); } catch { /* not created */ }
      reject(err);
    });
  });
}

// Prefer curl (ships with Windows 10+, macOS and Linux; more robust than
// Node's https on flaky networks where some CDN edges time out).
async function download(url, dst) {
  const hasCurl = spawnSync('curl', ['--version'], { stdio: 'ignore' }).status === 0;
  if (hasCurl) {
    const r = spawnSync('curl', ['-L', '--fail', '--retry', '3', '--connect-timeout', '30', '-o', dst, url], { stdio: 'inherit' });
    if (r.status === 0) return;
    try { fs.unlinkSync(dst); } catch { /* nothing there */ }
    throw new Error(`curl exited with ${r.status} for ${url}`);
  }
  await downloadViaHttps(url, dst, 0);
}

// C:\foo\bar -> /c/foo/bar (for paths used inside the mini-MSYS bash).
function toPosix(p) {
  const fwd = p.replace(/\\/g, '/');
  const m = /^([A-Za-z]):(.*)$/.exec(fwd);
  return m ? `/${m[1].toLowerCase()}${m[2]}` : fwd;
}

function sha256Of(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex').toUpperCase();
}

// Verify every ar archive member matches the expected ELF machine, one by one
// in pure Node (a polluted build tree once produced an archive with
// mixed-arch members). Handles GNU ("/" symbol table, "//" long-name table,
// "/N" long-name references) and BSD ("__.SYMDEF", "#1/N" inline names)
// ar formats; expectMachine is the ELF e_machine value (EM_AARCH64 = 183,
// EM_X86_64 = 62).
function verifyMembers(archive, expectMachine) {
  const buf = fs.readFileSync(archive);
  if (buf.length < 8 || buf.toString('latin1', 0, 8) !== '!<arch>\n') {
    console.error(`  BAD: ${archive} is not an ar archive`);
    return false;
  }
  const longnames = [];
  let off = 8;
  let total = 0;
  let bad = 0;
  while (off + 60 <= buf.length) {
    const hdr = buf.toString('latin1', off, off + 60);
    if (hdr.length < 60 || hdr.charCodeAt(58) !== 0x60 || hdr.charCodeAt(59) !== 0x0a) break;
    const rawName = hdr.substring(0, 16);
    const size = parseInt(hdr.substring(48, 58).trim(), 10);
    if (!Number.isFinite(size)) break;
    let data = buf.subarray(off + 60, off + 60 + size);
    off = off + 60 + size + (size % 2);

    if (rawName.startsWith('//')) {
      // GNU long-name table: entries separated by '/\n'.
      const table = data.toString('latin1');
      let start = 0;
      for (let i = 0; i < table.length - 1; i++) {
        if (table[i] === '/' && table[i + 1] === '\n') {
          longnames.push({ off: start, name: table.substring(start, i) });
          start = i + 2;
          i++;
        }
      }
      continue;
    }

    let member;
    const gnuRef = /^\/(\d+)/.exec(rawName);       // "/N": long-name reference
    const bsdInline = /^#1\/(\d+)/.exec(rawName);  // "#1/N": BSD inline name
    if (gnuRef) {
      const hit = longnames.find(e => e.off === Number(gnuRef[1]));
      member = hit ? hit.name : rawName.trim();
    } else if (bsdInline) {
      const n = Math.min(Number(bsdInline[1]), data.length);
      member = data.toString('latin1', 0, n).replace(/\0[\s\S]*$/, '');
      data = data.subarray(n);
    } else if (rawName.trim() === '/' || rawName.startsWith('/SYM64') || rawName.startsWith('__.SYMDEF')) {
      continue; // symbol tables
    } else {
      member = rawName.replace(/\/$/, '').trim();
    }

    total++;
    const isElf = data.length > 20 && data[0] === 0x7f && data[1] === 0x45 && data[2] === 0x4c && data[3] === 0x46;
    if (!isElf || data.readUInt16LE(18) !== expectMachine) {
      console.log(`  BAD member: ${member}`);
      bad++;
    }
  }
  console.log(`  ${path.basename(archive)}: ${total} members checked, bad: ${bad}`);
  return total > 0 && bad === 0;
}

function run(cmd, args, opts) {
  const res = spawnSync(cmd, args, { stdio: 'inherit', ...opts });
  return res.status === 0;
}

// ---- 1. Locate the OHOS native SDK ------------------------------------------
const SDK_REAL = locateSdk();
if (!SDK_REAL) {
  die('OHOS native SDK not found. Set DEVECO_SDK_HOME (SDK root containing default/openharmony) or OHOS_SDK (native SDK dir).');
}
console.log(`Using SDK: ${SDK_REAL}`);

// ---- 2. Space-free link to the SDK ------------------------------------------
// CC is word-split by make, so a sysroot path with spaces ("DevEco Studio")
// must be hidden behind a space-free junction (Windows) / symlink (POSIX).
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

fs.mkdirSync(DOWNLOADS, { recursive: true });
const JOBS = os.cpus().length;

// ---- 3. Windows: bootstrap the pinned mini-MSYS root -------------------------
let bashExe = null;
if (process.platform === 'win32') {
  bashExe = path.join(MSYS, 'usr', 'bin', 'bash.exe');
  if (REBUILD_MSYS && fs.existsSync(MSYS)) fs.rmSync(MSYS, { recursive: true, force: true });
  if (!fs.existsSync(bashExe)) {
    console.log(`Bootstrapping mini-MSYS at ${MSYS} ...`);
    fs.mkdirSync(MSYS, { recursive: true });
    for (const pkg of MSYS_PACKAGES) {
      const file = `${pkg}-x86_64.pkg.tar.zst`;
      const dst = path.join(DOWNLOADS, file);
      if (!fs.existsSync(dst)) {
        console.log(`  downloading ${file}`);
        try {
          await download(`https://repo.msys2.org/msys/x86_64/${file}`, dst, 0);
        } catch (err) {
          die(`failed to download ${file}: ${err.message}`);
        }
      }
      // System32 tar.exe (bsdtar) handles .tar.zst.
      if (!run('tar', ['-xf', dst, '-C', MSYS])) die(`failed to extract ${file}`);
    }
  }
  if (!fs.existsSync(bashExe)) die(`bash.exe not found at ${bashExe} after bootstrap`);
  // msys2-runtime automounts drives under /cygdrive when /etc/fstab is
  // absent; force the standard MSYS2 "/" prefix so the /c/... POSIX paths
  // used below resolve, and provide /tmp (silences a bash startup warning).
  const fstab = path.join(MSYS, 'etc', 'fstab');
  if (!fs.existsSync(fstab)) {
    fs.mkdirSync(path.dirname(fstab), { recursive: true });
    fs.writeFileSync(fstab, 'none / cygdrive binary,posix=0 0 0\n');
  }
  fs.mkdirSync(path.join(MSYS, 'tmp'), { recursive: true });
} else {
  // macOS / Linux: system perl + make required.
  for (const t of ['perl', 'make']) {
    if (spawnSync(t, ['--version'], { stdio: 'ignore' }).status !== 0) die(`${t} not found in PATH`);
  }
}

// ---- 4. Download the OpenSSL source (cached) ---------------------------------
const TGZ = path.join(DOWNLOADS, TARBALL);
if (!fs.existsSync(TGZ)) {
  console.log(`Downloading OpenSSL ${VERSION} ...`);
  const urls = [
    `https://github.com/openssl/openssl/releases/download/openssl-${VERSION}/${TARBALL}`,
    `https://www.openssl.org/source/${TARBALL}`
  ];
  let ok = false;
  for (const u of urls) {
    try {
      await download(u, TGZ, 0);
      ok = true;
      break;
    } catch { /* try next mirror */ }
  }
  if (!ok) die(`failed to download ${TARBALL}`);
}

// ---- 5. Build -----------------------------------------------------------------
// Notes (all verified against the OHOS musl sysroot):
//   - __MUSL__: the OHOS sysroot is musl, not glibc; OpenSSL's linux-*
//     targets assume glibc.
//   - AR/RANLIB: LLVM binutils shipped with the SDK.
//   - pristine tree per ABI: never share build trees between ABIs.
function buildAbi(label, triple, otarget, libdir, subdir, machine) {
  const srcdir = path.join(WORK, `openssl-${VERSION}-${label}`);
  const prefix = path.join(WORK, 'out', `openssl-${label}`);
  const lib = path.join(prefix, libdir);
  console.log(`\n=== OpenSSL ${VERSION} ${label} (${triple}) ===`);
  fs.rmSync(srcdir, { recursive: true, force: true });
  fs.rmSync(prefix, { recursive: true, force: true });
  fs.mkdirSync(srcdir, { recursive: true });
  if (!run('tar', ['-xzf', TGZ, '-C', srcdir, '--strip-components=1'])) die(`failed to extract ${TARBALL}`);

  if (process.platform === 'win32') {
    // Run Configure + make inside the mini-MSYS bash. Two path forms:
    //   - POSIX paths (toPosix) for bash cd / --prefix (msys perl understands)
    //   - Windows-style forward-slash paths for CC --sysroot: clang is a
    //     native Windows exe, and the sysroot inside a CC string is never
    //     auto-converted by msys.
    const inner = [
      '#!/bin/bash',
      'set -euo pipefail',
      `export PATH="/usr/bin:${toPosix(path.join(SDK, 'llvm', 'bin'))}"`,
      `export CC="clang -target ${triple} --sysroot=${SDK.replace(/\\\\/g, '/')}/sysroot -D__MUSL__ -ffunction-sections -fdata-sections"`,
      'export AR=llvm-ar',
      'export RANLIB=llvm-ranlib',
      `cd "${toPosix(srcdir)}"`,
      `perl Configure ${otarget} --prefix="${toPosix(prefix)}" --openssldir="${toPosix(path.join(prefix, 'ssl'))}" no-shared no-tests no-docs`,
      // msys perl rewrites the forward-slash --sysroot path inside $ENV{CC}
      // into a backslash Windows path when it generates the Makefile. Those
      // backslashes are then consumed as escapes by the shell interpreting
      // make recipes, corrupting the sysroot path ("stdlib.h file not
      // found"). Normalize the CC line back to forward slashes (that line
      // only, so make's own line continuations stay intact).
      "sed -i '/^CC=/s|\\\\|/|g' Makefile",
      `make -j${JOBS} build_libs`,
      'make install_dev'
    ].join('\n') + '\n';
    const innerPath = path.join(WORK, `build-openssl-inner-${label}.sh`);
    fs.writeFileSync(innerPath, inner);
    if (!run(bashExe, [toPosix(innerPath)])) die(`OpenSSL build failed for ${label}`);
  } else {
    const env = {
      ...process.env,
      PATH: path.join(SDK, 'llvm', 'bin') + path.delimiter + (process.env.PATH ?? ''),
      CC: `clang -target ${triple} --sysroot=${path.join(SDK, 'sysroot')} -D__MUSL__ -ffunction-sections -fdata-sections`,
      AR: 'llvm-ar',
      RANLIB: 'llvm-ranlib'
    };
    const spawnOpts = { cwd: srcdir, env };
    if (!run('perl', ['Configure', otarget, `--prefix=${prefix}`, `--openssldir=${path.join(prefix, 'ssl')}`, 'no-shared', 'no-tests', 'no-docs'], spawnOpts)) die(`perl Configure failed for ${label}`);
    if (!run('make', [`-j${JOBS}`, 'build_libs'], spawnOpts)) die(`make build_libs failed for ${label}`);
    if (!run('make', ['install_dev'], spawnOpts)) die(`make install_dev failed for ${label}`);
  }

  const libcrypto = path.join(lib, 'libcrypto.a');
  const libssl = path.join(lib, 'libssl.a');
  if (!fs.existsSync(libcrypto)) die(`${libcrypto} missing`);
  if (!fs.existsSync(libssl)) die(`${libssl} missing`);
  if (!verifyMembers(libcrypto, machine) || !verifyMembers(libssl, machine)) {
    die(`architecture verification failed for ${label}`);
  }
  fs.copyFileSync(libcrypto, path.join(VENDORDIR, 'prebuilt', subdir, 'libcrypto.a'));
  fs.copyFileSync(libssl, path.join(VENDORDIR, 'prebuilt', subdir, 'libssl.a'));
  console.log(`-> prebuilt/${subdir}/libcrypto.a  ${fs.statSync(path.join(VENDORDIR, 'prebuilt', subdir, 'libcrypto.a')).size} bytes  sha256 ${sha256Of(path.join(VENDORDIR, 'prebuilt', subdir, 'libcrypto.a'))}`);
  console.log(`-> prebuilt/${subdir}/libssl.a  ${fs.statSync(path.join(VENDORDIR, 'prebuilt', subdir, 'libssl.a')).size} bytes  sha256 ${sha256Of(path.join(VENDORDIR, 'prebuilt', subdir, 'libssl.a'))}`);
  if (label === 'arm64') {
    fs.rmSync(path.join(VENDORDIR, 'openssl-include'), { recursive: true, force: true });
    fs.mkdirSync(path.join(VENDORDIR, 'openssl-include'), { recursive: true });
    fs.cpSync(path.join(prefix, 'include', 'openssl'), path.join(VENDORDIR, 'openssl-include', 'openssl'), { recursive: true });
    // Official release tarballs ship "LICENSE"; the git tree (used when the
    // tarball is reproduced via git archive) ships "LICENSE.txt".
    const licSrc = [path.join(srcdir, 'LICENSE'), path.join(srcdir, 'LICENSE.txt')].find(p => fs.existsSync(p));
    fs.copyFileSync(licSrc, path.join(VENDORDIR, 'prebuilt', 'openssl-LICENSE.txt'));
    console.log('-> openssl-include/ + prebuilt/openssl-LICENSE.txt refreshed');
  }
}

const EM_AARCH64 = 183;
const EM_X86_64 = 62;
switch (TARGET) {
  case 'arm64': buildAbi('arm64', 'aarch64-linux-ohos', 'linux-aarch64', 'lib', 'arm64-v8a', EM_AARCH64); break;
  case 'x64': buildAbi('x64', 'x86_64-linux-ohos', 'linux-x86_64', 'lib64', 'x86_64', EM_X86_64); break;
  case 'all':
    buildAbi('arm64', 'aarch64-linux-ohos', 'linux-aarch64', 'lib', 'arm64-v8a', EM_AARCH64);
    buildAbi('x64', 'x86_64-linux-ohos', 'linux-x86_64', 'lib64', 'x86_64', EM_X86_64);
    break;
  default:
    console.error('usage: node build-openssl.mjs [arm64|x64|all] [--rebuild-msys]');
    process.exit(2);
}

console.log(`\nDone. Updated prebuilt OpenSSL libs for target(s): ${TARGET}`);
console.log('Next: update the SHA256 table in README.md, rebuild libtorrent');
console.log('(build-torrent.mjs) and then the HAP.');
