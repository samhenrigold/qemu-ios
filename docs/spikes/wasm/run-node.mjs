// Runs the wasm qemu-system-arm under Node: node run-node.mjs path/to/qemu-system-arm.mjs [qemu args...]
// The Mac's /Users is mounted at /Users (NODEFS), so host paths work unchanged; SPIKE_* and IT_* environment
// variables are passed through (Emscripten's getenv doesn't read process.env). SPIKE_OPENLOG=FILE records every
// path the emulator opens (to list the NAND pages a resumed session reads, for the browser page).
import fs from 'node:fs';
import path from 'node:path';

const [, , modPath, ...args] = process.argv;
const factory = (await import(path.resolve(modPath))).default;
await factory({
  arguments: args,
  preRun: [(M) => {
    M.FS.mkdir('/Users');
    M.FS.mount(M.FS.filesystems.NODEFS, { root: '/Users' }, '/Users');
    for (const [k, v] of Object.entries(process.env)) {
      if (k.startsWith('SPIKE_') || k.startsWith('IT_')) M.ENV[k] = v;
    }
    const log = process.env.SPIKE_OPENLOG;
    if (log) {
      const open = M.FS.open;
      M.FS.open = (p, ...rest) => (fs.appendFileSync(log, `${p}\n`), open(p, ...rest));
    }
  }],
  print: (s) => process.stdout.write(s + '\n'),
  printErr: (s) => process.stderr.write(s + '\n'),
});
