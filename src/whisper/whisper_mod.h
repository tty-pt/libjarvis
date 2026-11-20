#ifndef JARVIS_WHISPER_MOD_H
#define JARVIS_WHISPER_MOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

struct whisper_context;

typedef struct {
    struct whisper_context *ctx;
    int n_threads;
    float *pcm;
    int n_samples;
    char lang[32];
    const char *prompt;
    int max_tok;
    int is_partial;
    int64_t job_voiced_f;
    int has_job;
    int busy;
    volatile sig_atomic_t abort_flag;
    int has_result;
    char result_text[4096];
    int64_t result_voiced_f;
    int result_is_partial;
    int result_all_silent;
    int64_t done_at_frame;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
} whisper_decoder_t;

/* Initialize decoder with main context */
int whisper_decoder_init(whisper_decoder_t *dec, struct whisper_context *ctx_main,
                         int n_threads, int max_utter_frames);

/* Cleanup decoder */
void whisper_decoder_fini(whisper_decoder_t *dec);

/* Background worker thread */
void *whisper_decode_worker_fn(void *arg);

/* Submit a job for decoding (non-blocking) */
int whisper_decoder_submit(whisper_decoder_t *dec,
                           int64_t start_f, int64_t end_f, int64_t voiced_f,
                           const char *lang, const char *prompt,
                           int max_tok, int is_partial,
                           int (*ring_extract)(int64_t, int64_t, float*, int));

/* Force submit final decode (waits for completion) */
void whisper_decoder_force_final(whisper_decoder_t *dec,
                                 int64_t start_f, int64_t end_f, int64_t voiced_f,
                                 const char *lang, const char *prompt, int max_tok,
                                 int (*ring_extract)(int64_t, int64_t, float*, int));

/* Reset session */
void whisper_decoder_reset(whisper_decoder_t *dec);

/* Run a single Whisper slice (blocking, for offline/wake) */
int whisper_run_slice(struct whisper_context *ctx, int n_threads,
                      const float *pcm, int n_samples,
                      const char *lang, const char *prompt,
                      int max_tok, int is_wake, int is_partial,
                      int use_abort_cb,
                      char *out_text, size_t out_sz, int *out_all_silent);

/* Normalize PCM with AGC */
float *whisper_normalize_pcm_agc(const float *pcm, int n_samples);

/* Collapse repetitions in text */
void whisper_collapse_repetitions(char *s, size_t s_sz);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_WHISPER_MOD_H */