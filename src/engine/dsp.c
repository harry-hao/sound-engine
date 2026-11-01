/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <stdio.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include "engine.h"

struct dsp {
    SpeexEchoState *echo;
    SpeexPreprocessState *pre;
    int use_aec;
    int use_pre;
};

int dsp_init(dsp_t **out, int use_aec, int use_pre)
{
    dsp_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return -1;
    }

    d->echo = speex_echo_state_init(ENGINE_BLOCK_FRAMES, AEC_FILTER_LENGTH);
    d->pre = speex_preprocess_state_init(ENGINE_BLOCK_FRAMES, ENGINE_RATE);
    if (!d->echo) {
        dsp_free(d);
        return -1;
    }
    if (!d->pre) {
        dsp_free(d);
        return -1;
    }

    d->use_aec = use_aec;
    d->use_pre = use_pre;
    fprintf(stderr, "[dsp] aec=%d preprocess(ns/agc/vad)=%d\n", d->use_aec, d->use_pre);

    int rate = ENGINE_RATE;
    speex_echo_ctl(d->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

    int one = 1;
    speex_preprocess_ctl(d->pre, SPEEX_PREPROCESS_SET_DENOISE, &one);
    speex_preprocess_ctl(d->pre, SPEEX_PREPROCESS_SET_AGC, &one);
    speex_preprocess_ctl(d->pre, SPEEX_PREPROCESS_SET_VAD, &one);
    speex_preprocess_ctl(d->pre, SPEEX_PREPROCESS_SET_ECHO_STATE, d->echo);
    float agc_level = 8000.0f;
    speex_preprocess_ctl(d->pre, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_level);
    *out = d;
    return 0;
}

int dsp_process(dsp_t *d, const int16_t *near, const int16_t *far, int16_t *out)
{
    memcpy(out, near, ENGINE_BLOCK_FRAMES * sizeof(int16_t));
    if (d->use_aec && g_aec_enabled) {
        speex_echo_cancellation(d->echo, near, far, out);
    }
    if (d->use_pre) {
        return speex_preprocess_run(d->pre, out);
    }
    return 0;
}

int dsp_process_silent_far(dsp_t *d, const int16_t *near, int16_t *out)
{
    memcpy(out, near, ENGINE_BLOCK_FRAMES * sizeof(int16_t));
    if (d->use_pre) {
        return speex_preprocess_run(d->pre, out);
    }
    return 0;
}

void dsp_free(dsp_t *d)
{
    if (!d) {
        return;
    }
    if (d->echo) {
        speex_echo_state_destroy(d->echo);
    }
    if (d->pre) {
        speex_preprocess_state_destroy(d->pre);
    }
    free(d);
}
