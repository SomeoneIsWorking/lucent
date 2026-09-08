#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the browser's origin-private filesystem at an absolute virtual path.
 * Call once, on the application pthread before opening persistent files.
 * Returns 1 on success, 0 with errno set on refusal. The browser main thread
 * must remain free to service filesystem worker creation. */
int lucent_web_mount_storage(const char *mountpoint);

/* Release the mount on its application worker after all persistent files are
 * closed. Stored files are retained. This prevents browser-main-thread runtime
 * destruction from synchronously waiting for OPFS workers. */
int lucent_web_unmount_storage(const char *mountpoint);

#ifdef __cplusplus
}
#endif
