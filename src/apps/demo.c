/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "client.h"

static volatile sig_atomic_t g_sigint = 0;
static atomic_bool g_stop = false;
static atomic_int g_aec = 1;
static atomic_int g_nav_active = 0;
static atomic_int g_alert_active = 0;
static atomic_int g_hold_media = 0;
static atomic_int g_hw_vol_pct = -1;
static atomic_int g_level_pct = 0;
static int g_ctl_failures = 0;
static pthread_mutex_t g_ui_mu = PTHREAD_MUTEX_INITIALIZER;

/*
 * Recording session state and its backing buffer are protected by this
 * mutex. g_stop is the only cross-thread shutdown flag; audio rings use
 * their own acquire/release atomics in ring.h.
 */
static pthread_mutex_t g_rec_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_recording = 0;
static long g_rec_frames = 0;
static int16_t *g_rec_buf;
static long g_rec_cap_frames;

typedef struct {
    int16_t *pcm;
    int frames;
    uint32_t rate;
    uint16_t channels;
} clip_t;

static int clip_load(clip_t *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not WAV\n", path);
        fclose(f);
        return -1;
    }
    uint32_t rate = 0, channels = 0, bits = 0, data_size = 0;
    long data_offset = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            break;
        }
        uint32_t sz = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            if (fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) {
                break;
            }
            if (sz > sizeof(fmt)) {
                fseek(f, (long)sz - sizeof(fmt), SEEK_CUR);
            }
            channels = fmt[2] | fmt[3] << 8;
            rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24;
            bits = fmt[14] | fmt[15] << 8;
        } else if (!memcmp(ch, "data", 4)) {
            data_size = sz;
            data_offset = ftell(f);
            break;
        } else {
            fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
        }
    }
    if ((rate != 16000 && rate != 32000 && rate != 44100 && rate != 48000) ||
        (channels != 1 && channels != 2) || bits != 16 || data_size == 0) {
        fprintf(stderr, "%s: need 16k/32k/44.1k/48k, mono/stereo, s16 (got %ukHz/%uch/%ubit)\n",
                path, rate / 1000, channels, bits);
        fclose(f);
        return -1;
    }
    c->rate = rate;
    c->channels = (uint16_t)channels;
    c->frames = (int)(data_size / sizeof(int16_t) / channels);
    c->pcm = malloc((size_t)c->frames * c->channels * sizeof(int16_t));
    if (!c->pcm) {
        fclose(f);
        return -1;
    }
    fseek(f, data_offset, SEEK_SET);
    if (fread(c->pcm, sizeof(int16_t), (size_t)c->frames * c->channels, f) !=
        (size_t)c->frames * c->channels) {
        fprintf(stderr, "%s: short read\n", path);
        free(c->pcm);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int clip_block_frames(const clip_t *clip)
{
    return (int)(clip->rate / (1000 / AC_BLOCK_MS));
}

static void clip_free(clip_t *c)
{
    free(c->pcm);
    c->pcm = NULL;
}

static struct termios g_saved;
static void tty_raw(void)
{
    tcgetattr(STDIN_FILENO, &g_saved);
    struct termios raw = g_saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void tty_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
}

static ring_hdr_t *g_media_ring, *g_nav_ring, *g_alert_ring, *g_loop_ring;
static ring_hdr_t *g_mic_ring;
static int g_media_sock = -1, g_nav_sock = -1, g_alert_sock = -1, g_loop_sock = -1;
static int g_mic_sock = -1;

static void on_sigint(int s)
{
    (void)s;
    g_sigint = 1;
    if (g_media_sock >= 0) {
        shutdown(g_media_sock, SHUT_RDWR);
    }
    if (g_nav_sock >= 0) {
        shutdown(g_nav_sock, SHUT_RDWR);
    }
    if (g_alert_sock >= 0) {
        shutdown(g_alert_sock, SHUT_RDWR);
    }
    if (g_loop_sock >= 0) {
        shutdown(g_loop_sock, SHUT_RDWR);
    }
    if (g_mic_sock >= 0) {
        shutdown(g_mic_sock, SHUT_RDWR);
    }
}

#define MAX_RECORD_SECS 60

static void draw_status(void);

static int recording_active(void)
{
    pthread_mutex_lock(&g_rec_mu);
    int active = g_recording;
    pthread_mutex_unlock(&g_rec_mu);
    return active;
}

static void recording_start(void)
{
    pthread_mutex_lock(&g_rec_mu);
    g_recording = 0; /* Drop any block that was already read by an old loop. */
    g_rec_frames = 0;
    g_recording = 1;
    pthread_mutex_unlock(&g_rec_mu);
}

static long recording_stop(void)
{
    pthread_mutex_lock(&g_rec_mu);
    g_recording = 0;
    long frames = g_rec_frames;
    pthread_mutex_unlock(&g_rec_mu);
    return frames;
}

static void recording_store(const int16_t *buf, int frames, uint32_t stride, uint32_t rate)
{
    int stopped_by_limit = 0;
    pthread_mutex_lock(&g_rec_mu);
    if (g_recording) {
        if (g_rec_frames + frames > MAX_RECORD_SECS * (long)rate) {
            g_recording = 0;
            stopped_by_limit = 1;
        } else if (g_rec_frames + frames > g_rec_cap_frames) {
            long need = g_rec_frames + frames;
            int16_t *grown = realloc(g_rec_buf, (size_t)need * stride * sizeof(*g_rec_buf));
            if (grown != NULL) {
                g_rec_buf = grown;
                g_rec_cap_frames = need;
            }
        }
        if (g_recording && g_rec_frames + frames <= g_rec_cap_frames) {
            memcpy(g_rec_buf + (size_t)g_rec_frames * stride, buf,
                   (size_t)frames * stride * sizeof(*g_rec_buf));
            g_rec_frames += frames;
        } else if (g_recording) {
            g_recording = 0;
            stopped_by_limit = 1;
        }
    }
    pthread_mutex_unlock(&g_rec_mu);
    if (stopped_by_limit) {
        draw_status();
    }
}

static void draw_status(void)
{
    int recording = recording_active();
    pthread_mutex_lock(&g_ui_mu);
    printf("\r\033[K");
    char vol[8];
    if (g_hw_vol_pct < 0) {
        snprintf(vol, sizeof(vol), "-");
    } else {
        snprintf(vol, sizeof(vol), "%d", g_hw_vol_pct);
    }
    printf("MEDIA:%-4s NAV:%-4s ALERT:%-4s │ VOL %3s%% │ LVL %3d%% │ REC %-8s │ AEC %s │ "
           "[n]av [a]lert [r]ec [p]lay [c]aec [+/-]vol [q]uit",
           (g_nav_active || g_alert_active) ? "DUCK" : "PLAY",
           g_alert_active ? "DUCK"
           : g_nav_active ? "PLAY"
                          : "IDLE",
           g_alert_active ? "PLAY" : "IDLE", vol, g_level_pct,
           recording ? "ON" : (g_hold_media ? "PLAY" : "OFF"), g_aec ? "ON" : "OFF");
    fflush(stdout);
    pthread_mutex_unlock(&g_ui_mu);
}

static void *demo_media_thread(void *arg)
{
    clip_t *clip = (clip_t *)arg;
    int pos = 0;
    int block = clip_block_frames(clip);
    while (!atomic_load(&g_stop)) {
        while (atomic_load(&g_hold_media) && !atomic_load(&g_stop)) {
            usleep(20000);
        }
        int n = (pos + block <= clip->frames) ? block : clip->frames - pos;
        if (n <= 0) {
            pos = 0;
            continue;
        }
        if (ac_ring_write_wait(g_media_ring, g_media_sock, clip->pcm + (size_t)pos * clip->channels,
                               n) < 0) {
            break;
        }
        pos += n;
    }
    return NULL;
}

static void play_clip(ring_hdr_t *ring, int sock, const clip_t *clip, const char *name)
{
    atomic_int *active = name[0] == 'n' ? &g_nav_active : &g_alert_active;
    *active = 1;
    draw_status();
    int block = clip_block_frames(clip);
    int pos = 0;
    while (pos < clip->frames && !atomic_load(&g_stop)) {
        int n = (pos + block <= clip->frames) ? block : clip->frames - pos;
        if (ac_ring_write_wait(ring, sock, clip->pcm + (size_t)pos * clip->channels, n) < 0) {
            break;
        }
        pos += n;
    }
    *active = 0;
    draw_status();
}

static void *demo_record_thread(void *arg)
{
    (void)arg;
    static int16_t buf[AC_MAX_SAMPLES];

    while (!atomic_load(&g_stop)) {
        if (!recording_active()) {
            ring_clear(g_mic_ring);
            usleep(10000);
            continue;
        }

        uint32_t stride = g_mic_ring->channels;
        uint32_t block = ac_block_frames(g_mic_ring);
        int n = ac_ring_read_wait(g_mic_ring, g_mic_sock, buf, (int)block);
        if (n < 0) {
            break;
        }
        if (n <= 0 || atomic_load(&g_stop)) {
            continue;
        }
        recording_store(buf, n, stride, g_mic_ring->rate);
    }
    return NULL;
}

static int ctl_request(ctl_msg_t *msg)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    send(fd, msg, sizeof(*msg), 0);
    ctl_msg_t reply;
    int rc = recv(fd, &reply, sizeof(reply), MSG_WAITALL) == sizeof(reply) ? 0 : -1;
    close(fd);
    if (rc == 0) {
        *msg = reply;
    }
    return rc;
}

