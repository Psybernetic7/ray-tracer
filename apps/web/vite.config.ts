import { defineConfig } from 'vite';

// Threads in WebAssembly need SharedArrayBuffer, which browsers only allow on a page that is
// "cross-origin isolated" by these two headers. The dev and preview servers send them; GitHub
// Pages can't, so there public/coi-serviceworker.js adds them instead.
const isolation = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
};

export default defineConfig({
  base: './', // Relative paths, so the site works under any folder, such as /ray-tracer/
  server: { headers: isolation },
  preview: { headers: isolation },
  worker: { format: 'es' },
  build: { target: 'es2022' },
});
