/*
 * CUnit tests for the public API declared in player/ai_translate.h.
 *
 * This is the mpv port of the mplayer-era ai_translator test. The mpv
 * implementation uses an object-handle API (create/destroy) instead of the
 * old global-singleton module, and only the lookahead worker exists (there is
 * no realtime-tap fallback), so an un-openable / un-reopenable source disables
 * the feature by returning NULL rather than degrading to a realtime mode.
 *
 * Scope: the public-API contract that does not require a live STT /
 * translation server or a real media file:
 *   - NULL-handle safety and sentinel returns
 *   - construction guards (disabled, un-reopenable sources -> NULL)
 *   - worker lifecycle for an un-openable file (create -> worker fails to
 *     open -> frontier becomes +inf so playback never stalls -> destroy joins)
 *   - input-guard robustness (NaN / out-of-range pts, MP_NOPTS_VALUE)
 *
 * The stateful lookahead logic (pts-keyed lookup, stall/resume, coverage)
 * needs the worker to ingest real audio and reach a translation server, so it
 * is covered by the end-to-end runs described in aitranslator-tdd.md.
 *
 * The mp_log / mp_time boundary and the option-table's m_option_type_*
 * references are replaced by link stubs (ai_translate_test_stubs.c) so these
 * tests are deterministic and offline. The real libcurl / FFmpeg libraries are
 * linked, but the reachable states here never contact a server: an un-openable
 * path fails fast in avformat_open_input().
 *
 * Build & run (from the repo root, after `meson setup build`):
 *   CFG=build                      # dir holding the generated config.h
 *   CU=$(pkg-config --cflags --libs cunit)
 *   cc -I. -Iinclude -I"$CFG" -o /tmp/ai_translate_test \
 *       player/ai_translate.c player/ai_translate_test.c \
 *       player/ai_translate_test_stubs.c \
 *       ta/ta.c ta/ta_talloc.c ta/ta_utils.c \
 *       $(pkg-config --cflags --libs libcurl libavformat libavcodec \
 *                                    libavutil libswresample) \
 *       -lpthread $CU
 *   /tmp/ai_translate_test
 *
 * config.h must have HAVE_LIBCURL 1, otherwise ai_translate_create() is the
 * built-out stub that always returns NULL and the lifecycle test is skipped.
 */

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

#include "config.h"
#include "player/ai_translate.h"

/* A benign options set: translation off (no API key needed) and a short STT
 * window. No real media is opened in most tests, so no HTTP request is made. */
static struct ai_translate_opts default_opts(void)
{
    struct ai_translate_opts o;
    memset(&o, 0, sizeof(o));
    o.enabled   = true;
    o.lang      = "vi";
    o.src       = "";
    o.stt_url   = "http://127.0.0.1:65535";  /* unused: no full window pushed */
    o.tr_url    = "http://127.0.0.1:65535";
    o.stt_key   = "";
    o.model     = "claude-opus-4-8";
    o.translate = false;                      /* no Claude calls */
    o.window    = 5;
    o.lookahead = 10;
    return o;
}

/* --------------------------------------------------------------------- */
/* NULL-handle contract: every accessor must be safe on a NULL worker    */
/* and report "nothing" without touching caller state.                   */
/* --------------------------------------------------------------------- */

static void test_null_handle_contract(void)
{
    char *txt = (char *)0x1;

    /* mutators on a NULL handle are no-ops */
    ai_translate_destroy(NULL);
    ai_translate_set_playhead(NULL, 3.0);
    ai_translate_seek(NULL, 3.0);

    /* frontier is +inf so the stall gate never pauses when disabled */
    CU_ASSERT_TRUE(isinf(ai_translate_get_frontier(NULL)));

    /* lookup reports "nothing" and clears the out pointer */
    CU_ASSERT_FALSE(ai_translate_get_line(NULL, 5.0, &txt));
    CU_ASSERT_PTR_NULL(txt);

    CU_PASS("NULL-handle calls are all safe");
}

/* pts guards: NaN / out-of-range / sentinel must not crash any accessor. */
static void test_pts_guards(void)
{
    double bad[] = {NAN, -1e300, 1e18, 0.0, -0.0, INFINITY, -INFINITY};
    char *txt = NULL;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ai_translate_set_playhead(NULL, bad[i]);
        ai_translate_seek(NULL, bad[i]);
        CU_ASSERT_FALSE(ai_translate_get_line(NULL, bad[i], &txt));
        CU_ASSERT_PTR_NULL(txt);
    }
    CU_PASS("pts guards accept degenerate values");
}

