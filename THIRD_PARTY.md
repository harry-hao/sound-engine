# Third-party dependencies

This project (its own source code) is MIT licensed — see `LICENSE`.
It links against the following third-party libraries at runtime:

| Library      | Link     | License    | Upstream |
|--------------|----------|------------|----------|
| alsa-lib     | dynamic  | LGPL-2.1   | https://github.com/alsa-project/alsa-lib |
| speexdsp     | dynamic  | Xiph BSD   | https://github.com/xiph/speexdsp |
| FFmpeg libswresample / libavutil | dynamic | LGPL-2.1+ | https://ffmpeg.org |

Dynamic linking is used for all LGPL libraries; the LGPL license texts apply
to those libraries themselves, not to this project's source code.

## Assets

# Audio Asset Credits

| File | Source | License |
|------|--------|---------|
| `assets/media.wav` | Erik Satie -- Gymnopédie No.1, Wikimedia Commons | Public domain |
| `assets/nav.mp3` | Universfield -- "Let's Go", Pixabay | Pixabay Content License (free use) |
| `assets/alert.wav` | ALSA project -- Front_Center.wav | LGPL 2.1 (ALSA test file) |

Audio files are derived from the above sources via ffmpeg, they carry the same
license as their originals.
