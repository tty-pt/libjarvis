#ifndef JARVIS_TTS_H
#define JARVIS_TTS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tts_ctx tts_ctx_t;

tts_ctx_t *tts_create(void);
void tts_destroy(tts_ctx_t *ctx);

/* Ensure TTS engine is loaded with given voice */
bool tts_ensure_loaded(tts_ctx_t *ctx, const char *voice);

/* Synthesize a text segment to PCM */
int tts_synthesize(tts_ctx_t *ctx, const char *text, float **out_pcm, int *out_samples);

/* Write earcon WAV file */
bool tts_write_earcon(const char *path, bool is_done);

/* Clean text for TTS */
void tts_clean_text(const char *raw, char *out, size_t out_sz);

/* Split text into TTS segments */
size_t tts_split_segments(const char *text, char out_segments[][256], size_t max_segments);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_TTS_H */