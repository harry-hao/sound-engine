/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include "engine.h"
#include "audio.h"
#include "limiter.h"
#include "resample.h"

static volatile sig_atomic_t g_quit = 0;
atomic_int g_dsp_enabled = 1;
atomic_int g_aec_enabled = 1;
atomic_int g_hw_volume_pct = -1;
atomic_int g_play_level_pct = 0;

static void on_sigint(int s)
{
    (void)s;
    g_quit = 1;
}

static snd_pcm_t *g_play = NULL, *g_cap = NULL;

/* AEC alignment */
typedef struct {
    uint64_t audible_end_ns; /* set by playback thread, snd_pcm_htimestamp() */
    int16_t data[ENGINE_BLOCK_FRAMES];
} far_block_t;
#define FARQ_BLOCKS 64
static far_block_t g_farq[FARQ_BLOCKS];
static uint32_t g_farq_r = 0, g_farq_w = 0;
static pthread_mutex_t g_far_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_playback_buffered_ms = 0;
static atomic_int g_capture_buffered_ms = 0;
static int g_stats_debug = 0;
static atomic_long g_play_iters = 0;
static atomic_long g_far_published = 0;
static atomic_long g_far_invalid = 0;

/* Skip AEC when far reference is too silent. */
#define FAR_SILENCE_RMS 160
/* Skip AEC when far reference is too far from near capture. */
#define FAR_MATCH_MAX_DIFF_NS 100000000ULL

static void far_publish(const int16_t *blk, uint64_t audible_end_ns)
{
    /* invalid timestamp: don't publish */
    if (audible_end_ns == 0) {
        atomic_fetch_add_explicit(&g_far_invalid, 1, memory_order_relaxed);
        return;
    }

    /* Have to lock because we modify both g_farq_r/w. */
    pthread_mutex_lock(&g_far_mu);
    /* Discard oldest far block if queue is full. */
    if (g_farq_w - g_farq_r >= FARQ_BLOCKS) {
        g_farq_r++;
    }

    far_block_t *block = &g_farq[g_farq_w % FARQ_BLOCKS];
    block->audible_end_ns = audible_end_ns;
    memcpy(block->data, blk, sizeof(block->data));
    g_farq_w++;
    pthread_mutex_unlock(&g_far_mu);
    atomic_fetch_add_explicit(&g_far_published, 1, memory_order_relaxed);
}

static int far_take_by_timestamp(const uint64_t near_end_ns, int16_t *far_ref, long long *diff_us)
{
    int found = 0;
    int result = 0;
    uint32_t selected = 0;
    uint64_t best_diff = UINT64_MAX;

    pthread_mutex_lock(&g_far_mu);
    for (uint32_t i = g_farq_r; i < g_farq_w; i++) {
        far_block_t *block = &g_farq[i % FARQ_BLOCKS];
        uint64_t diff;

        if (block->audible_end_ns >= near_end_ns) {
            diff = block->audible_end_ns - near_end_ns;
        } else {
            diff = near_end_ns - block->audible_end_ns;
        }
        if (diff < best_diff) {
            best_diff = diff;
            selected = i;
            found = 1;
        }
    }

    if (found && best_diff <= FAR_MATCH_MAX_DIFF_NS) {
        far_block_t *block = &g_farq[selected % FARQ_BLOCKS];
        memcpy(far_ref, block->data, sizeof(block->data));
        *diff_us = (long long)((int64_t)block->audible_end_ns - (int64_t)near_end_ns) / 1000;
        g_farq_r = selected + 1;
        result = 1;
    } else if (found) {

        far_block_t *block = &g_farq[selected % FARQ_BLOCKS];
        *diff_us = (long long)((int64_t)block->audible_end_ns - (int64_t)near_end_ns) / 1000;
        g_farq_r = g_farq_w;
        result = -1;
    }
    pthread_mutex_unlock(&g_far_mu);

    return result;
}

static double block_rms(const int16_t *samples, int count)
{
    double sum = 0;

    for (int i = 0; i < count; i++) {
        double value = samples[i];
        sum += value * value;
    }
    return sqrt(sum / (double)count);
}

