/* SPDX-License-Identifier: MIT */
#ifndef AUDIO_H
#define AUDIO_H

#include <math.h>
#include <stdint.h>

/*
 * Shared audio format limits. Buffers are sized for the worst case: 96 kHz,
 * 10 ms blocks, stereo.
 */
#define AUDIO_MAX_CHANNELS 2
#define AUDIO_MAX_BLOCK_FRAMES 960
#define AUDIO_MAX_BLOCK_SAMPLES (AUDIO_MAX_BLOCK_FRAMES * AUDIO_MAX_CHANNELS)

/* Convert a mixed float sample to S16 with consistent rounding. */
static inline int16_t audio_clamp16(float value)
{
    if (value > 32767.0f) {
        return INT16_MAX;
    }
    if (value < -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)lrintf(value);
}

#endif
