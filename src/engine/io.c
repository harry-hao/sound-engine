/* SPDX-License-Identifier: MIT */
#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <time.h>
#include "engine.h"
#include "resample.h"

atomic_int g_pcm_quit = 0;
atomic_long g_pcm_underrun = 0;
atomic_long g_pcm_overrun = 0;

static resampler_t *cap_res = NULL;

static const unsigned RATE_CANDIDATES[] = {48000, 44100, 96000, 32000, 16000};
static const unsigned CHANNEL_CANDIDATES[] = {2, 1};

typedef struct {
    unsigned rate;
    unsigned channels;
    unsigned block_frames;
    snd_pcm_uframes_t period_frames;
    snd_pcm_uframes_t buffer_frames;
} pcm_hw_info_t;

static pcm_hw_info_t g_pcm_hw[2];

typedef enum {
    TIMESTAMP_UNTESTED,
    TIMESTAMP_HARDWARE,
    TIMESTAMP_FALLBACK,
} timestamp_mode_t;

typedef struct {
    timestamp_mode_t mode;
    int good_samples;
    int bad_samples;
    uint64_t previous_ns;
    int logged;
} timestamp_probe_t;

static timestamp_probe_t g_timestamp[2];

unsigned pcm_rate_for(snd_pcm_stream_t dir)
{
    return g_pcm_hw[dir].rate;
}

unsigned pcm_channels_for(snd_pcm_stream_t dir)
{
    return g_pcm_hw[dir].channels;
}

unsigned pcm_block_frames_for(snd_pcm_stream_t dir)
{
    return g_pcm_hw[dir].block_frames;
}

snd_pcm_uframes_t pcm_buffer_frames_for(snd_pcm_stream_t dir)
{
    return g_pcm_hw[dir].buffer_frames;
}

static uint64_t timespec_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_ns(&ts);
}

static const char *pcm_direction_name(snd_pcm_stream_t dir)
{
    return dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback";
}

static bool htimestamp_self_test(snd_pcm_t *h, snd_pcm_stream_t dir, int ts_ret,
                                 const struct timespec *ts)
{
    timestamp_probe_t *probe = &g_timestamp[dir];
    bool valid = ts_ret == 0 && (ts->tv_sec != 0 || ts->tv_nsec != 0);
    uint64_t current_ns = valid ? timespec_ns(ts) : 0;
    bool monotonic = valid && (probe->good_samples == 0 || current_ns > probe->previous_ns);

    if (probe->mode == TIMESTAMP_UNTESTED && snd_pcm_state(h) != SND_PCM_STATE_RUNNING) {
        return false;
    }

    if (probe->mode == TIMESTAMP_UNTESTED) {
        if (valid && monotonic) {
            probe->good_samples++;
            probe->bad_samples = 0;
        } else {
            probe->bad_samples++;
            probe->good_samples = 0;
        }
        probe->previous_ns = current_ns;

        if (probe->good_samples >= 3) {
            probe->mode = TIMESTAMP_HARDWARE;
        } else if (probe->bad_samples >= 3) {
            probe->mode = TIMESTAMP_FALLBACK;
        }

        if (probe->mode != TIMESTAMP_UNTESTED && !probe->logged) {
            fprintf(
                stderr, "[alsa] timestamp self-test: %s=%s (%d/3 valid)\n", pcm_direction_name(dir),
                probe->mode == TIMESTAMP_HARDWARE ? "hardware" : "monotonic-fallback",
                probe->mode == TIMESTAMP_HARDWARE ? probe->good_samples : 3 - probe->bad_samples);
            probe->logged = 1;
        }
    }

    return probe->mode == TIMESTAMP_HARDWARE && valid;
}

/*
 * Read the hardware timestamp with a monotonic fallback. Returns the clock
 * base in nanoseconds used to derive content timing on either stream
 * direction, and stores the buffered frame count when avail_out is non-NULL.
 * The first samples taken after the stream enters RUNNING also feed the
 * hardware-timestamp self test; until it passes, the monotonic clock is
 * used.
 */
