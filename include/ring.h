/* SPDX-License-Identifier: MIT */

#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define RING_MAGIC 0x52494E47u /* "RING" */

typedef struct {
    uint32_t magic;
    _Atomic uint32_t read_idx;  /* frames consumed */
    _Atomic uint32_t write_idx; /* frames produced */
    uint32_t capacity;
    uint32_t rate; /* sample rate per frame */
    uint32_t channels;
    int16_t data[]; /* frame data follows */
} ring_hdr_t;

static inline size_t ring_bytes(const ring_hdr_t *r)
{
    return sizeof(*r) + (size_t)r->capacity * r->channels * sizeof(r->data[0]);
}

static inline int16_t *ring_data(ring_hdr_t *r)
{
    return r->data;
}

static inline uint32_t ring_used(const ring_hdr_t *r)
{
    return atomic_load_explicit(&r->write_idx, memory_order_acquire) -
           atomic_load_explicit(&r->read_idx, memory_order_acquire);
}

static inline uint32_t ring_free(const ring_hdr_t *r)
{
    return r->capacity - ring_used(r);
}

static inline int ring_write(ring_hdr_t *r, const int16_t *src, int frames)
{
    if (r->channels == 0) {
        return -1;
    }
    uint32_t write_idx = atomic_load_explicit(&r->write_idx, memory_order_relaxed);
    if ((uint32_t)frames >
        r->capacity - (write_idx - atomic_load_explicit(&r->read_idx, memory_order_acquire))) {
        return -1;
    }
    int done = 0;
    while (done < frames) {
        uint32_t pos = write_idx % r->capacity;
        uint32_t n = (uint32_t)(frames - done) < (r->capacity - pos) ? (uint32_t)(frames - done)
                                                                     : (r->capacity - pos);
        memcpy(ring_data(r) + (size_t)pos * r->channels, src + (size_t)done * r->channels,
               (size_t)n * r->channels * sizeof(int16_t));
        write_idx += n;
        atomic_store_explicit(&r->write_idx, write_idx, memory_order_release);
        done += (int)n;
    }
    return frames;
}

static inline int ring_read(ring_hdr_t *r, int16_t *dst, int frames)
{
    if (r->channels == 0) {
        return -1;
    }
    uint32_t read_idx = atomic_load_explicit(&r->read_idx, memory_order_relaxed);
    uint32_t used = atomic_load_explicit(&r->write_idx, memory_order_acquire) - read_idx;
    if (used > (uint32_t)frames) {
        used = (uint32_t)frames;
    }
    if (used == 0) {
        return 0;
    }
    int done = 0;
    while (done < (int)used) {
        uint32_t pos = read_idx % r->capacity;
        uint32_t todo = used - (uint32_t)done;
        todo = todo < (r->capacity - pos) ? todo : (r->capacity - pos);
        memcpy(dst + (size_t)done * r->channels, ring_data(r) + (size_t)pos * r->channels,
               (size_t)todo * r->channels * sizeof(int16_t));
        read_idx += todo;
        atomic_store_explicit(&r->read_idx, read_idx, memory_order_release);
        done += (int)todo;
    }
    return done;
}

/*
 * Consumer-side helper: discard every currently readable frame. Must be
 * called only by the ring's single consumer.
 */
static inline void ring_clear(ring_hdr_t *r)
{
    uint32_t used = atomic_load_explicit(&r->write_idx, memory_order_acquire);
    atomic_store_explicit(&r->read_idx, used, memory_order_release);
}

#endif
