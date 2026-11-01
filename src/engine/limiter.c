/* SPDX-License-Identifier: MIT */
#include "limiter.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"

#define LIMITER_THRESHOLD 0.85f
#define LIMITER_ATTACK_MS 1.0f
#define LIMITER_RELEASE_MS 150.0f
#define LIMITER_LOOKAHEAD_MS 5.0f

struct limiter {
    unsigned rate;
    unsigned channels;
    float threshold;
    float attack;
    float release;
    float gain;
    float *delay;
    size_t delay_samples;
    size_t delay_pos;
};

// https://www.musicdsp.org/en/latest/Effects/169-compressor.html
static float env_coeff(unsigned rate, float ms)
{
    return 1.0f - expf(-1.0f / ((ms / 1000.0f) * (float)rate));
}

int limiter_create(limiter_t **limiter, unsigned rate, unsigned channels)
{
    if (limiter == NULL || rate == 0 || channels == 0 || channels > AUDIO_MAX_CHANNELS) {
        return -EINVAL;
    }

    limiter_t *instance = calloc(1, sizeof(*instance));
    if (instance == NULL) {
        return -ENOMEM;
    }

    instance->rate = rate;
    instance->channels = channels;
    instance->threshold = LIMITER_THRESHOLD * 32767.0f;
    instance->attack = env_coeff(rate, LIMITER_ATTACK_MS);
    instance->release = env_coeff(rate, LIMITER_RELEASE_MS);
    instance->gain = 1.0f;
    instance->delay_samples = (size_t)(rate / 1000.0f * LIMITER_LOOKAHEAD_MS) * channels;
    if (instance->delay_samples > 0) {
        instance->delay = calloc(instance->delay_samples, sizeof(*instance->delay));
        if (instance->delay == NULL) {
            free(instance);
            return -ENOMEM;
        }
    }
    *limiter = instance;
    return 0;
}

void limiter_destroy(limiter_t **limiter)
{
    if (limiter == NULL || *limiter == NULL) {
        return;
    }
    free((*limiter)->delay);
    free(*limiter);
    *limiter = NULL;
}

void limiter_process(limiter_t *limiter, const float *input, int16_t *output, unsigned frames)
{
    const unsigned channels = limiter->channels;

    /* Find the peak value in the current block of audio samples. */
    float block_peak = 0.0f;
    for (unsigned i = 0; i < (size_t)frames * channels; i++) {
        float v = fabsf(input[i]);
        if (v > block_peak) {
            block_peak = v;
        }
    }

    /* Decide target gain. */
    float target = block_peak > limiter->threshold ? limiter->threshold / block_peak : 1.0f;

    /* Smooth the gain. */
    for (unsigned frame = 0; frame < frames; frame++) {
        const float *in = &input[(size_t)frame * channels];
        float coeff = target < limiter->gain ? limiter->attack : limiter->release;
        limiter->gain += (target - limiter->gain) * coeff;

        float delayed[AUDIO_MAX_CHANNELS] = {0};
        if (limiter->delay != NULL) {
            float *slot = &limiter->delay[limiter->delay_pos];
            for (unsigned ch = 0; ch < channels; ch++) {
                delayed[ch] = slot[ch];
                slot[ch] = in[ch];
            }
            limiter->delay_pos = (limiter->delay_pos + channels) % limiter->delay_samples;
        } else {
            for (unsigned ch = 0; ch < channels; ch++) {
                delayed[ch] = in[ch];
            }
        }

        int16_t *out = &output[(size_t)frame * channels];
        for (unsigned ch = 0; ch < channels; ch++) {
            out[ch] = audio_clamp16(delayed[ch] * limiter->gain);
        }
    }
}