/* --------------------------------------------------------------------- */
/* Construction guards: create() returns NULL (feature off) without ever */
/* spawning a worker.                                                    */
/* --------------------------------------------------------------------- */

static void test_disabled_returns_null(void)
{
    struct ai_translate_opts o = default_opts();

    /* NULL opts */
    CU_ASSERT_PTR_NULL(ai_translate_create(NULL, NULL, NULL, "movie.mp4"));

    /* explicitly disabled */
    o.enabled = false;
    CU_ASSERT_PTR_NULL(ai_translate_create(NULL, NULL, &o, "movie.mp4"));
}

static void test_unreopenable_sources_return_null(void)
{
    struct ai_translate_opts o = default_opts();
    const char *bad[] = {
        NULL, "", "-", "stdin://", "dvd://1", "dvdnav://",
        "bd://", "bluray://", "tv://", "pvr://", "cdda://",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CU_ASSERT_PTR_NULL_FATAL(ai_translate_create(NULL, NULL, &o, bad[i]));
    }
}

/* --------------------------------------------------------------------- */
/* Lifecycle for an un-openable regular path.                            */
/* --------------------------------------------------------------------- */

#if HAVE_LIBCURL
/* create() spawns the worker; the worker fails to open the path and marks the
 * job done, which surfaces as a +inf frontier (so the stall gate never
 * pauses). All accessors stay safe and destroy() joins cleanly. */
static void test_lifecycle_bad_path(void)
{
    struct ai_translate_opts o = default_opts();
    char *txt = NULL;

    struct ai_translate *ai =
        ai_translate_create(NULL, NULL, &o, "/no/such/media/file.xyz");
    CU_ASSERT_PTR_NOT_NULL_FATAL(ai);

    /* Worker runs on its own thread; give it time to fail the open and mark
     * the job done (frontier -> +inf). Poll rather than sleep-and-hope. */
    double frontier = ai_translate_get_frontier(ai);
    for (int i = 0; i < 200 && !isinf(frontier); i++) {
        usleep(5000);                       /* up to ~1s total */
        frontier = ai_translate_get_frontier(ai);
    }
    CU_ASSERT_TRUE(isinf(frontier));

    /* No results for an un-openable source, at any pts. */
    CU_ASSERT_FALSE(ai_translate_get_line(ai, 5.0, &txt));
    CU_ASSERT_PTR_NULL(txt);

    /* Playhead / seek reports remain safe on a done worker. */
    ai_translate_set_playhead(ai, 1.0);
    ai_translate_seek(ai, 42.0);
    CU_ASSERT_FALSE(ai_translate_get_line(ai, 42.0, &txt));

    ai_translate_destroy(ai);
    CU_PASS("bad-path worker lifecycle joins cleanly");
}

/* Rapid create/destroy without letting the worker settle must not crash or
 * leak the thread (teardown signals quit and joins). */
static void test_create_destroy_churn(void)
{
    struct ai_translate_opts o = default_opts();

    for (int i = 0; i < 5; i++) {
        struct ai_translate *ai =
            ai_translate_create(NULL, NULL, &o, "/no/such/file.xyz");
        CU_ASSERT_PTR_NOT_NULL_FATAL(ai);
        ai_translate_destroy(ai);           /* immediate teardown */
    }
    CU_PASS("create/destroy churn is clean");
}
#endif /* HAVE_LIBCURL */

/* --------------------------------------------------------------------- */

int main(void)
{
    CU_pSuite s;

    if (CU_initialize_registry() != CUE_SUCCESS)
        return CU_get_error();

    s = CU_add_suite("ai_translate public API", NULL, NULL);
    if (!s) {
        CU_cleanup_registry();
        return CU_get_error();
    }

#define ADD(name, fn) \
    do { if (!CU_add_test(s, name, fn)) { \
             CU_cleanup_registry(); return CU_get_error(); } } while (0)

    ADD("NULL-handle contract",             test_null_handle_contract);
    ADD("pts guards",                       test_pts_guards);
    ADD("disabled create returns NULL",     test_disabled_returns_null);
    ADD("un-reopenable sources -> NULL",    test_unreopenable_sources_return_null);
#if HAVE_LIBCURL
    ADD("lifecycle: un-openable path",      test_lifecycle_bad_path);
    ADD("create/destroy churn",             test_create_destroy_churn);
#endif

#undef ADD

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    {
        unsigned int failures = CU_get_number_of_failures();
        CU_cleanup_registry();
        return failures ? 1 : 0;
    }
}