static int play_level_pct(const int16_t *samples, int count)
{
    double pct = sqrt(block_rms(samples, count) / 32767.0) * 100.0;
    int value = (int)(pct + 0.5);
    return value > 100 ? 100 : value;
}

static int channel_active(const int *used_frames, uint32_t wanted_channel)
{
    return used_frames[wanted_channel] > 0;
}

static float policy_gain(uint32_t channel, int alert_active, int nav_active)
{
    /* loopback is for experimental testing, always full volume */
    if (channel == CHANNEL_LOOPBACK) {
        return 1.0f;
    }
    if (alert_active) {
        return channel == CHANNEL_ALERT ? 1.0f : 0.1f;
    }
    if (nav_active && channel == CHANNEL_MEDIA) {
        return 0.3f;
    }
    return 1.0f;
}

static int convert_stream_to_hw(channel_t *channel, const int16_t *input, unsigned input_frames,
                                int16_t *output, unsigned output_frames, unsigned hw_rate,
                                unsigned hw_channels)
{
    int16_t converted[AUDIO_MAX_BLOCK_SAMPLES];
    const int16_t *source = input;
    unsigned source_frames = input_frames;

    if (channel->format.rate != hw_rate) {
        int err;
        if (channel->resampler == NULL) {
            err = resampler_create(&channel->resampler, channel->format.rate, hw_rate,
                                   channel->format.channels, channel->format.channels);
            if (err < 0) {
                return err;
            }
        }
        err = resampler_process(channel->resampler, input, input_frames, converted, output_frames);
        if (err < 0) {
            return err;
        }
        source = converted;
        source_frames = output_frames;
    }

    if (channel->format.channels == hw_channels) {
        memcpy(output, source, (size_t)source_frames * hw_channels * sizeof(*output));
    } else if (channel->format.channels == 1) {
        for (unsigned frame = 0; frame < source_frames; frame++) {
            for (unsigned ch = 0; ch < hw_channels; ch++) {
                output[frame * hw_channels + ch] = source[frame];
            }
        }
    } else if (channel->format.channels == 2 && hw_channels == 1) {
        for (unsigned frame = 0; frame < source_frames; frame++) {
            int32_t left = source[frame * 2];
            int32_t right = source[frame * 2 + 1];
            output[frame] = (int16_t)((left + right) / 2);
        }
    } else {
        return -EINVAL;
    }
    return 0;
}

typedef struct {
    unsigned rate;
    unsigned channels;
    unsigned block_frames;
    resampler_t *reference_resampler;
    limiter_t *limiter;
} playback_ctx_t;

static int downmix_far_reference(const int16_t *hw_block, int16_t *far_ref, playback_ctx_t *ctx)
{
    if (ctx->reference_resampler == NULL) {
        int err =
            resampler_create(&ctx->reference_resampler, ctx->rate, ENGINE_RATE, ctx->channels, 1);
        if (err < 0) {
            return err;
        }
    }
    return resampler_process(ctx->reference_resampler, hw_block, ctx->block_frames, far_ref,
                             ENGINE_BLOCK_FRAMES);
}

