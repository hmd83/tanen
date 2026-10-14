/* Host stand-in for <zephyr/kernel.h> — just the corner of it the units under
 * test touch. Deliberately not a Zephyr port: anything a unit needs beyond this
 * should be added here by hand, so a new kernel dependency in src/ shows up as
 * a host-test compile error instead of being silently satisfied. */
#ifndef TANEN_HOST_STUB_KERNEL_H
#define TANEN_HOST_STUB_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define __packed __attribute__((__packed__))

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* Zephyr's IS_ENABLED(): 1 if the macro is defined to 1, 0 if undefined —
 * usable in #if as well as in C, like the real one in sys/util_macro.h. */
#define Z_IS_ENABLED_XXXX1 Z_IS_ENABLED_YYYY,
#define IS_ENABLED(config_macro) Z_IS_ENABLED1(config_macro)
#define Z_IS_ENABLED1(config_macro) Z_IS_ENABLED2(Z_IS_ENABLED_XXXX##config_macro)
#define Z_IS_ENABLED2(one_or_two_args) Z_IS_ENABLED3(one_or_two_args 1, 0)
#define Z_IS_ENABLED3(ignore_this, val, ...) val

/* Time is whatever the test says it is: k_uptime_get() returns stub_uptime_ms,
 * and k_sleep() advances it instead of blocking. */
extern int64_t stub_uptime_ms;

typedef struct {
	int64_t ms;
} k_timeout_t;

#define K_SECONDS(s) ((k_timeout_t){ .ms = (int64_t)(s) * 1000 })

static inline int64_t k_uptime_get(void)
{
	return stub_uptime_ms;
}

static inline int32_t k_sleep(k_timeout_t t)
{
	stub_uptime_ms += t.ms;
	return 0;
}

#endif
