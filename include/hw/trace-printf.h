/*
 * Free-form debug lines through a trace event that takes one string, for the
 * device models' old fprintf traces: TRACE_PRINTF(trace_foo_log, fmt, ...).
 * Callers check trace_event_get_state_backends(TRACE_FOO_LOG) first, so the
 * formatting only happens while `-trace foo_log` is on. A trailing newline is
 * dropped; the trace backend adds its own.
 */
#ifndef HW_TRACE_PRINTF_H
#define HW_TRACE_PRINTF_H

#define TRACE_PRINTF(fn, ...)                                       \
    do {                                                            \
        g_autofree char *trace_printf_msg_ = g_strdup_printf(__VA_ARGS__); \
        fn(g_strchomp(trace_printf_msg_));                          \
    } while (0)

#endif