static void mix_play_channels(int16_t *far, int16_t *hw_out, playback_ctx_t *ctx)
{
    float acc[AUDIO_MAX_BLOCK_SAMPLES] = {0};
    int16_t input[AUDIO_MAX_BLOCK_SAMPLES];
    int16_t mapped[AUDIO_PLAY_CHANNEL_COUNT][AUDIO_MAX_BLOCK_SAMPLES];
    int used_frames[AUDIO_PLAY_CHANNEL_COUNT] = {0};
    const unsigned hw_samples = ctx->block_frames * ctx->channels;

    /* Avoid channel being freed while we are reading. */
    channels_lock();
    for (int i = 0; i < AUDIO_PLAY_CHANNEL_COUNT; i++) {
        channel_t *channel = &g_channels[i];
        unsigned input_frames;
        int read_frames;
        int err;

        if (!channel->used) {
            continue;
        }

        /* We only support 16k, 32k, 44.1k, 48k rate client for now. */
        input_frames = channel->format.rate / (1000 / ENGINE_BLOCK_MS);
        if (input_frames == 0 || input_frames > AUDIO_MAX_BLOCK_FRAMES ||
            channel->format.channels == 0 || channel->format.channels > AUDIO_MAX_CHANNELS) {
            continue;
        }

        read_frames = ring_read(channel->ring, input, (int)input_frames);
        if (read_frames <= 0) {
            continue;
        }

        /* FIXME: Use jitter buffer to avoid client underrun. */
        if ((unsigned)read_frames < input_frames) {
            memset(input + (size_t)read_frames * channel->format.channels, 0,
                   (size_t)(input_frames - (unsigned)read_frames) * channel->format.channels *
                       sizeof(*input));
        }

        /* FIXME: avoid hold lock while doing resampling. */
        err = convert_stream_to_hw(channel, input, input_frames, mapped[i], ctx->block_frames,
                                   ctx->rate, ctx->channels);
        if (err < 0) {
            fprintf(stderr, "[mixer] format conversion failed: %s\n", strerror(-err));
            continue;
        }

        used_frames[i] = read_frames;
    }

    /* Ducking: alert > nav > media. The loopback not involved. */
    int alert_active = channel_active(used_frames, CHANNEL_ALERT);
    int nav_active = channel_active(used_frames, CHANNEL_NAV);
    for (int i = 0; i < AUDIO_PLAY_CHANNEL_COUNT; i++) {
        channel_t *channel = &g_channels[i];
        if (!channel->used || used_frames[i] == 0) {
            continue;
        }
        float gain = policy_gain((uint32_t)i, alert_active, nav_active);
        for (unsigned sample = 0; sample < hw_samples; sample++) {
            acc[sample] += gain * channel->volume * mapped[i][sample];
        }
    }
    channels_unlock();

    /* Fallback to hard clip if limiter is not available. */
    if (ctx->limiter == NULL) {
        for (unsigned sample = 0; sample < hw_samples; sample++) {
            hw_out[sample] = audio_clamp16(acc[sample]);
        }
        memset(far, 0, (size_t)ENGINE_BLOCK_FRAMES * sizeof(*far));
        return;
    } else {
        limiter_process(ctx->limiter, acc, hw_out, ctx->block_frames);
    }

    if (downmix_far_reference(hw_out, far, ctx) < 0) {
        memset(far, 0, (size_t)ENGINE_BLOCK_FRAMES * sizeof(*far));
    }
}

static void *playback_thread(void *arg)
{
    (void)arg;
    int16_t far_blk[ENGINE_BLOCK_FRAMES];
    int16_t hw_blk[AUDIO_MAX_BLOCK_SAMPLES];
    playback_ctx_t ctx = {.rate = pcm_rate_for(SND_PCM_STREAM_PLAYBACK),
                          .channels = pcm_channels_for(SND_PCM_STREAM_PLAYBACK),
                          .block_frames = pcm_block_frames_for(SND_PCM_STREAM_PLAYBACK),
                          .reference_resampler = NULL,
                          .limiter = NULL};

    if (limiter_create(&ctx.limiter, ctx.rate, ctx.channels) < 0) {
        fprintf(stderr, "[limiter] create failed, using hard clip fallback\n");
    }

    unsigned written = 0;
    int started = 0;
    const unsigned hw_rate = ctx.rate;

    unsigned start_frames = hw_rate / 1000 * PLAYBACK_BUFFER_MS;
    snd_pcm_uframes_t hw_buffer = pcm_buffer_frames_for(SND_PCM_STREAM_PLAYBACK);
    if (start_frames > hw_buffer) {
        start_frames = (unsigned)hw_buffer;
    }

    while (!g_pcm_quit) {
        mix_play_channels(far_blk, hw_blk, &ctx);
        unsigned hw_samples = ctx.block_frames * ctx.channels;

        uint64_t audible_end_ns = 0;
        snd_pcm_sframes_t delay = 0;
        int werr = pcm_playback_write(g_play, hw_blk, &audible_end_ns, &delay);
        if (werr < 0) {
            fprintf(stderr, "[alsa] playback write failed: %s\n", snd_strerror(-werr));
            break;
        }
        if (block_rms(far_blk, ENGINE_BLOCK_FRAMES) >= FAR_SILENCE_RMS) {
            far_publish(far_blk, audible_end_ns);
        }
        if (delay > 0) {
            atomic_store_explicit(&g_playback_buffered_ms, (int)(delay * 1000 / hw_rate),
                                  memory_order_relaxed);
        }
        atomic_store_explicit(&g_play_level_pct, play_level_pct(hw_blk, (int)hw_samples),
                              memory_order_relaxed);
        atomic_fetch_add(&g_play_iters, 1);
        written += ctx.block_frames;

        if (!started && written >= start_frames) {
            snd_pcm_start(g_play);
            started = 1;
        }
    }
    resampler_destroy(&ctx.reference_resampler);
    limiter_destroy(&ctx.limiter);
    return NULL;
}

