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

typedef struct jarvis_stt jarvis_stt_t;

/* Callback to check if running state should abort */
typedef bool (*jarvis_abort_cb_t)(void *user_data);

/* Audio capture ring buffer & VAD state */
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

#endif /* JARVIS_STT_H */
