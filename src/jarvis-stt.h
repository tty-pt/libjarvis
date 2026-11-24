#ifndef JARVIS_STT_H
#define JARVIS_STT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include "whisper.h"
#include <ttypt/stele.h>

#define JARVIS_SAMPLE_RATE      16000
#define JARVIS_FRAME_MS         20
#define JARVIS_FRAME_SAMPLES    ((JARVIS_SAMPLE_RATE * JARVIS_FRAME_MS) / 1000) /* 320 samples */
#define JARVIS_FRAME_BYTES      (JARVIS_FRAME_SAMPLES * (int)sizeof(int16_t))
#define JARVIS_RING_FRAMES      6000 /* 120 seconds circular float32 PCM buffer */
#define JARVIS_RING_SAMPLES     (JARVIS_RING_FRAMES * JARVIS_FRAME_SAMPLES)
#define JARVIS_MAX_UTTER_FRAMES 4500 /* 90 seconds max utterance */
#define JARVIS_WARM_HOLD_MS     60000L

/* Audio capture ring buffer & adaptive noise floor VAD state */
typedef struct {
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
	float           pcm[JARVIS_RING_SAMPLES];
	float           db[JARVIS_RING_FRAMES];
	uint8_t         is_speech[JARVIS_RING_FRAMES];
	int64_t         total_frames;
	int64_t         last_cmd_end_f;
	double          noise_floor_db;
	double          ambient_std_db;
	int             env_frames;
	int             active_users;
	long            hold_until_ms;
	int             in_speech_hint;
	int             pw_open;
} jarvis_audio_ring_t;

/* Background Whisper decode worker state */
typedef struct {
	pthread_mutex_t         mtx;
	pthread_cond_t          cond;
	struct whisper_context *ctx;
	int                     n_threads;
	float                  *pcm;
	int                     n_samples;
	char                    lang[32];
	const char             *prompt;
	int                     max_tok;
	int                     is_partial;
	int64_t                 job_voiced_f;
	int64_t                 job_end_f;
	int                     has_job;
	int                     busy;
	volatile sig_atomic_t   abort_flag;
	int                     has_result;
	char                    result_text[4096];
	int64_t                 result_voiced_f;
	int64_t                 result_end_f;
	int                     result_is_partial;
	int                     result_all_silent;
	int64_t                 done_at_frame;
} jarvis_decode_worker_t;

/* Global STT state references shared across audio threads */
extern jarvis_audio_ring_t    g_audio;
extern jarvis_decode_worker_t g_dec;

/* DSP and Whisper helper routines */
float *jarvis_stt_apply_preemphasis_and_agc(const float *pcm, int n_samples);
int    jarvis_stt_compute_audio_ctx(int n_samples, int is_wake, int is_partial);
void   jarvis_stt_run_whisper_slice(struct whisper_context *ctx, int n_threads, const float *pcm, int n_samples,
                                   const char *lang, const char *prompt, int max_tok, int is_wake, int is_partial,
                                   int use_abort_cb, char *out_text, size_t out_sz, int *out_all_silent);

int    jarvis_stt_ring_extract_pcm(int64_t start_f, int64_t end_f, float *out_pcm, int max_samples);
int    jarvis_stt_load_wav_f32(const char *path, float **out_pcm, int *out_samples);

#endif /* JARVIS_STT_H */
