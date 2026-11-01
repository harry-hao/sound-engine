/* SPDX-License-Identifier: MIT */
#ifndef IPC_H
#define IPC_H

#include <stdint.h>

#include "audio.h"

#define CTL_SOCK_PATH "/tmp/alsa-engine.sock"
#define SHM_NAME_PREFIX "/alsa-eng-"

typedef struct {
    uint32_t rate;
    uint16_t channels;
} audio_format_t;

enum {
    CMD_PLAY = 1,
    CMD_RECORD = 2,
    CMD_CLOSE = 3,
    CMD_RECORD_RAW = 4,
    CMD_TOGGLE_AEC = 5,
    CMD_SET_VOLUME = 6,
    CMD_STATUS = 7,
};

enum {
    CHANNEL_MEDIA = 0,
    CHANNEL_NAV = 1,
    CHANNEL_ALERT = 2,
    CHANNEL_LOOPBACK = 3,
    CHANNEL_MIC = 4,
    CHANNEL_MIC_RAW = 5,
    AUDIO_CHANNEL_COUNT,
};

#define AUDIO_PLAY_CHANNEL_COUNT 4

typedef struct {
    uint32_t cmd;
    uint32_t shm_id;
    uint32_t capacity_frames;
    float volume;
    uint32_t channel;
    uint32_t rate;
    uint16_t channels;
} ctl_msg_t;

#define STREAM_RING_FRAMES 16000

#endif
