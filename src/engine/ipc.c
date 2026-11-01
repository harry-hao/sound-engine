/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE /* for SO_PEERCRED */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "engine.h"

channel_t g_channels[AUDIO_CHANNEL_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_id = 1;
static int g_lfd = -1;

void channels_init(void)
{
    memset(g_channels, 0, sizeof(g_channels));
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++) {
        g_channels[i].volume = 1.0f;
    }
}

void channels_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

void channels_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static const char *channel_name(uint32_t channel)
{
    switch (channel) {
    case CHANNEL_MEDIA:
        return "media";
    case CHANNEL_NAV:
        return "nav";
    case CHANNEL_ALERT:
        return "alert";
    case CHANNEL_LOOPBACK:
        return "loopback";
    case CHANNEL_MIC:
        return "mic";
    case CHANNEL_MIC_RAW:
        return "mic_raw";
    default:
        return "unknown";
    }
}

static void shm_name(uint32_t id, char *buf, size_t len)
{
    snprintf(buf, len, SHM_NAME_PREFIX "%u", id);
}

static ring_hdr_t *create_ring(uint32_t id, audio_format_t format)
{
    char name[64];
    shm_name(id, name, sizeof(name));
    shm_unlink(name);

    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        shm_unlink(name);
        return NULL;
    }

    size_t len =
        sizeof(ring_hdr_t) + (size_t)STREAM_RING_FRAMES * format.channels * sizeof(int16_t);
    if (ftruncate(fd, (off_t)len) < 0) {
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    ring_hdr_t *ring = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ring == MAP_FAILED) {
        shm_unlink(name);
        return NULL;
    }

    memset(ring, 0, len);
    ring->magic = RING_MAGIC;
    ring->capacity = STREAM_RING_FRAMES;
    ring->channels = format.channels;
    ring->rate = format.rate;
    return ring;
}

static void destroy_ring(const ring_hdr_t *ring, uint32_t id)
{
    char name[64];
    shm_name(id, name, sizeof(name));
    shm_unlink(name);
    munmap((void *)ring, ring_bytes(ring));
}

static channel_t *alloc_channel(uint32_t channel, float volume, audio_format_t format,
                                uint32_t *out_id)
{
    channel_t *target = &g_channels[channel];
    if (target->used) {
        return NULL;
    }

    uint32_t id = g_next_id++;
    ring_hdr_t *ring = create_ring(id, format);
    if (!ring) {
        return NULL;
    }

    target->used = 1;
    target->volume = volume;
    target->format = format;
    target->ring = ring;
    target->shm_id = id;
    target->fd = -1;
    *out_id = id;
    return target;
}

static void free_channel(uint32_t channel)
{
    channel_t *target = &g_channels[channel];
    if (!target->used) {
        return;
    }

    if (target->fd >= 0) {
        close(target->fd);
        target->fd = -1;
    }
    ring_hdr_t *ring = target->ring;
    uint32_t id = target->shm_id;
    target->used = 0;
    resampler_destroy(&target->resampler);
    target->ring = NULL;
    destroy_ring(ring, id);
}

static void reject(int fd, ctl_msg_t msg)
{
    msg.shm_id = 0;
    msg.capacity_frames = 0;
    send(fd, &msg, sizeof(msg), 0);
    close(fd);
}

static int cmd_to_channel(const ctl_msg_t *msg)
{
    if (msg->cmd == CMD_PLAY) {
        return (msg->channel < AUDIO_PLAY_CHANNEL_COUNT) ? (int)msg->channel : -1;
    }
    if (msg->cmd == CMD_RECORD) {
        return CHANNEL_MIC;
    }
    if (msg->cmd == CMD_RECORD_RAW) {
        return CHANNEL_MIC_RAW;
    }
    return -1;
}

static int play_format_valid(const ctl_msg_t *msg)
{
    static const uint32_t rates[] = {16000, 32000, 44100, 48000};

    if (msg->channels == 0 || msg->channels > AUDIO_MAX_CHANNELS) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (msg->rate == rates[i]) {
            return 1;
        }
    }
    return 0;
}

/*
 * Negotiate the client-engine ringbuffer format for a registration request.
 *
 * Request type            Ring format        Converted by
 * ----------------------- ------------------ -------------------------------
 * plain play stream       client's own       mixer (to hw format)
 * mic playback stream     client's own       mixer (to hw format)
 * raw record stream       capture native     none (pre-DSP passthrough)
 * clean record stream     engine 16k/mono    none (DSP output is this format)
 */
static audio_format_t format_for_request(const ctl_msg_t *msg)
{
    audio_format_t format = {.rate = ENGINE_RATE, .channels = 1};

    if (msg->cmd == CMD_PLAY) {
        format.rate = msg->rate;
        format.channels = msg->channels;
    } else if (msg->cmd == CMD_RECORD_RAW) {
        format.rate = pcm_rate_for(SND_PCM_STREAM_CAPTURE);
        format.channels = pcm_channels_for(SND_PCM_STREAM_CAPTURE) > AUDIO_MAX_CHANNELS
                              ? AUDIO_MAX_CHANNELS
                              : (uint16_t)pcm_channels_for(SND_PCM_STREAM_CAPTURE);
    }

    return format;
}

/* Allow owner to access; the peer's uid is returned via out_uid (for logging). */
static int peer_allowed(int fd, uid_t *out_uid)
{
    *out_uid = -1;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        return 0; /* Reject */
    }
    *out_uid = cred.uid;
    return cred.uid == geteuid();
}