static void send_toggle_aec(void)
{
    int wanted = g_aec ? 0 : 1;
    ctl_msg_t msg = {.cmd = CMD_TOGGLE_AEC, .channel = (uint32_t)wanted};
    if (ctl_request(&msg) == 0) {
        g_aec = msg.channel != 0;
    }
}

static void change_hw_volume(int delta_pct)
{
    int current = g_hw_vol_pct;
    if (current < 0) {
        return;
    }
    int wanted = current + delta_pct;
    if (wanted < 0) {
        wanted = 0;
    } else if (wanted > 100) {
        wanted = 100;
    }
    ctl_msg_t msg = {.cmd = CMD_SET_VOLUME, .volume = (float)wanted};
    if (ctl_request(&msg) == 0 && msg.volume >= 0.0f) {
        g_hw_vol_pct = (int)msg.volume;
    }
}

static void play_recording(void)
{
    /* stop recording and get the total frames recorded */
    long rec_total = recording_stop();
    if (rec_total == 0) {
        return;
    }
    /* hold background media while we play the recording */
    atomic_store(&g_hold_media, 1);
    draw_status();
    uint32_t channels = g_loop_ring->channels;
    uint32_t block = ac_block_frames(g_loop_ring);
    long pos = 0;
    while (pos < rec_total && !atomic_load(&g_stop)) {
        long n = (pos + block <= rec_total) ? block : rec_total - pos;
        if (ac_ring_write_wait(g_loop_ring, g_loop_sock, g_rec_buf + (size_t)pos * channels,
                               (int)n) < 0) {
            break;
        }
        pos += n;
    }
    atomic_store(&g_hold_media, 0);
    draw_status();
}

