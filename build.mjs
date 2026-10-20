#!/usr/bin/env node
// Honyomi BT engine (integrated HSP) build script.
// Usage: run `node build.mjs` from the repo root.
// Output: torrent/build/**/*.tgz, copied to ../mihon/entry/libs/torrent-default.tgz
// Only Node.js is required (bundled with the HarmonyOS SDK / DevEco Studio).
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)));

function die(msg) {
  console.error(`ERROR: ${msg}`);
  process.exit(1);
}

function devEcoHome() {
  if (process.env.DEVECO_HOME) return process.env.DEVECO_HOME;
  const cands = process.platform === 'win32'
    ? [path.join(process.env.ProgramFiles ?? 'C:\\Program Files', 'Huawei', 'DevEco Studio')]
    : ['/Applications/DevEco Studio.app'];
  for (const c of cands) if (fs.existsSync(c)) return c;
  return null;
}

// Run a command string through the shell (handles quoted paths with spaces,
// e.g. ohpm.bat living under "DevEco Studio").
function runShell(cmd, cwd) {
  const r = spawnSync(cmd, { cwd: cwd || ROOT, stdio: 'inherit', shell: true });
  return r.status === 0;
}

const home = devEcoHome();
if (!home) die('DEVECO_HOME is not set (DevEco Studio install dir)');
const hvigorw = path.join(home, 'tools', 'hvigor', 'bin', 'hvigorw.js');
if (!fs.existsSync(hvigorw)) die(`hvigorw.js not found at ${hvigorw}`);
const ohpmExt = process.platform === 'win32' ? '.bat' : '';
const ohpm = path.join(home, 'tools', 'ohpm', 'bin', 'ohpm' + ohpmExt);
if (!fs.existsSync(ohpm)) die(`ohpm not found at ${ohpm}`);

console.log('==> ohpm install');
if (!runShell(`"${ohpm}" install --all`)) die('ohpm install failed');

console.log('==> hvigor assembleHsp (release)');
const hvigorArgs = [
  hvigorw,
  '--mode', 'module',
  '-p', 'module=torrent@default',
  '-p', 'product=default',
  '-p', 'buildMode=release',
  '--no-daemon',
  'assembleHsp'
];
const hv = spawnSync(process.execPath, hvigorArgs, { cwd: ROOT, stdio: 'inherit' });
if (hv.status !== 0) die('hvigor assembleHsp failed');

// Find the newest .tgz produced under torrent/build.
let tgz = null;
let newestMtime = 0;
const buildDir = path.join(ROOT, 'torrent', 'build');
function walk(dir) {
  let entries = [];
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of entries) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.isFile() && e.name.endsWith('.tgz')) {
      const m = fs.statSync(p).mtimeMs;
      if (m > newestMtime) { newestMtime = m; tgz = p; }
    }
  }
}
walk(buildDir);
if (!tgz) die('No .tgz artifact found under torrent\\build');

const destDir = path.join(ROOT, '..', 'mihon', 'entry', 'libs');
fs.mkdirSync(destDir, { recursive: true });
fs.copyFileSync(tgz, path.join(destDir, 'torrent-default.tgz'));
console.log(`OK: ${tgz} -> mihon\\entry\\libs\\torrent-default.tgz`);
