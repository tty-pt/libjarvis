#ifndef JARVIS_AUDIO_H
#define JARVIS_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jarvis_audio_cap  jarvis_audio_cap_t;
typedef struct jarvis_audio_play jarvis_audio_play_t;

/* Capture stream: signed 16-bit LE mono PCM */
jarvis_audio_cap_t *jarvis_audio_cap_open(int sample_rate, int channels);
int                 jarvis_audio_cap_read(jarvis_audio_cap_t *cap, int16_t *dst, size_t n_samples);
void                jarvis_audio_cap_close(jarvis_audio_cap_t *cap);

/* Playback stream: signed 16-bit LE PCM */
jarvis_audio_play_t *jarvis_audio_play_open(int sample_rate, int channels, const char *app_name);
int                  jarvis_audio_play_write(jarvis_audio_play_t *play, const int16_t *samples, size_t n_samples);
void                 jarvis_audio_play_finish(jarvis_audio_play_t *play);
int                  jarvis_audio_play_poll_done(jarvis_audio_play_t *play);
void                 jarvis_audio_play_abort(jarvis_audio_play_t *play);
void                 jarvis_audio_play_close(jarvis_audio_play_t *play);
int                  jarvis_audio_play_rate(const jarvis_audio_play_t *play);

/* Desktop audio utilities */
void        jarvis_audio_play_chime_async(const char *wav_path);
void        jarvis_audio_unmute_mic(void);
void        jarvis_audio_duck(void);
void        jarvis_audio_restore(void);
int         jarvis_audio_is_external_playing(void);
bool        jarvis_audio_has_capture_backend(void);
const char *jarvis_audio_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_AUDIO_H */
