#!/usr/bin/env node
// ============================================================================
// Fetches the Boost 1.86 headers required to build torrent_bridge.cpp and
// (re)build libtorrent against the prebuilt static libraries.
//
// The headers (~140 MB, ~16k files) are gitignored; this script downloads
// the official boost_1_86_0 tarball once (cached in %TEMP%/mihon-torrent,
// $TMPDIR/mihon-torrent on macOS/Linux) and extracts only the public boost/
// headers to deps/boost-1.86/boost.
//
// Usage: node fetch-boost.mjs
// (also run automatically by build-torrent.mjs)
// Only Node.js is required (bundled with the HarmonyOS SDK / DevEco Studio).
// ============================================================================
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import https from 'node:https';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const VENDORDIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)));
const WORK = path.join(os.tmpdir(), 'mihon-torrent');
const DEPS = path.join(VENDORDIR, 'deps', 'boost-1.86');
const TARBALL = path.join(WORK, 'downloads', 'boost-1.86.0-b2-nodocs.tar.gz');
const URL = 'https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.gz';

function die(msg) {
  console.error(`ERROR: ${msg}`);
  process.exit(1);
}

// Download via Node https with redirect following (GitHub release URLs redirect).
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

function run(cmd, args, opts) {
  const r = spawnSync(cmd, args, { stdio: 'inherit', ...opts });
  return r.status === 0;
}

if (fs.existsSync(path.join(DEPS, 'boost', 'version.hpp'))) {
  console.log(`Boost 1.86 headers already present at ${DEPS}`);
  process.exit(0);
}

fs.mkdirSync(path.join(WORK, 'downloads'), { recursive: true });
if (!fs.existsSync(TARBALL)) {
  console.log(`Downloading ${URL} ...`);
  try {
    await download(URL, TARBALL, 0);
  } catch (err) {
    die(`download failed: ${err.message}. Download the tarball manually from\n       ${URL}\n       and place it at ${TARBALL}`);
  }
}

fs.mkdirSync(DEPS, { recursive: true });
console.log('Extracting boost/ headers ...');
if (!run('tar', ['-xzf', TARBALL, '-C', DEPS, '--strip-components=1', 'boost-1.86.0/boost'])) {
  die(`tar extraction failed for ${TARBALL}`);
}
if (!fs.existsSync(path.join(DEPS, 'boost', 'version.hpp'))) {
  die(`extraction incomplete - ${path.join(DEPS, 'boost', 'version.hpp')} missing`);
}
console.log(`Done. Boost 1.86 headers installed at ${DEPS}`);
