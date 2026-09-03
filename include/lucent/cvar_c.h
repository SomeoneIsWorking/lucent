/* lucent/cvar_c.h -- C access to the layered CVars defined in lucent/cvar.hpp.
 *
 * A C translation unit cannot hold a Var<T>, but it can read one that a C++
 * startup unit registered. Each getter looks the CVar up by name and returns
 * its effective value (default < file < environment < --set).
 *
 * An unknown name is a programming error -- the CVar was never registered -- and
 * aborts with a message rather than silently returning the fallback, so a typo
 * surfaces at once instead of as a setting that never takes effect.
 */
#ifndef LUCENT_CVAR_C_H
#define LUCENT_CVAR_C_H

#ifdef __cplusplus
extern "C" {
#endif

/* Boolean CVar. `fallback` is returned only when the value fails to parse; an
 * unregistered name aborts. */
int lucent_cvar_flag(const char *name, int fallback);

/* Integer CVar (decimal or 0x hex in its sources). */
long lucent_cvar_number(const char *name, long fallback);

/* String CVar. The pointer is owned by the CVar and valid until the value is
 * next mutated; copy it if you need to keep it. Never null for a registered
 * name. */
const char *lucent_cvar_text(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* LUCENT_CVAR_C_H */
