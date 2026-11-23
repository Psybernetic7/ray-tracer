// Builds both WebAssembly modules into public/wasm/: studio-mt (multithreaded) and studio-st
// (the single-threaded fallback). Needs the Emscripten SDK activated in this shell, so that
// emcmake is on the PATH, and Ninja.
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..', '..');

function run(command, args) {
  const line = [command, ...args].join(' ');
  console.log(`> ${line}`);
  // Through a shell, which finds emcmake.bat on Windows as well as emcmake elsewhere. The
  // arguments are fixed strings from this file, so there is nothing to escape.
  const result = spawnSync(line, { cwd: root, stdio: 'inherit', shell: true });
  if (result.status !== 0) process.exit(result.status ?? 1);
}

for (const [flavour, threads] of [['mt', 'ON'], ['st', 'OFF']]) {
  const build = join('build-web', flavour);
  run('emcmake', ['cmake', '-S', '.', '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DRT_WEB_THREADS=${threads}`]);
  run('cmake', ['--build', build]);
}
