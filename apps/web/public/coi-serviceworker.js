/*
 * Cross-origin isolation for hosts that can't set response headers, such as GitHub Pages.
 *
 * WebAssembly threads share memory through SharedArrayBuffer, and browsers only allow that on a
 * page served with two headers: Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy.
 * This one file runs in two places:
 *
 *   - On the page (a plain <script>), it registers itself as a service worker, then reloads the
 *     page once so the page is loaded through it.
 *   - As the service worker, it adds the two headers to every same-origin response.
 *
 * If service workers are unavailable (private windows in some browsers, or file://), nothing
 * happens and the app falls back to its single-threaded build. The same idea as the
 * coi-serviceworker project by Guido Zuidhof (MIT).
 */
if (typeof window === 'undefined') {
  // --- Service worker side ---
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

  self.addEventListener('fetch', (event) => {
    const request = event.request;
    if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') return;
    if (new URL(request.url).origin !== self.location.origin) return; // Tiles from AWS pass untouched

    event.respondWith(
      fetch(request).then((response) => {
        if (response.status === 0) return response; // Opaque: headers can't be changed
        const headers = new Headers(response.headers);
        headers.set('Cross-Origin-Opener-Policy', 'same-origin');
        headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
        headers.set('Cross-Origin-Resource-Policy', 'same-origin');
        return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
      }),
    );
  });
} else {
  // --- Page side ---
  (() => {
    if (window.crossOriginIsolated || !window.isSecureContext || !('serviceWorker' in navigator)) return;

    // Reload at most once per attempt, so a browser that refuses isolation can't loop forever.
    const key = 'coi-reloaded';
    let reloaded = false;
    try {
      reloaded = sessionStorage.getItem(key) === '1';
    } catch {}
    const reloadOnce = () => {
      if (reloaded) return;
      try {
        sessionStorage.setItem(key, '1');
      } catch {}
      window.location.reload();
    };

    navigator.serviceWorker.register(document.currentScript.src).then(
      (registration) => {
        // Already installed but this page load bypassed it (a hard refresh): load through it.
        if (registration.active && !navigator.serviceWorker.controller) reloadOnce();
        // First visit: reload when it takes control.
        navigator.serviceWorker.addEventListener('controllerchange', reloadOnce);
      },
      (error) => console.warn('Cross-origin isolation is unavailable; running single-threaded.', error),
    );
  })();

  try {
    if (window.crossOriginIsolated) sessionStorage.removeItem('coi-reloaded');
  } catch {}
}
