#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* C ABI for consumers whose application entry point is C. The returned path is thread-local
 * process-lifetime storage and remains valid until the next call on the same thread. */
const char *lucent_platform_user_data_directory(const char *application_name);

/* Sets the exact app-private directory supplied by an Android Activity or another platform shell.
 * Passing NULL or an empty string clears the override. Returns 1 on success, 0 on invalid input. */
int lucent_platform_set_user_data_directory(const char *directory);

/* Creates the directory and parents with private owner-only permissions where supported. */
int lucent_platform_ensure_user_data_directory(const char *application_name);

#ifdef __cplusplus
}
#endif
