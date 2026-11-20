#ifndef JARVIS_VAD_H
#define JARVIS_VAD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vad_biquad vad_biquad_t;

typedef struct {
    double noise_floor_db;
    double ambient_std_db;
    int env_frames;
    int in_speech_hint;
} vad_state_t;

typedef struct vad_ctx vad_ctx_t;

vad_ctx_t *vad_create(double initial_noise_floor_db, double threshold_db);
void vad_destroy(vad_ctx_t *ctx);

/* Compute dB for a frame */
float vad_calc_frame_db(const int16_t *samples, size_t n, vad_biquad_t *flt);

/* Update ambient noise floor from recent non-speech frames */
void vad_update_ambient(vad_ctx_t *ctx, const float *db_ring, int64_t total_frames,
                        int ring_frames, const uint8_t *is_speech);

/* VAD thresholds */
double vad_calc_start_db(double amb_avg, double amb_std);
double vad_calc_keep_db(double utter_amb_avg, double utter_amb_std,
                        double utter_start_db, double peak_speech_db);

/* Push frame into VAD ring (updates noise floor) */
bool vad_push_frame(vad_ctx_t *ctx, const float *frame_samples, size_t count, float db);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_VAD_H */