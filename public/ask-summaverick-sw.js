/* Minimal service worker: exists so Summaverick can be installed to the
 * home screen. All requests pass straight through to the network — no
 * caching, so deploys are always picked up immediately. */
self.addEventListener('install', function () {
  self.skipWaiting();
});

self.addEventListener('activate', function (event) {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', function () {
  // Intentionally empty: default network handling.
});
