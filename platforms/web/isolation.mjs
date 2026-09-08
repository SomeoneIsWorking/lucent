/* Static hosts can supply pthread isolation through a same-origin service worker.
 * The consumer must register this before loading its native application. */
export async function prepareApplication(serviceWorker = "service-worker.js") {
  if (!globalThis.isSecureContext || !navigator.serviceWorker) {
    throw new Error("This application needs a secure browser with service workers.");
  }
  await navigator.serviceWorker.register(serviceWorker, {scope: "./"});
  await navigator.serviceWorker.ready;
  if (!globalThis.crossOriginIsolated) {
    const key = `lucent-isolation:${new URL(serviceWorker, location.href).pathname}`;
    if (sessionStorage.getItem(key)) {
      throw new Error("Browser isolation is unavailable. Enable service workers and reload this page.");
    }
    sessionStorage.setItem(key, "requested");
    location.reload();
    return false;
  }
  sessionStorage.removeItem(`lucent-isolation:${new URL(serviceWorker, location.href).pathname}`);
  return true;
}
