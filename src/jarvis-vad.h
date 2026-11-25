#ifndef JARVIS_VAD_H
#define JARVIS_VAD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JARVIS_VAD_SAMPLE_RATE   16000
#define JARVIS_VAD_CHUNK_SAMPLES 512 /* 32 ms native Silero v5/v6 window @ 16 kHz */

struct whisper_vad_context;

/* Streaming neural Voice Activity Detector (Silero VAD via whisper.cpp).
 * Accumulates arbitrary frame sizes (e.g. 320 samples / 20 ms) into native
 * 512-sample chunks and runs stateful LSTM inference (< 0.2 ms on CPU).
 * Gracefully degrades to available == 0 (returning -1.0f) when no VAD model
 * is installed or when explicitly disabled via `vad_model = off`.
 */
typedef struct jarvis_nvad {
	struct whisper_vad_context *vctx;
	float buf[JARVIS_VAD_CHUNK_SAMPLES];
	int   buf_fill;
	float last_prob;
	int   n_threads;
	int   available;
	char  model_path[512];
} jarvis_nvad_t;

/* Resolve Silero VAD GGML model path from ~/.jarvisrc (`vad_model`),
 * $JARVIS_VAD_MODEL, user/system share dirs, or repo submodule fallback.
 * Returns 0 if a readable model path was found, -1 if unavailable or disabled.
 */
int   jarvis_nvad_resolve_model(char *out_path, size_t out_sz);

/* Initialize streaming Silero VAD. If model_path is NULL/empty or unreadable,
 * initializes cleanly in fallback mode (available == 0) and returns 0.
 * Returns -1 only on invalid arguments.
 */
int   jarvis_nvad_init(jarvis_nvad_t *vad, const char *model_path, int n_threads);

/* Reset LSTM hidden/cell state and chunk accumulator (e.g. between streams). */
void  jarvis_nvad_reset(jarvis_nvad_t *vad);

/* Feed `n_samples` mono 16 kHz float32 PCM samples (e.g. 320 samples = 20 ms).
 * Returns speech probability in [0.0f, 1.0f] when neural VAD is active,
 * or -1.0f when running in fallback mode (available == 0).
 */
float jarvis_nvad_process(jarvis_nvad_t *vad, const float *pcm, size_t n_samples);

int   jarvis_nvad_is_available(const jarvis_nvad_t *vad);
void  jarvis_nvad_free(jarvis_nvad_t *vad);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_VAD_H */
