/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <errno.h>
#include "engine.h"

static snd_mixer_t *mixer = NULL;
static snd_mixer_elem_t *volume_elem = NULL;
static pthread_mutex_t mixer_mu = PTHREAD_MUTEX_INITIALIZER;

static int volume_usable(snd_mixer_elem_t *elem)
{
    return snd_mixer_selem_is_active(elem) && snd_mixer_selem_has_playback_volume(elem);
}

int hw_volume_init(void)
{
    pthread_mutex_lock(&mixer_mu);
    if (volume_elem) {
        pthread_mutex_unlock(&mixer_mu);
        return 0;
    }

    static const char *mix_devices[] = {"default", "hw:0"};
    int err = -ENOENT;
    for (size_t i = 0; i < sizeof(mix_devices) / sizeof(mix_devices[0]); i++) {
        err = snd_mixer_open(&mixer, 0);
        if (err == 0) {
            err = snd_mixer_attach(mixer, mix_devices[i]);
        }
        if (err == 0) {
            err = snd_mixer_selem_register(mixer, NULL, NULL);
        }
        if (err == 0) {
            err = snd_mixer_load(mixer);
        }
        if (err == 0) {
            break;
        }
        fprintf(stderr, "[vol] %s: %s\n", mix_devices[i], snd_strerror(err));
        snd_mixer_close(mixer);
        mixer = NULL;
    }
    if (err == 0) {
        static const char *preferred[] = {"Master", "PCM", "Speaker"};
        snd_mixer_elem_t *fallback = NULL;
        for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mixer); elem;
             elem = snd_mixer_elem_next(elem)) {
            if (!volume_usable(elem)) {
                continue;
            }
            for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
                if (strcmp(snd_mixer_selem_get_name(elem), preferred[i]) == 0) {
                    volume_elem = elem;
                    break;
                }
            }
            if (volume_elem) {
                break;
            }
            if (!fallback) {
                fallback = elem;
            }
        }
        if (!volume_elem) {
            volume_elem = fallback;
        }
    }
    if (err == 0 && volume_elem) {
        long min = 0, max = 0, value = 0;
        if (snd_mixer_selem_get_playback_volume_range(volume_elem, &min, &max) == 0 &&
            snd_mixer_selem_get_playback_volume(volume_elem, SND_MIXER_SCHN_MONO, &value) == 0 &&
            max > min) {
            g_hw_volume_pct = (int)((value - min) * 100 / (max - min));
        }
    } else {
        if (err == 0) {
            err = -ENOENT;
        }
        if (mixer) {
            snd_mixer_close(mixer);
            mixer = NULL;
        }
    }
    pthread_mutex_unlock(&mixer_mu);
    if (err < 0) {
        fprintf(stderr, "[vol] hardware volume unavailable: %s\n", snd_strerror(err));
    } else {
        fprintf(stderr, "[vol] using %s at %d%%\n", snd_mixer_selem_get_name(volume_elem),
                g_hw_volume_pct);
    }
    return err;
}

int hw_volume_get(void)
{
    if (!volume_elem) {
        return -ENOENT;
    }
    long min = 0, max = 0, value = 0;
    pthread_mutex_lock(&mixer_mu);
    int err = snd_mixer_selem_get_playback_volume_range(volume_elem, &min, &max);
    if (err == 0) {
        err = snd_mixer_selem_get_playback_volume(volume_elem, SND_MIXER_SCHN_MONO, &value);
    }
    if (err == 0 && max > min) {
        g_hw_volume_pct = (int)((value - min) * 100 / (max - min));
        err = g_hw_volume_pct;
    } else if (err == 0) {
        err = -EINVAL;
    }
    pthread_mutex_unlock(&mixer_mu);
    return err;
}

int hw_volume_set(int pct)
{
    if (pct < 0 || pct > 100) {
        return -EINVAL;
    }
    if (!volume_elem) {
        return -ENOENT;
    }
    pthread_mutex_lock(&mixer_mu);
    long min = 0, max = 0;
    int err = snd_mixer_selem_get_playback_volume_range(volume_elem, &min, &max);
    if (err == 0) {
        long value = min + (long)(((max - min) * pct) / 100.0 + 0.5);
        err = snd_mixer_selem_set_playback_volume_all(volume_elem, value);
    }
    long value = 0;
    if (err == 0) {
        err = snd_mixer_selem_get_playback_volume(volume_elem, SND_MIXER_SCHN_MONO, &value);
    }
    if (err == 0 && max > min) {
        g_hw_volume_pct = (int)((value - min) * 100 / (max - min));
        pthread_mutex_unlock(&mixer_mu);
        return g_hw_volume_pct;
    }
    if (err == 0) {
        err = -EINVAL;
    }
    pthread_mutex_unlock(&mixer_mu);
    return err;
}