static int handle_control(int client_fd, ctl_msg_t *msg)
{
    switch (msg->cmd) {
    case CMD_TOGGLE_AEC:
        g_aec_enabled = msg->channel == 1;
        fprintf(stderr, "[ipc] aec_enabled=%d\n", g_aec_enabled);
        msg->channel = (uint32_t)g_aec_enabled;
        break;
    case CMD_SET_VOLUME: {
        int new_pct = hw_volume_set((int)msg->volume);
        if (new_pct >= 0) {
            fprintf(stderr, "[ipc] hw volume=%d%%\n", new_pct);
        }
        msg->volume = new_pct < 0 ? (float)hw_volume_get() : (float)new_pct;
        break;
    }
    case CMD_STATUS:
        msg->volume = (float)hw_volume_get();
        msg->channel = (uint32_t)atomic_load_explicit(&g_play_level_pct, memory_order_relaxed);
        break;
    default:
        return 0;
    }

    send(client_fd, msg, sizeof(*msg), 0);
    return 1;
}

static void handle_registration(int client_fd, ctl_msg_t *msg)
{
    int channel = cmd_to_channel(msg);
    if (channel < 0) {
        reject(client_fd, *msg);
        return;
    }
    if (msg->cmd == CMD_PLAY && !play_format_valid(msg)) {
        reject(client_fd, *msg);
        return;
    }

    audio_format_t format = format_for_request(msg);
    channels_lock();
    uint32_t id = 0;
    channel_t *target = alloc_channel((uint32_t)channel, msg->volume, format, &id);
    channels_unlock();

    msg->shm_id = id;
    msg->capacity_frames = STREAM_RING_FRAMES;
    msg->rate = format.rate;
    msg->channels = format.channels;
    send(client_fd, msg, sizeof(*msg), 0);
    fprintf(stderr, "[ipc] channel %s #%u %s\n", channel_name((uint32_t)channel), id,
            target ? "ok" : "BUSY/FAILED");
    if (!target) {
        close(client_fd);
        return;
    }

    channels_lock();
    target->fd = client_fd;
    channels_unlock();
}

static void handle_accept(int listen_fd)
{
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        return;
    }

    uid_t peer_uid;
    if (!peer_allowed(client_fd, &peer_uid)) {
        fprintf(stderr, "[ipc] rejected connection from uid=%d\n", (int)peer_uid);
        close(client_fd);
        return;
    }

    ctl_msg_t msg;
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    size_t got = recv(client_fd, &msg, sizeof(msg), MSG_WAITALL);
    if (got != sizeof(msg)) {

        fprintf(stderr, "[ipc] registration failed: got %zu/%zu bytes\n", got, sizeof(msg));
        close(client_fd);
        return;
    }

    if (handle_control(client_fd, &msg)) {
        close(client_fd);
        return;
    }
    handle_registration(client_fd, &msg);
}

int ipc_init(void)
{
    /* Clear residual remainings. */
    for (uint32_t i = 1; i < 256; i++) {
        char name[64];
        shm_name(i, name, sizeof(name));
        shm_unlink(name);
    }
    unlink(CTL_SOCK_PATH);

    /* Open listening socket. */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    /* Bind to the control socket path. */
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    /* Set permission owner-only for the control socket. Also checked in peer_allow. */
    if (chmod(CTL_SOCK_PATH, 0600) < 0) {
        perror("chmod socket");
    }

    /* Start listening for incoming connections. */
    if (listen(listen_fd, 8) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    g_lfd = listen_fd;
    fprintf(stderr, "[ipc] listening on %s\n", CTL_SOCK_PATH);
    return 0;
}

typedef struct {
    int channel;
    uint32_t shm_id;
} channel_ref_t;

void ipc_poll_step(int timeout_ms)
{
    channel_ref_t refs[AUDIO_CHANNEL_COUNT];

    /* fds[0] = listen socket; fds[1..N] = active channel sockets. */
    struct pollfd fds[AUDIO_CHANNEL_COUNT + 1];
    int count = 0;

    fds[count].fd = g_lfd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    count++;

    channels_lock();
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++) {
        channel_t *channel = &g_channels[i];
        if (!channel->used || channel->fd < 0) {
            continue;
        }
        refs[count - 1].channel = i;
        refs[count - 1].shm_id = channel->shm_id;
        fds[count].fd = channel->fd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        count++;
    }
    channels_unlock();

    if (poll(fds, count, timeout_ms) <= 0) {
        return;
    }

    /* Handle active channels */
    for (int i = 1; i < count; i++) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }

        ctl_msg_t msg;
        ssize_t got = recv(fds[i].fd, &msg, sizeof(msg), MSG_DONTWAIT);
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }

        int dead = got == 0;
        if (got == (ssize_t)sizeof(msg)) {
            if (msg.cmd == CMD_CLOSE) {
                dead = 1;
            } else if (handle_control(fds[i].fd, &msg)) {

                continue;
            } else {
                fprintf(stderr, "[ipc] unexpected cmd %u on stream #%u\n", msg.cmd,
                        refs[i - 1].shm_id);
                continue;
            }
        } else if (got > 0) {
            fprintf(stderr, "[ipc] protocol error on stream #%u: got %zd bytes\n",
                    refs[i - 1].shm_id, got);
            dead = 1;
        }
        if (!dead) {
            continue;
        }

        fprintf(stderr, "[ipc] stream #%u closed\n", refs[i - 1].shm_id);
        channels_lock();
        free_channel((uint32_t)refs[i - 1].channel);
        channels_unlock();
    }

    /* Handle new connections */
    if (fds[0].revents & POLLIN) {
        handle_accept(g_lfd);
    }
}
