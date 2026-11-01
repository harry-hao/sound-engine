/* SPDX-License-Identifier: MIT */
#ifndef LIMITER_H
#define LIMITER_H

#include <stdint.h>

typedef struct limiter limiter_t;

/* Returns 0 or a negative errno value. */
int limiter_create(limiter_t **limiter, unsigned rate, unsigned channels);
void limiter_destroy(limiter_t **limiter);

void limiter_process(limiter_t *limiter, const float *input, int16_t *output, unsigned frames);

#endif
