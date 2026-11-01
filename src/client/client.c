/* SPDX-License-Identifier: MIT */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "client.h"

void ac_msleep(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int ac_open_channel_format(int cmd, float volume, uint32_t channel, uint32_t rate,
                           uint16_t channels, ring_hdr_t **ring, int *sock_fd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect engine");
        close(fd);
        return -1;
    }
    ctl_msg_t msg = {.cmd = (uint32_t)cmd,
                     .volume = volume,
                     .channel = channel,
                     .rate = rate,
                     .channels = channels};
    if (send(fd, &msg, sizeof(msg), 0) != sizeof(msg)) {
        fprintf(stderr, "engine send failed\n");
        close(fd);
        return -1;
    }
    if (recv(fd, &msg, sizeof(msg), MSG_WAITALL) != sizeof(msg)) {
        fprintf(stderr, "engine reply failed\n");
        close(fd);
        return -1;
    }
    if (msg.shm_id == 0) {
        fprintf(stderr, "engine refused stream\n");
        close(fd);
        return -1;
    }
    char name[64];
    snprintf(name, sizeof(name), SHM_NAME_PREFIX "%u", msg.shm_id);
    int shmfd = shm_open(name, O_RDWR, 0600);
    if (shmfd < 0) {
        perror("shm_open");
        close(fd);
        return -1;
    }
    if (msg.channels == 0 || msg.channels > AUDIO_MAX_CHANNELS || msg.capacity_frames == 0 ||
        msg.capacity_frames > STREAM_RING_FRAMES || msg.rate == 0) {
        fprintf(stderr, "engine returned invalid ring format\n");
        close(fd);
        return -1;
    }
    size_t len = sizeof(ring_hdr_t) + (size_t)msg.capacity_frames * msg.channels * sizeof(int16_t);
    ring_hdr_t *r = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    close(shmfd);
    if (r == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }
    if (r->magic != RING_MAGIC || r->capacity != msg.capacity_frames ||
        r->channels != msg.channels || r->rate != msg.rate) {
        fprintf(stderr, "engine returned inconsistent shared memory\n");
        munmap(r, len);
        close(fd);
        return -1;
    }
    *ring = r;
    *sock_fd = fd;
    return 0;
}

int ac_open_channel(int cmd, float volume, uint32_t channel, ring_hdr_t **ring, int *sock_fd)
{
    return ac_open_channel_format(cmd, volume, channel, AC_RATE, 1, ring, sock_fd);
}

static int peer_closed(int sock_fd)
{
    if (sock_fd < 0) {
        return 0;
    }
    uint8_t probe;
    ssize_t got = recv(sock_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (got > 0) {
        return 0;
    }
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    return 1;
}

int ac_ring_write_wait(ring_hdr_t *r, int sock_fd, const int16_t *data, int frames)
{
    if (r->magic != RING_MAGIC || r->capacity == 0 || r->channels == 0 ||
        r->channels > AUDIO_MAX_CHANNELS) {
        return -1;
    }
    while (ring_write(r, data, frames) < 0) {
        if (peer_closed(sock_fd)) {
            return -1;
        }
        ac_msleep(2);
    }
    return frames;
}

int ac_ring_read_wait(ring_hdr_t *r, int sock_fd, int16_t *data, int frames)
{
    if (r->magic != RING_MAGIC || r->capacity == 0 || r->channels == 0 ||
        r->channels > AUDIO_MAX_CHANNELS) {
        return -1;
    }
    while (1) {
        int got = ring_read(r, data, frames);
        if (got > 0) {
            return got;
        }
        if (peer_closed(sock_fd)) {
            return -1;
        }
        ac_msleep(2);
    }
}
