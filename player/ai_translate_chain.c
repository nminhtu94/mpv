/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

// Frontend glue for the AI lookahead translator (player/ai_translate.c). This
// is the MPContext-facing counterpart of e.g. audio.c/reinit_audio_chain or
// sub.c/reinit_sub: it owns the worker's lifecycle and drives it once per
// playloop iteration. The worker itself (ai_translate.c) knows nothing about
// MPContext, so all of that glue lives here instead of there.
//
// This file only gathers the glue that already existed in loadfile.c
// (lifecycle) and playloop.c (per-frame driver); it does not add new
// behavior (e.g. no live option-triggered re-spawn yet).

#include <math.h>
#include <string.h>

#include "ai_translate.h"
#include "core.h"
#include "mpv_talloc.h"

#include "common/common.h"
#include "common/msg.h"
#include "osdep/timer.h"
#include "sub/osd.h"

// Minimum wall time to hold an AI stall before degrading (resuming sub-less).
#define AI_STALL_DEGRADE 30.0

// Spawn the lookahead worker for the file that was just loaded. Called from
// loadfile.c/play_current_file() once playback is about to start.
void reinit_ai_translate(struct MPContext *mpctx)
{
    if (mpctx->ai_translate) {
        osd_set_external_remove_owner(mpctx->osd, mpctx->ai_translate);
        ai_translate_destroy(mpctx->ai_translate);
        mpctx->ai_translate = NULL;
        mpctx->paused_for_ai = false;
        TA_FREEP(&mpctx->ai_overlay_text);
    }

    if (mpctx->playback_initialized && mpctx->filename) {
        mpctx->ai_stall_start = -1;
        mpctx->ai_last_pts = MP_NOPTS_VALUE;
        mpctx->ai_translate = ai_translate_create(mpctx->global, mpctx->log,
                                                mpctx->opts->ai_opts,
                                                mpctx->filename);
    }
}

// Stop the worker and remove its OSD overlay. Called from
// loadfile.c/play_current_file() (terminate_playback) when a file is unloaded.
void uninit_ai_translate(struct MPContext *mpctx)
{
    if (!mpctx->ai_translate)
        return;

    osd_set_external_remove_owner(mpctx->osd, mpctx->ai_translate);
    ai_translate_destroy(mpctx->ai_translate);
    mpctx->ai_translate = NULL;
    mpctx->paused_for_ai = false;
    TA_FREEP(&mpctx->ai_overlay_text);
}

static void ai_set_overlay(struct MPContext *mpctx, const char *text)
{
    // Only touch the OSD when the shown text actually changes.
    if ((text && mpctx->ai_overlay_text &&
         strcmp(text, mpctx->ai_overlay_text) == 0) ||
        (!text && !mpctx->ai_overlay_text))
        return;

    talloc_free(mpctx->ai_overlay_text);
    mpctx->ai_overlay_text = text ? talloc_strdup(mpctx, text) : NULL;

    struct osd_external_ass ov = {
        .owner = mpctx->ai_translate,
        .id = 1,
        .format = text ? 1 /* ass-events */ : 0 /* remove */,
    };
    char *data = NULL;
    if (text) {
        bstr buf = {0};
        osd_mangle_ass(&buf, text, true);
        data = talloc_asprintf(NULL, "{\\an2}%.*s", BSTR_P(buf));
        talloc_free(buf.start);
        ov.data = data;
    }
    osd_set_external(mpctx->osd, &ov);
    talloc_free(data);
    mp_wakeup_core(mpctx);
}

// Lookahead translation: pace the worker, gate playback until it is far enough
// ahead, and render the in-sync line onto the OSD. Called once per
// run_playloop() iteration from playloop.c.
void handle_ai_translate(struct MPContext *mpctx)
{
    struct ai_translate *ai = mpctx->ai_translate;
    if (!ai) return;

    double pts = mpctx->playback_pts;
    if (pts == MP_NOPTS_VALUE) {
        ai_set_overlay(mpctx, NULL);
        return;
    }

    // Distinguish a normal frame advance from a seek (large pts jump). Also
    // treat a non-zero start position (--start / resume) as an initial seek so
    // the worker jumps there instead of decoding from the beginning.
    bool first = mpctx->ai_last_pts == MP_NOPTS_VALUE;
    if ((first && pts > 5.0) ||
        (!first && fabs(pts - mpctx->ai_last_pts) > 1.0))
    {
        ai_translate_seek(ai, pts);
    } else {
        ai_translate_set_playhead(ai, pts);
    }
    mpctx->ai_last_pts = pts;

    // Stall gate: pause until translation covers playhead + lookahead.
    double lookahead = mpctx->opts->ai_opts->lookahead;
    double frontier = ai_translate_get_frontier(ai);
    bool want_pause = frontier < pts + lookahead;

    double now = mp_time_sec();
    if (want_pause) {
        if (mpctx->ai_stall_start < 0)
            mpctx->ai_stall_start = now;
        // Degrade: after a long continuous stall, resume sub-less and only
        // re-arm once translation gets 2x lookahead ahead again.
        if (now - mpctx->ai_stall_start > AI_STALL_DEGRADE) {
            if (mpctx->paused_for_ai)
                MP_WARN(mpctx, "[ai-translate] translation slower than "
                        "playback; continuing without waiting.\n");
            want_pause = false;
            if (frontier >= pts + lookahead * 2)
                mpctx->ai_stall_start = -1; // recovered; re-arm
        }
    } else {
        mpctx->ai_stall_start = -1;
    }

    if (want_pause != mpctx->paused_for_ai) {
        mpctx->paused_for_ai = want_pause;
        if (want_pause)
            MP_VERBOSE(mpctx, "[ai-translate] pausing until translation is "
                       "%.0fs ahead...\n", lookahead);
        update_internal_pause_state(mpctx);
    }

    char *text = NULL;
    ai_translate_get_line(ai, pts, &text);
    ai_set_overlay(mpctx, text);
    talloc_free(text);

    // While paused for translation the audio/video threads are idle, so keep
    // polling the worker's progress to know when to resume.
    if (mpctx->paused_for_ai)
        mp_set_timeout(mpctx, 0.1);
}
