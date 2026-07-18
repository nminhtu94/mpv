/*
 * Link stubs for the offline ai_translate CUnit tests (ai_translate_test.c).
 *
 * ai_translate.c pulls in three kinds of mpv symbols that we do not want to
 * drag the whole player/config machinery in for:
 *
 *   - the logging boundary (mp_log_new / mp_msg): replaced with no-ops so the
 *     tests stay quiet and log-independent;
 *   - the monotonic clock (mp_time_sec / mp_time_ns): backed directly by
 *     clock_gettime so we don't need osdep/timer.c's init;
 *   - the m_option_type_* descriptors referenced by ai_translate_conf's
 *     static option table: only their addresses are taken (never
 *     dereferenced in these tests), so empty descriptors satisfy the linker.
 */

#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#include "common/msg.h"
#include "options/m_option.h"
#include "osdep/timer.h"

/* --- logging boundary -------------------------------------------------- */

struct mp_log *mp_log_new(void *talloc_ctx, struct mp_log *parent,
                          const char *name)
{
    /* Return an opaque non-NULL token; mp_msg below ignores it anyway. */
    return (struct mp_log *)talloc_ctx;
}

void mp_msg(struct mp_log *log, int lev, const char *format, ...)
{
    (void)log;
    (void)lev;
    (void)format;
}

int mp_msg_level(struct mp_log *log)
{
    (void)log;
    return -1;
}

/* --- monotonic clock --------------------------------------------------- */

int64_t mp_time_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
}

double mp_time_sec(void)
{
    return mp_time_ns() / 1e9;
}

/* --- option-type descriptors (addresses only) -------------------------- */

const struct m_option_type m_option_type_bool = {0};
const struct m_option_type m_option_type_double = {0};
const struct m_option_type m_option_type_string = {0};
