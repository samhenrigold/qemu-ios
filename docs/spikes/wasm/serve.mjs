// Serves the browser check (page.html) on 127.0.0.1:8123 with the COOP/COEP headers threads need.
// usage: [SPIKE_PROBE=secs] [SPIKE_ARGS='-icount ...'] node serve.mjs BUILD_WASM_DIR RUN_DIR OPENLOG
// RUN_DIR without state.bin cold-boots (its overlay should be empty and nor-rw.bin a fresh clone).
// The page gets a manifest: the qemu argv for resuming RUN_DIR's state.bin, and the files to preload into
// MEMFS at their Mac paths: RUN_DIR's NOR and overlay, the boot files, and the NAND pages OPENLOG lists (the
// pages a Node resume read). It posts its log lines to /log and its canvas to /shot (RUN_DIR/browser.*).
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';

const [, , wasmDir, runDir, openlog] = process.argv.map((p, i) => (i > 1 ? path.resolve(p) : p));
const here = path.dirname(new URL(import.meta.url).pathname);
const base = `${process.env.HOME}/Developer/qemu-ios-files/m68/ltm-1x/n72-7E18`;
const rom = `${process.env.HOME}/Developer/qemu-ios-files/bootrom_240_4`;
const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) =>
  e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]);
const pages = [...new Set(fs.readFileSync(openlog, 'utf8').split('\n').filter((p) => p.startsWith(`${base}/nand/`) && fs.existsSync(p)))];  // absent pages read as erased
const state = fs.existsSync(`${runDir}/state.bin`) ? [`${runDir}/state.bin`] : [];  // none: a cold boot
const files = [rom, `${base}/iBoot.bin`, `${base}/gid-blobs.bin`, `${base}/nor.bin`, `${runDir}/nor-rw.bin`,
  ...state, ...walk(`${runDir}/overlay`), ...pages];
const dirs = [`${base}/nand/cs0`, `${base}/nand/cs1`, `${base}/nand/cs2`, `${base}/nand/cs3`,
  ...['cs0', 'cs1', 'cs2', 'cs3'].map((c) => `${runDir}/overlay/${c}`)];
const M = `iPod-Touch,boot-args=amfi_allow_any_signature=1 cs_enforcement_disable=1,direct-iboot=${base}/iBoot.bin,` +
  `direct-llb=,bootrom=${rom},nand=${base}/nand,nor=${base}/nor.bin,nor-rw=${runDir}/nor-rw.bin,` +
  `nandrw=${runDir}/overlay,gid-blobs=${base}/gid-blobs.bin`;
const manifest = {
  args: ['-M', M, '-m', '128M', '-display', 'none', '-no-shutdown', '-audio', 'none', '-serial', 'null',
    ...state.flatMap((f) => ['-incoming', `file:${f}`]), ...(process.env.SPIKE_ARGS || '').split(' ').filter(Boolean)],
  env: { IT_TVOUT_READY: '1', SPIKE_PROBE: process.env.SPIKE_PROBE || '60', SPIKE_CONT: '1', SPIKE_SHOT_EVERY: '1',
    SPIKE_SHOT: '/tmp/screen.ppm' },
  files, dirs,
};
const allowed = new Set(files);
const bytes = files.reduce((n, f) => n + fs.statSync(f).size, 0);
console.log(`manifest: ${files.length} files, ${(bytes / 1e6).toFixed(0)} MB (${pages.length} NAND pages)`);

http.createServer((req, res) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  const url = decodeURIComponent(req.url.split('?')[0]);
  if (req.method === 'POST') {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      const body = Buffer.concat(chunks);
      if (url === '/log') fs.appendFileSync(`${runDir}/browser.log`, `${Date.now() / 1000} ${body}\n`);
      if (url === '/shot') fs.writeFileSync(`${runDir}/browser.png`, Buffer.from(body.toString().split(',')[1], 'base64'));
      res.end();
    });
    return;
  }
  let file;
  if (url === '/' || url === '/page.html') file = path.join(here, 'page.html');
  else if (url === '/manifest.json') return res.end(JSON.stringify(manifest));
  else if (url.startsWith('/qemu/')) file = path.join(wasmDir, path.basename(url));
  else if (url.startsWith('/f/') && allowed.has(url.slice(2))) file = url.slice(2);
  if (!file || !fs.existsSync(file)) { res.statusCode = 404; return res.end(); }
  res.setHeader('Content-Type', file.endsWith('.js') ? 'text/javascript' : file.endsWith('.html') ? 'text/html'
    : file.endsWith('.wasm') ? 'application/wasm' : 'application/octet-stream');
  fs.createReadStream(file).pipe(res);
}).listen(8123, '127.0.0.1');
