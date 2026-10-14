/* Host stand-in for <zephyr/logging/log.h>. The macros swallow the message but
 * still pass it through a printf-format-checked function, so a mismatched
 * format/argument in src/ fails the host build under -Werror — the same class
 * of bug prj_production.conf (CONFIG_LOG=n) would otherwise hide. */
#ifndef TANEN_HOST_STUB_LOG_H
#define TANEN_HOST_STUB_LOG_H

__attribute__((format(printf, 1, 2)))
static inline void stub_log(const char *fmt, ...)
{
	(void)fmt;
}

#define LOG_MODULE_REGISTER(...) struct stub_log_module_unused
#define LOG_ERR(...) stub_log(__VA_ARGS__)
#define LOG_WRN(...) stub_log(__VA_ARGS__)
#define LOG_INF(...) stub_log(__VA_ARGS__)
#define LOG_DBG(...) stub_log(__VA_ARGS__)

#endif
