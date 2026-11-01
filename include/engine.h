/* SPDX-License-Identifier: MIT */
#ifndef ENGINE_H
#define ENGINE_H

#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include "ring.h"
#include "ipc.h"
#include "resample.h"

#define ENGINE_RATE 16000
#define ENGINE_CHANNELS 1
#define ENGINE_BLOCK_MS 10
#define ENGINE_BLOCK_FRAMES (ENGINE_RATE / 1000 * ENGINE_BLOCK_MS)
#define AEC_TAIL_MS 500
#define AEC_FILTER_LENGTH (ENGINE_RATE / 1000 * AEC_TAIL_MS)

#define PLAYBACK_BUFFER_MS 50
#define CAPTURE_BUFFER_MS 50
#define PLAYBACK_START_FRAMES (ENGINE_RATE / 1000 * PLAYBACK_BUFFER_MS)

typedef struct {
    int used;
    float volume; /* only valid in playback */
    audio_format_t format;
    resampler_t *resampler;
    ring_hdr_t *ring;
    uint32_t shm_id;
    int fd;
} channel_t;

extern channel_t g_channels[AUDIO_CHANNEL_COUNT];

extern atomic_int g_pcm_quit;
extern atomic_long g_pcm_underrun;
extern atomic_long g_pcm_overrun;
extern atomic_int g_dsp_enabled;
extern atomic_int g_aec_enabled;
extern atomic_int g_hw_volume_pct;
extern atomic_int g_play_level_pct;

void channels_init(void);
void channels_lock(void);
void channels_unlock(void);

int ipc_init(void);
void ipc_poll_step(int timeout_ms);

typedef struct dsp dsp_t;
int dsp_init(dsp_t **out);
int dsp_process(dsp_t *d, const int16_t *near, const int16_t *far, int16_t *out);
int dsp_process_silent_far(dsp_t *d, const int16_t *near, int16_t *out);
void dsp_free(dsp_t *d);

int pcm_open(snd_pcm_t **h, snd_pcm_stream_t dir, const char *dev);
int pcm_capture_read(snd_pcm_t *h, int16_t *raw, int16_t *mono, uint64_t *capture_end_ns,
                     int *buffered_ms);
int pcm_playback_write(snd_pcm_t *h, const int16_t *frames, uint64_t *audible_end_ns,
                       snd_pcm_sframes_t *buffer_delay);

unsigned pcm_rate_for(snd_pcm_stream_t dir);
unsigned pcm_channels_for(snd_pcm_stream_t dir);
unsigned pcm_block_frames_for(snd_pcm_stream_t dir);
snd_pcm_uframes_t pcm_buffer_frames_for(snd_pcm_stream_t dir);

int hw_volume_init(void);

/* Returns a 0..100 percentage or a negative errno value. */
int hw_volume_get(void);

/* Returns the resulting 0..100 percentage or a negative errno value. */
int hw_volume_set(int pct);

#endif
