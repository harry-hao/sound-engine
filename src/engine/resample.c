/* SPDX-License-Identifier: MIT */
#include "resample.h"

#include <errno.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int16_t data[AUDIO_MAX_BLOCK_SAMPLES];
    size_t read;
    size_t write;
} fifo_t;

struct resampler {
    unsigned in_rate;
    unsigned out_rate;
    unsigned in_channels;
    unsigned out_channels;
    SwrContext *state;
    fifo_t fifo;
};

static size_t fifo_used(const fifo_t *fifo)
{
    return fifo->write - fifo->read;
}

static int fifo_push(fifo_t *fifo, const int16_t *samples, size_t samples_count)
{
    const size_t capacity = sizeof(fifo->data) / sizeof(fifo->data[0]);

    if (samples_count > capacity - fifo_used(fifo)) {
        return -ENOBUFS;
    }

    for (size_t i = 0; i < samples_count; i++) {
        fifo->data[fifo->write++ % capacity] = samples[i];
    }
    return 0;
}

static void fifo_pull(fifo_t *fifo, int16_t *samples, size_t samples_count)
{
    const size_t capacity = sizeof(fifo->data) / sizeof(fifo->data[0]);

    for (size_t i = 0; i < samples_count; i++) {
        samples[i] = fifo->data[fifo->read++ % capacity];
    }
}

int resampler_create(resampler_t **resampler, unsigned in_rate, unsigned out_rate,
                     unsigned in_channels, unsigned out_channels)
{
    if (resampler == NULL || in_rate == 0 || out_rate == 0 || in_channels == 0 ||
        out_channels == 0 || in_channels > AUDIO_MAX_CHANNELS ||
        out_channels > AUDIO_MAX_CHANNELS) {
        return -EINVAL;
    }

    resampler_t *instance = calloc(1, sizeof(resampler_t));
    if (instance == NULL) {
        return -ENOMEM;
    }

    instance->in_rate = in_rate;
    instance->out_rate = out_rate;
    instance->in_channels = in_channels;
    instance->out_channels = out_channels;
    if (in_rate != out_rate || in_channels != out_channels) {
        AVChannelLayout in_layout;
        AVChannelLayout out_layout;

        av_channel_layout_default(&in_layout, (int)in_channels);
        av_channel_layout_default(&out_layout, (int)out_channels);
        int err =
            swr_alloc_set_opts2(&instance->state, &out_layout, AV_SAMPLE_FMT_S16, (int)out_rate,
                                &in_layout, AV_SAMPLE_FMT_S16, (int)in_rate, 0, NULL);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (err < 0 || swr_init(instance->state) < 0) {
            if (instance->state != NULL) {
                swr_free(&instance->state);
            }
            free(instance);
            return -EIO;
        }
    }

    *resampler = instance;
    return 0;
}

void resampler_destroy(resampler_t **resampler)
{
    if (resampler == NULL || *resampler == NULL) {
        return;
    }

    if ((*resampler)->state != NULL) {
        swr_free(&(*resampler)->state);
    }
    free(*resampler);
    *resampler = NULL;
}

static int do_resample(resampler_t *resampler, const int16_t *input, unsigned input_frames,
                       int16_t *output, unsigned output_frames)
{

    const uint8_t *input_ptr = (const uint8_t *)input;
    uint8_t *output_ptr = (uint8_t *)output;
    int actual = swr_convert(resampler->state, &output_ptr, (int)output_frames, &input_ptr,
                             (int)input_frames);

    return actual < 0 ? -EIO : actual;
}

int resampler_process(resampler_t *resampler, const int16_t *input, unsigned input_frames,
                      int16_t *output, unsigned output_frames)
{
    if (resampler == NULL || input == NULL || output == NULL) {
        return -EINVAL;
    }

    if (input_frames == 0 || output_frames == 0 || input_frames > AUDIO_MAX_BLOCK_FRAMES ||
        output_frames > AUDIO_MAX_BLOCK_FRAMES ||
        input_frames > AUDIO_MAX_BLOCK_SAMPLES / resampler->in_channels ||
        output_frames > AUDIO_MAX_BLOCK_SAMPLES / resampler->out_channels) {
        return -EINVAL;
    }

    if (resampler->in_rate == resampler->out_rate &&
        resampler->in_channels == resampler->out_channels) {
        if (input_frames != output_frames) {
            return -EINVAL;
        }
        memcpy(output, input, (size_t)input_frames * resampler->in_channels * sizeof(*input));
        return 0;
    }

    if (resampler->state == NULL) {
        return -EINVAL;
    }

    int16_t converted[AUDIO_MAX_BLOCK_SAMPLES];
    int converted_frames;
    int err;
    size_t output_samples = (size_t)output_frames * resampler->out_channels;

    /* Do what we came for */
    converted_frames =
        do_resample(resampler, input, input_frames, converted, AUDIO_MAX_BLOCK_FRAMES);
    if (converted_frames < 0) {
        return converted_frames;
    }

    /* Push the converted samples into the FIFO. */
    err =
        fifo_push(&resampler->fifo, converted, (size_t)converted_frames * resampler->out_channels);
    if (err < 0) {
        return err;
    }

    /* Pull samples from the FIFO, zero the rest if any. */
    size_t avail = fifo_used(&resampler->fifo);
    size_t pull = avail < output_samples ? avail : output_samples;

    fifo_pull(&resampler->fifo, output, pull);
    if (pull < output_samples) {
        memset(output + pull, 0, (output_samples - pull) * sizeof(*output));
    }
    return 0;
}