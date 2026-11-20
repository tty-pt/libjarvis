#ifndef JARVIS_AUDIO_H
#define JARVIS_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_FRAME_MS        20
#define AUDIO_FRAME_SAMPLES   ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define AUDIO_RING_FRAMES     2000
#define AUDIO_RING_SAMPLES    (AUDIO_RING_FRAMES * AUDIO_FRAME_SAMPLES)
#define AUDIO_MAX_UTTER_FRAMES 1500
#define AUDIO_WARM_HOLD_MS    60000L

typedef struct audio_ctx audio_ctx_t;

typedef struct {
    float *pcm;
    float *db;
    uint8_t *is_speech;
    int64_t total_frames;
    int64_t last_cmd_end_f;
    double noise_floor_db;
    double ambient_std_db;
    int env_frames;
    int active_users;
    long hold_until_ms;
    int in_speech_hint;
    int pw_open;
} audio_state_t;

audio_ctx_t *audio_create(void);
void audio_destroy(audio_ctx_t *ctx);

/* Start/stop PipeWire capture */
int audio_acquire(audio_ctx_t *ctx);
void audio_release(audio_ctx_t *ctx, long hold_ms);

/* Ring buffer access */
int audio_get_state(const audio_ctx_t *ctx, audio_state_t *out_state);
int audio_extract_pcm(audio_ctx_t *ctx, int64_t start_f, int64_t end_f,
                      float *out_pcm, int max_samples);

/* Background capture thread */
void *audio_thread_fn(void *arg);

/* Utility */
long audio_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_AUDIO_H */