static void *record_thread(void *arg)
{
    dsp_t *dsp = (dsp_t *)arg;
    int16_t raw_near[AUDIO_MAX_BLOCK_SAMPLES];
    int16_t near[ENGINE_BLOCK_FRAMES], clean[ENGINE_BLOCK_FRAMES], far_ref[ENGINE_BLOCK_FRAMES];
    memset(far_ref, 0, sizeof(far_ref));
    int prev_vad = -1;
    long vad_changes = 0;
    long iter = 0;
    long sec_rec = 0;
    long rec_drops = 0;
    long long align_sum_us = 0;
    long long align_max_us = 0;
    long align_count = 0;
    double near_rms_sum = 0;
    double far_rms_sum = 0;
    long far_silent_count = 0;
    long far_stale_count = 0;
    long long far_stale_diff_sum_us = 0;
    long long far_stale_diff_max_us = 0;
    unsigned cap_block_frames = pcm_block_frames_for(SND_PCM_STREAM_CAPTURE);

    while (!g_pcm_quit) {
        uint64_t near_end_ns = 0;
        int cap_buffered_ms = 0;
        int rerr = pcm_capture_read(g_cap, raw_near, near, &near_end_ns, &cap_buffered_ms);
        if (rerr < 0) {
            fprintf(stderr, "[alsa] capture read failed: %s\n", snd_strerror(-rerr));
            break;
        }
        atomic_store_explicit(&g_capture_buffered_ms, cap_buffered_ms, memory_order_relaxed);
        sec_rec++;

        long long diff_us = 0;
        if (near_end_ns != 0) {
            int far_state = far_take_by_timestamp(near_end_ns, far_ref, &diff_us);
            if (far_state > 0) {
                long long abs_us = diff_us >= 0 ? diff_us : -diff_us;

                align_sum_us += diff_us;
                align_count++;
                if (abs_us > align_max_us) {
                    align_max_us = abs_us;
                }
            } else if (far_state == 0) {
                memset(far_ref, 0, sizeof(far_ref));
            } else {
                far_stale_count++;
                far_stale_diff_sum_us += diff_us;
                if (diff_us > far_stale_diff_max_us) {
                    far_stale_diff_max_us = diff_us;
                }
                memset(far_ref, 0, sizeof(far_ref));
            }
        } else {
            memset(far_ref, 0, sizeof(far_ref));
        }

        double near_rms = block_rms(near, ENGINE_BLOCK_FRAMES);
        near_rms_sum += near_rms * near_rms;
        double far_rms = block_rms(far_ref, ENGINE_BLOCK_FRAMES);
        far_rms_sum += far_rms * far_rms;
        if (far_rms < FAR_SILENCE_RMS) {
            memset(far_ref, 0, sizeof(far_ref));
            far_silent_count++;
        }

        if (!g_dsp_enabled) {
            memcpy(clean, near, sizeof(clean));
        } else if (far_rms < FAR_SILENCE_RMS) {

            dsp_process_silent_far(dsp, near, clean);
        } else {

            int vad = dsp_process(dsp, near, far_ref, clean);
            if (vad != prev_vad) {
                prev_vad = vad;
                vad_changes++;
            }
        }

        channels_lock();
        for (uint32_t ch = CHANNEL_MIC; ch <= CHANNEL_MIC_RAW; ch++) {
            channel_t *channel = &g_channels[ch];
            if (!channel->used) {
                continue;
            }
            unsigned block_frames = ENGINE_BLOCK_FRAMES;
            const int16_t *out = clean;
            if (ch == CHANNEL_MIC_RAW) {
                block_frames = cap_block_frames;
                out = raw_near;
            }

            if (ring_write(channel->ring, out, (int)block_frames) < 0) {
                rec_drops += block_frames;
            }
        }
        channels_unlock();

        iter++;
        if (iter % 100 == 0) {
            long pr = atomic_exchange(&g_play_iters, 0);
            long rec_now = sec_rec;
            long far_published = atomic_exchange(&g_far_published, 0);
            double near_rms = sqrt(near_rms_sum / (double)(rec_now * ENGINE_BLOCK_FRAMES));
            double far_rms = sqrt(far_rms_sum / (double)(rec_now * ENGINE_BLOCK_FRAMES));
            char align_buf[32];
            const char *vad_buf = prev_vad < 0 ? "-" : (prev_vad ? "speech" : "silence");

            sec_rec = 0;

            if (align_count > 0) {
                snprintf(align_buf, sizeof(align_buf), "%+.1f/%.1fms",
                         (double)align_sum_us / 1000.0 / (double)align_count,
                         (double)align_max_us / 1000.0);
            } else if (far_silent_count == rec_now) {
                snprintf(align_buf, sizeof(align_buf), "far-silent");
            } else {
                snprintf(align_buf, sizeof(align_buf), "no-match");
            }
            if (g_stats_debug) {
                fprintf(stderr,
                        "[engine] %4lds ur/or=%ld/%-3ld play=%3ld/s rec=%3ld/s buf=%3d/%3dms "
                        "vad=%-7s chg=%-2ld rms near=%5.0f far=%5.0f silent=%3ld align=%14s "
                        "farq=%3ld/%-3ld drops=%3ld\n",
                        iter / 100, atomic_load_explicit(&g_pcm_underrun, memory_order_relaxed),
                        atomic_load_explicit(&g_pcm_overrun, memory_order_relaxed), pr, rec_now,
                        atomic_load_explicit(&g_playback_buffered_ms, memory_order_relaxed),
                        atomic_load_explicit(&g_capture_buffered_ms, memory_order_relaxed), vad_buf,
                        vad_changes, near_rms, far_rms, far_silent_count, align_buf, far_published,
                        far_stale_count, rec_drops);
            } else {
                fprintf(stderr,
                        "[engine] %4lds ur/or=%ld/%-3ld play=%3ld/s rec=%3ld/s buf=%3d/%3dms\n",
                        iter / 100, atomic_load_explicit(&g_pcm_underrun, memory_order_relaxed),
                        atomic_load_explicit(&g_pcm_overrun, memory_order_relaxed), pr, rec_now,
                        atomic_load_explicit(&g_playback_buffered_ms, memory_order_relaxed),
                        atomic_load_explicit(&g_capture_buffered_ms, memory_order_relaxed));
            }
            align_sum_us = 0;
            align_count = 0;
            align_max_us = 0;
            near_rms_sum = 0;
            far_rms_sum = 0;
            far_silent_count = 0;
            far_stale_count = 0;
            far_stale_diff_sum_us = 0;
            far_stale_diff_max_us = 0;
            vad_changes = 0;
            if (rec_drops) {
                rec_drops = 0;
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fprintf(stderr, "usage: alsa-engine [--debug] [device]\n");
        fprintf(stderr, "full-duplex mmap engine with mixing and AEC/NS/AGC/VAD\n");
        fprintf(stderr, "  --debug  verbose per-second stats (VAD/RMS/AEC align)\n");
        fprintf(stderr, "device: ALSA device name; default: auto-detect "
                        "(ALSA_ENGINE_DEVICE, default, plughw:0,0, hw:0,0)\n");
        return 0;
    }
    signal(SIGINT, on_sigint);

    const char *dev = NULL;
    if (argc > 1) {

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--debug") == 0) {
                g_stats_debug = 1;
            } else {
                dev = argv[i];
            }
        }
    }
    if (dev == NULL) {

        const char *candidates[] = {
            getenv("ALSA_ENGINE_DEVICE"),
            "default",
            "plughw:0,0",
            "hw:0,0",
        };
        int last_err = 0;
        const char *last_dev = NULL;
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            const char *candidate = candidates[i];
            if (!candidate || !candidate[0]) {
                continue;
            }
            snd_pcm_t *cap = NULL, *play = NULL;
            int err = pcm_open(&cap, SND_PCM_STREAM_CAPTURE, candidate);
            if (err == 0) {

                err = pcm_open(&play, SND_PCM_STREAM_PLAYBACK, candidate);
                if (err == 0) {
                    g_cap = cap;
                    g_play = play;
                    dev = candidate;
                    break;
                }
                snd_pcm_close(cap);
            }
            if (err != 0) {
                last_err = err;
                last_dev = candidate;
            }
        }
        if (!dev) {
            fprintf(stderr,
                    "no usable ALSA device (last %s: %s; busy device or missing permission?)\n",
                    last_dev ? last_dev : "-", last_err ? snd_strerror(last_err) : "-");
            return 1;
        }
    }

    if (!g_cap) {

        int err = pcm_open(&g_cap, SND_PCM_STREAM_CAPTURE, dev);
        if (err < 0) {
            fprintf(stderr, "open capture %s: %s\n", dev, snd_strerror(err));
            return 1;
        }
        int perr = pcm_open(&g_play, SND_PCM_STREAM_PLAYBACK, dev);
        if (perr < 0) {
            fprintf(stderr, "open playback %s: %s\n", dev, snd_strerror(perr));
            return 1;
        }
    }
    fprintf(stderr, "[alsa] %s real mmap (%uHz/%uch) -> engine %dHz/%dch frame=%d\n", dev,
            pcm_rate_for(SND_PCM_STREAM_PLAYBACK), pcm_channels_for(SND_PCM_STREAM_PLAYBACK),
            ENGINE_RATE, ENGINE_CHANNELS, ENGINE_BLOCK_FRAMES);

    dsp_t *dsp = NULL;
    /* Debug-only runtime switches; documented in README. */
    g_dsp_enabled = getenv("BYPASS_DSP") == NULL;
    int aec_enabled = getenv("NO_AEC") == NULL;
    int pre_enabled = getenv("NO_NS") == NULL;
    g_aec_enabled = aec_enabled;
    if (dsp_init(&dsp, aec_enabled, pre_enabled) < 0) {
        fprintf(stderr, "dsp init failed\n");
        return 1;
    }
    channels_init();
    hw_volume_init();
    if (ipc_init() < 0) {
        fprintf(stderr, "ipc init failed\n");
        return 1;
    }

    pthread_t pt, rt;
    int pthread_err = pthread_create(&pt, NULL, playback_thread, NULL);
    if (pthread_err != 0) {
        fprintf(stderr, "pthread_create playback: %s\n", strerror(pthread_err));
        dsp_free(dsp);
        snd_pcm_close(g_play);
        snd_pcm_close(g_cap);
        return 1;
    }
    fprintf(stderr, "[engine] playback thread started\n");
    pthread_err = pthread_create(&rt, NULL, record_thread, dsp);
    if (pthread_err != 0) {
        fprintf(stderr, "pthread_create record: %s\n", strerror(pthread_err));
        g_pcm_quit = 1;
        pthread_join(pt, NULL);
        dsp_free(dsp);
        snd_pcm_close(g_play);
        snd_pcm_close(g_cap);
        return 1;
    }
    fprintf(stderr, "[engine] record thread started\n");

    while (!g_quit && !g_pcm_quit) {
        ipc_poll_step(200);
    }

    fprintf(stderr, "[engine] exit\n");
    g_pcm_quit = 1;
    pthread_join(rt, NULL);
    pthread_join(pt, NULL);
    dsp_free(dsp);
    snd_pcm_close(g_play);
    snd_pcm_close(g_cap);
    return 0;
}