static void *demo_status_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        ctl_msg_t msg = {.cmd = CMD_STATUS};
        if (ctl_request(&msg) == 0) {
            g_hw_vol_pct = (int)msg.volume;
            g_level_pct = (int)msg.channel;
            g_ctl_failures = 0;
            draw_status();
        } else if (++g_ctl_failures >= 5) {
            atomic_store(&g_stop, true);
        }
        usleep(200000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fprintf(stderr, "usage: demo\n");
        fprintf(
            stderr,
            "interactive demo: music loop, nav/alert ducking, mic record/playback, AEC toggle\n");
        fprintf(
            stderr,
            "keys: n=nav, a=alert, r=record start/stop, p=playback, c=AEC, +/-=volume, q=quit\n");
        fprintf(stderr,
                "assets: media.wav/nav.wav/alert.wav next to the binary (mono/stereo S16LE)\n");
        return 0;
    }
    signal(SIGINT, on_sigint);

    char self[4096], dirbuf[4096];
    ssize_t self_len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    const char *dir = "assets";
    if (self_len > 0) {
        self[self_len] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            snprintf(dirbuf, sizeof(dirbuf), "%.*s/assets", (int)(slash - self), self);
            dir = dirbuf;
        }
    }

    char path[4352]; /* dirbuf 4096 + "/alert.wav" */
    clip_t media, nav, alert;
    snprintf(path, sizeof(path), "%s/media.wav", dir);
    if (clip_load(&media, path) < 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "%s/nav.wav", dir);
    if (clip_load(&nav, path) < 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "%s/alert.wav", dir);
    if (clip_load(&alert, path) < 0) {
        return 1;
    }

    if (ac_open_channel_format(CMD_PLAY, 1.0f, CHANNEL_MEDIA, media.rate, media.channels,
                               &g_media_ring, &g_media_sock) < 0) {
        return 1;
    }
    if (ac_open_channel_format(CMD_PLAY, 1.0f, CHANNEL_NAV, nav.rate, nav.channels, &g_nav_ring,
                               &g_nav_sock) < 0) {
        return 1;
    }
    if (ac_open_channel_format(CMD_PLAY, 1.0f, CHANNEL_ALERT, alert.rate, alert.channels,
                               &g_alert_ring, &g_alert_sock) < 0) {
        return 1;
    }
    if (ac_open_channel(CMD_PLAY, 1.0f, CHANNEL_LOOPBACK, &g_loop_ring, &g_loop_sock) < 0) {
        fprintf(stderr, "[demo] playback channel open failed\n");
        return 1;
    }
    if (ac_open_channel(CMD_RECORD, 1.0f, CHANNEL_MIC, &g_mic_ring, &g_mic_sock) < 0) {
        fprintf(stderr, "[demo] mic record channel open failed\n");
    }
    pthread_t tid, ltid, stid;
    pthread_create(&tid, NULL, demo_media_thread, &media);
    pthread_create(&stid, NULL, demo_status_thread, NULL);
    if (g_mic_ring) {
        pthread_create(&ltid, NULL, demo_record_thread, NULL);
    }

    tty_raw();
    printf("ALSA demo\n");
    draw_status();

    while (!atomic_load(&g_stop)) {
        if (g_sigint) {
            break;
        }
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            break;
        }
        if (c == 'n') {
            play_clip(g_nav_ring, g_nav_sock, &nav, "nav");
        } else if (c == 'a') {
            play_clip(g_alert_ring, g_alert_sock, &alert, "alert");
        } else if (c == 'r') {
            if (recording_active()) {
                recording_stop();
            } else {
                recording_start();
            }
            draw_status();
        } else if (c == 'p') {
            play_recording();
        } else if (c == 'c') {
            send_toggle_aec();
            draw_status();
        } else if (c == '+' || c == '=') {
            change_hw_volume(5);
            draw_status();
        } else if (c == '-') {
            change_hw_volume(-5);
            draw_status();
        } else if (c == 'q') {
            break;
        }
    }

    printf("\n[demo] exit\n");
    tty_restore();
    atomic_store(&g_stop, true);
    pthread_join(tid, NULL);
    pthread_join(stid, NULL);
    if (g_mic_ring) {
        pthread_join(ltid, NULL);
    }
    close(g_media_sock);
    close(g_nav_sock);
    close(g_alert_sock);
    close(g_loop_sock);
    if (g_mic_sock >= 0) {
        close(g_mic_sock);
    }
    clip_free(&media);
    clip_free(&nav);
    clip_free(&alert);
    return 0;
}
