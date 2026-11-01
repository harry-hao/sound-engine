/* SPDX-License-Identifier: MIT */
#ifndef AUDIO_CLIENT_H
#define AUDIO_CLIENT_H

#include <stdint.h>
#include "ring.h"
#include "ipc.h"

#define AC_BLOCK_FRAMES 160
#define AC_RATE 16000
#define AC_BLOCK_MS 10
#define AC_MAX_BLOCK_FRAMES 480
#define AC_MAX_SAMPLES (AC_MAX_BLOCK_FRAMES * AUDIO_MAX_CHANNELS)

void ac_msleep(int ms);

/* PLAY: channel = CHANNEL_MEDIA/NAV/ALERT */

int ac_open_channel_format(int cmd, float volume, uint32_t channel, uint32_t rate,
                           uint16_t channels, ring_hdr_t **ring, int *sock_fd);
int ac_open_channel(int cmd, float volume, uint32_t channel, ring_hdr_t **ring, int *sock_fd);

int ac_ring_write_wait(ring_hdr_t *r, int sock_fd, const int16_t *data, int frames);

static inline int ac_ring_read(ring_hdr_t *r, int16_t *data, int frames)
{
    return ring_read(r, data, frames);
}

static inline uint32_t ac_block_frames(const ring_hdr_t *r)
{
    return r->rate / (1000 / AC_BLOCK_MS);
}

int ac_ring_read_wait(ring_hdr_t *r, int sock_fd, int16_t *data, int frames);

#endif