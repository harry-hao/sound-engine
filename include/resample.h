/* SPDX-License-Identifier: MIT */
#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stdint.h>

#include "audio.h"

typedef struct resampler resampler_t;

int resampler_create(resampler_t **resampler, unsigned in_rate, unsigned out_rate,
                     unsigned in_channels, unsigned out_channels);
void resampler_destroy(resampler_t **resampler);
int resampler_process(resampler_t *resampler, const int16_t *input, unsigned input_frames,
                      int16_t *output, unsigned output_frames);

#endif