static uint64_t pcm_hw_clock(snd_pcm_t *h, snd_pcm_stream_t dir, snd_pcm_uframes_t *avail_out)
{
    struct timespec ts;
    snd_pcm_uframes_t avail = 0; /* required by snd_pcm_htimestamp */
    int ts_ret = snd_pcm_htimestamp(h, &avail, &ts);
    if (avail_out != NULL) {
        *avail_out = avail;
    }
    return htimestamp_self_test(h, dir, ts_ret, &ts) ? timespec_ns(&ts) : monotonic_ns();
}

int pcm_open(snd_pcm_t **h, snd_pcm_stream_t dir, const char *dev)
{
    g_timestamp[dir] = (timestamp_probe_t){0};
    if (dir == SND_PCM_STREAM_CAPTURE) {
        resampler_destroy(&cap_res);
    }
    int err = snd_pcm_open(h, dev, dir, 0);
    if (err < 0) {
        return err;
    }

    snd_pcm_hw_params_t *probe = NULL;
    snd_pcm_hw_params_alloca(&probe);
    if (snd_pcm_hw_params_any(*h, probe) != 0) {
        fprintf(stderr, "[alsa] hw_params_any failed\n");
        snd_pcm_close(*h);
        *h = NULL;
        return -EINVAL;
    }

    if (snd_pcm_hw_params_test_format(*h, probe, SND_PCM_FORMAT_S16_LE) != 0) {
        fprintf(stderr, "[alsa] %s: S16_LE not supported\n", dev);
        snd_pcm_close(*h);
        *h = NULL;
        return -EINVAL;
    }
    if (snd_pcm_hw_params_test_access(*h, probe, SND_PCM_ACCESS_MMAP_INTERLEAVED) != 0) {
        fprintf(stderr, "[alsa] %s: MMAP_INTERLEAVED not supported\n", dev);
        snd_pcm_close(*h);
        *h = NULL;
        return -EINVAL;
    }
    unsigned channels = 0;
    for (size_t i = 0; i < sizeof(CHANNEL_CANDIDATES) / sizeof(CHANNEL_CANDIDATES[0]); i++) {
        if (snd_pcm_hw_params_test_channels(*h, probe, CHANNEL_CANDIDATES[i]) == 0) {
            channels = CHANNEL_CANDIDATES[i];
            break;
        }
    }
    if (channels == 0) {
        fprintf(stderr, "[alsa] no mono/stereo channel support on %s\n", dev);
        snd_pcm_close(*h);
        *h = NULL;
        return -EINVAL;
    }

    unsigned rate = 0;
    for (size_t i = 0; i < sizeof(RATE_CANDIDATES) / sizeof(RATE_CANDIDATES[0]); i++) {
        if (snd_pcm_hw_params_test_rate(*h, probe, RATE_CANDIDATES[i], 0) == 0) {
            rate = RATE_CANDIDATES[i];
            break;
        }
    }
    if (rate == 0) {
        fprintf(stderr, "[alsa] only support rate in 48k/44.1k/96k/32k/16k\n");
        snd_pcm_close(*h);
        *h = NULL;
        return -EINVAL;
    }

    unsigned block_frames = rate / (1000 / ENGINE_BLOCK_MS); /* 10ms */
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_uframes_t period_size = block_frames; /* 10ms */
    unsigned int buffer_ms =
        dir == SND_PCM_STREAM_PLAYBACK ? PLAYBACK_BUFFER_MS : CAPTURE_BUFFER_MS;
    snd_pcm_uframes_t buffer_size = (snd_pcm_uframes_t)rate / 1000 * buffer_ms;
    int rate_dir = 0;

    snd_pcm_hw_params_alloca(&hw);
    err = snd_pcm_hw_params_any(*h, hw);
    if (err == 0) {
        err = snd_pcm_hw_params_set_access(*h, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_format(*h, hw, SND_PCM_FORMAT_S16_LE);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_channels(*h, hw, channels);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_rate_resample(*h, hw, 0);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_rate_near(*h, hw, &rate, &rate_dir);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_period_size_near(*h, hw, &period_size, &rate_dir);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_buffer_size_near(*h, hw, &buffer_size);
    }
    if (err == 0) {
        err = snd_pcm_hw_params(*h, hw);
    }
    if (err < 0) {
        snd_pcm_close(*h);
        *h = NULL;
        return err;
    }
    g_pcm_hw[dir].rate = rate;
    g_pcm_hw[dir].channels = channels;
    g_pcm_hw[dir].block_frames = block_frames;
    snd_pcm_hw_params_get_period_size(hw, &g_pcm_hw[dir].period_frames, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &g_pcm_hw[dir].buffer_frames);
    if (dir == SND_PCM_STREAM_CAPTURE) {
        err = resampler_create(&cap_res, rate, ENGINE_RATE, 1, 1);
        if (err < 0) {
            fprintf(stderr, "[alsa] create capture resampler failed: %s\n", strerror(-err));
            snd_pcm_close(*h);
            *h = NULL;
            return err;
        }
    }
    fprintf(stderr, "[alsa] native rate=%uHz block=%u frames\n", rate, block_frames);

    snd_pcm_sw_params_t *sw = NULL;
    snd_pcm_sw_params_alloca(&sw);
    err = snd_pcm_sw_params_current(*h, sw);
    if (err == 0) {
        err = snd_pcm_sw_params_set_tstamp_mode(*h, sw, SND_PCM_TSTAMP_ENABLE);
    }
    if (err == 0) {

        (void)snd_pcm_sw_params_set_tstamp_type(*h, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
        err = snd_pcm_sw_params(*h, sw);
    }
    if (err < 0) {
        fprintf(stderr, "[alsa] enable timestamp failed: %s\n", snd_strerror(err));
        snd_pcm_close(*h);
        *h = NULL;
        return err;
    }

    if (dir == SND_PCM_STREAM_CAPTURE) {
        err = snd_pcm_start(*h);
    }
    if (err < 0) {
        fprintf(stderr, "[alsa] %s: %s\n",
                dir == SND_PCM_STREAM_CAPTURE ? "capture start failed" : "set_params failed",
                snd_strerror(err));
    }
    fprintf(stderr, "[alsa] %s: buffer=%lu frames period=%lu frames\n",
            dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", g_pcm_hw[dir].buffer_frames,
            g_pcm_hw[dir].period_frames);
    return err;
}

/*
 * Write native-rate input into the device ring buffer directly. Input is
 * S16LE interleaved hardware audio.
 *
 * Returns 0 on success.
 */
static int pcm_write_all_mmap(snd_pcm_t *h, const int16_t *src, unsigned frames, unsigned channels)
{
    const int16_t *p = src;
    unsigned todo = frames;

    while (todo > 0) {
        if (g_pcm_quit) {
            return -EINTR;
        }
        snd_pcm_sframes_t n = snd_pcm_mmap_writei(h, p, todo);
        if (n == -EPIPE) {
            atomic_fetch_add_explicit(&g_pcm_underrun, 1, memory_order_relaxed);
            if (snd_pcm_recover(h, (int)n, 1) < 0) {
                return -EIO;
            }
            continue;
        }
        if (n < 0) {
            return (int)n;
        }
        p += (size_t)n * channels;
        todo -= (unsigned)n;
    }
    return 0;
}

/*
 * Read frames from device ring buffer, native rate/channels S16_LE
 * interleaved.
 */
static int pcm_read_all_mmap(snd_pcm_t *h, int16_t *dst, unsigned frames, unsigned channels)
{
    int16_t *p = dst;
    unsigned todo = frames;

    while (todo > 0) {
        if (g_pcm_quit) {
            return -EINTR;
        }
        snd_pcm_sframes_t n = snd_pcm_mmap_readi(h, p, todo);
        if (n == -EPIPE) {
            atomic_fetch_add_explicit(&g_pcm_overrun, 1, memory_order_relaxed);
            if (snd_pcm_recover(h, (int)n, 1) < 0) {
                return -EIO;
            }
            continue;
        }
        if (n < 0) {
            return (int)n;
        }
        p += (size_t)n * channels;
        todo -= (unsigned)n;
    }
    return 0;
}

int pcm_capture_read(snd_pcm_t *h, int16_t *raw, int16_t *mono, uint64_t *capture_end_ns,
                     int *buffered_ms)
{
    int16_t hw[AUDIO_MAX_BLOCK_SAMPLES];
    int16_t mono_hw[AUDIO_MAX_BLOCK_FRAMES];
    int err;
    unsigned hw_rate = pcm_rate_for(SND_PCM_STREAM_CAPTURE);
    unsigned channels = pcm_channels_for(SND_PCM_STREAM_CAPTURE);
    unsigned block = pcm_block_frames_for(SND_PCM_STREAM_CAPTURE);
    if (buffered_ms != NULL) {
        *buffered_ms = 0;
    }

    err = pcm_read_all_mmap(h, hw, block, channels);
    if (err == 0 && raw != NULL) {
        memcpy(raw, hw, (size_t)block * channels * sizeof(*raw));
    }
    if (err < 0) {
        return err;
    }

    if (capture_end_ns != NULL) {
        snd_pcm_uframes_t avail;

        *capture_end_ns = 0;
        uint64_t clock_ns = pcm_hw_clock(h, SND_PCM_STREAM_CAPTURE, &avail);
        if (buffered_ms != NULL) {

            *buffered_ms = (int)(avail * 1000 / hw_rate);
        }
        uint64_t backlog_ns = (uint64_t)avail * 1000000000ULL / hw_rate;
        *capture_end_ns = clock_ns - backlog_ns;
    }

    for (unsigned j = 0; j < block; j++) {
        int acc = 0;
        for (unsigned ch = 0; ch < channels; ch++) {
            acc += hw[j * channels + ch];
        }
        mono_hw[j] = (int16_t)(acc / (int)channels);
    }

    return resampler_process(cap_res, mono_hw, block, mono, ENGINE_BLOCK_FRAMES);
}

/*
 * Write a native-rate interleaved block directly to ALSA. The mixer owns
 * stream format conversion. After writing, hardware timestamp and buffered
 * delay describe when this block becomes audible. The audible timestamp is
 * the content time of the block's last sample: the monotonic time when it
 * reaches the speaker. Resampler processing delay is intentionally not
 * added; it delays availability, not the acoustic timeline.
 */
int pcm_playback_write(snd_pcm_t *h, const int16_t *frames, uint64_t *audible_end_ns,
                       snd_pcm_sframes_t *buffer_delay)
{
    unsigned channels = pcm_channels_for(SND_PCM_STREAM_PLAYBACK);
    int err =
        pcm_write_all_mmap(h, frames, pcm_block_frames_for(SND_PCM_STREAM_PLAYBACK), channels);
    if (err < 0) {
        return err;
    }

    if (audible_end_ns != NULL || buffer_delay != NULL) {
        snd_pcm_uframes_t avail = 0;
        snd_pcm_uframes_t buffered = 0;
        unsigned hw_rate = pcm_rate_for(SND_PCM_STREAM_PLAYBACK);
        snd_pcm_uframes_t buffer = g_pcm_hw[SND_PCM_STREAM_PLAYBACK].buffer_frames;

        if (audible_end_ns != NULL) {
            *audible_end_ns = 0;
        }
        if (buffer_delay != NULL) {
            *buffer_delay = 0;
        }
        uint64_t clock_ns = pcm_hw_clock(h, SND_PCM_STREAM_PLAYBACK, &avail);
        if (avail <= buffer) {
            buffered = buffer - avail;
        }
        if (buffer_delay != NULL) {
            *buffer_delay = (snd_pcm_sframes_t)buffered;
        }
        if (audible_end_ns != NULL) {
            *audible_end_ns = clock_ns + (uint64_t)buffered * 1000000000ULL / hw_rate;
        }
    }

    return 0;
}
