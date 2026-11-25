#ifndef JARVIS_STT_BACKEND_H
#define JARVIS_STT_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	JARVIS_HW_CPU      = 0,
	JARVIS_HW_IGPU     = 1,
	JARVIS_HW_FAST_GPU = 2
} jarvis_hw_tier_t;

typedef struct {
	jarvis_hw_tier_t tier;
	char             backend[32];  /* "CUDA" | "Metal" | "Vulkan" | "CPU" */
	char             device[128];  /* GGML device description/name */
	int              has_gpu;      /* 1 if discrete or integrated GPU active */
	int              is_igpu;      /* 1 if GGML_BACKEND_DEVICE_TYPE_IGPU */
	int              ac512_ms;     /* measured warmup decode ms @ audio_ctx=512 (or Parakeet 1s) */
	int              ac1500_ms;    /* measured warmup decode ms @ audio_ctx=0 (0 if skipped) */
	int              final_audio_ctx; /* calibrated default: 0 (full), 512, or 768 */
	int              beam_size;       /* calibrated default: 1..2 */
} jarvis_hw_profile_t;

typedef bool (*jarvis_stt_abort_fn)(void *user_data);

#define JARVIS_STT_MAX_WORDS 256

typedef struct {
	char text[64];  /* trimmed whole word (UTF-8) */
	int  start_ms;  /* offset from start of PCM slice, ms */
	int  end_ms;    /* end offset from start of PCM slice, ms */
} jarvis_stt_word_t;

typedef struct {
	const float        *pcm;        /* 16 kHz mono float32 */
	int                 n_samples;
	int                 n_threads;
	const char         *lang;       /* ISO code (e.g. "pt", "en") or "auto" */
	const char         *prompt;     /* optional initial prompt (Whisper only) */
	int                 max_tok;
	int                 audio_ctx;  /* 0 = full native window, >0 = reduced ctx (Whisper) */
	int                 beam_size;  /* 1 = greedy, >1 = beam search (Whisper) */
	int                 is_wake;
	int                 is_partial;
	int                 apply_agc;  /* 1 = normalize PCM via jarvis_stt_normalize_pcm */
	int                 use_context;/* 1 = allow prompt context carry-over (streaming windows) */
	jarvis_stt_word_t  *out_words;  /* optional caller buffer for word-level timestamps */
	int                 max_words;  /* capacity of out_words (0 = timestamps disabled) */
	jarvis_stt_abort_fn abort_cb;
	void               *abort_ud;
} jarvis_stt_req_t;

typedef struct {
	char text[4096];
	char detected_lang[32];
	int  all_silent;
	int  n_words;
} jarvis_stt_res_t;

typedef struct jarvis_stt_backend_ops {
	const char *name; /* "whisper" | "parakeet" */
	bool (*supports_lang)(const char *lang);
	bool (*is_multilingual)(void *impl);
	int  (*transcribe)(void *impl, const jarvis_stt_req_t *req, jarvis_stt_res_t *res);
	void (*destroy)(void *impl);
} jarvis_stt_backend_ops_t;

typedef struct {
	const jarvis_stt_backend_ops_t *ops;
	void                           *impl;
	char                            model_path[512];
} jarvis_stt_ctx_t;

/* Backend lifecycle & format/language helpers */
bool              jarvis_stt_whisper_supports_lang(const char *lang);
bool              jarvis_stt_parakeet_supports_lang(const char *lang);
bool              jarvis_stt_model_looks_parakeet(const char *model_path);

jarvis_stt_ctx_t *jarvis_stt_open_whisper(const char *model_path, bool use_gpu);
jarvis_stt_ctx_t *jarvis_stt_open_parakeet(const char *model_path, bool use_gpu);
jarvis_stt_ctx_t *jarvis_stt_open_auto(const char *model_path, bool use_gpu);

static inline int
jarvis_stt_transcribe(jarvis_stt_ctx_t *ctx, const jarvis_stt_req_t *req, jarvis_stt_res_t *res)
{
	if (!ctx || !ctx->ops || !ctx->ops->transcribe || !req || !res)
		return -1;
	return ctx->ops->transcribe(ctx->impl, req, res);
}

static inline bool
jarvis_stt_is_multilingual(const jarvis_stt_ctx_t *ctx)
{
	if (!ctx || !ctx->ops || !ctx->ops->is_multilingual)
		return false;
	return ctx->ops->is_multilingual(ctx->impl);
}

static inline bool
jarvis_stt_supports_lang(const jarvis_stt_ctx_t *ctx, const char *lang)
{
	if (!ctx || !ctx->ops || !ctx->ops->supports_lang)
		return false;
	return ctx->ops->supports_lang(lang);
}

static inline const char *
jarvis_stt_backend_name(const jarvis_stt_ctx_t *ctx)
{
	return (ctx && ctx->ops && ctx->ops->name) ? ctx->ops->name : "none";
}

void              jarvis_stt_close(jarvis_stt_ctx_t *ctx);

/* Hardware discovery & startup latency calibration */
const char *jarvis_hw_tier_str(jarvis_hw_tier_t tier);
void        jarvis_hw_probe_devices(jarvis_hw_profile_t *out);
void        jarvis_hw_calibrate_profile(jarvis_hw_profile_t *prof, int ac512_ms, int ac1500_ms);
void        jarvis_hw_warmup_and_calibrate(jarvis_hw_profile_t *prof,
                                           jarvis_stt_ctx_t    *main_ctx,
                                           jarvis_stt_ctx_t    *wake_ctx,
                                           int                  n_threads,
                                           const char          *lang);

#endif /* JARVIS_STT_BACKEND_H */
