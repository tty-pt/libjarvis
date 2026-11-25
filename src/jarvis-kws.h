#ifndef JARVIS_KWS_H
#define JARVIS_KWS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	JARVIS_KWS_TIER_VAD_GATE = 0, /* Tier 1: Silero VAD gated ctx_wake fallback (universal) */
	JARVIS_KWS_TIER_ONNX     = 1  /* Tier 2: Sub-millisecond ONNX neural KWS model */
} jarvis_kws_tier_t;

typedef struct {
	char  input_name[64];   /* default: "input" */
	char  output_name[64];  /* default: "output" */
	int   window_samples;   /* default: 16000 (1.0 s @ 16 kHz) */
	int   hop_samples;      /* default: 1280  (80 ms @ 16 kHz) */
	float threshold;        /* default: 0.50f */
	int   min_hits;         /* default: 2 consecutive hops >= threshold */
	int   cooldown_frames;  /* default: 40 (800 ms @ 20 ms/frame) */
	float vad_gate_prob;    /* default: 0.55f Silero VAD speech gate */
} jarvis_kws_cfg_t;

typedef struct jarvis_kws jarvis_kws_t;

void jarvis_kws_default_cfg(jarvis_kws_cfg_t *cfg);

/**
 * @brief Resolve an optional ONNX KWS model path for @p wake_word.
 *
 * Search order:
 *   1. @p explicit_path (if non-empty and readable)
 *   2. `$JARVIS_KWS_MODEL` (if readable)
 *   3. `kws_model` in ~/.jarvisrc (if readable)
 *   4. `~/.local/share/jarvis/models/kws/<wake_word>.onnx`
 *   5. `jarvis_plat_data_file("jarvis/models/kws", "<wake_word>.onnx")`
 *
 * Returns true if a readable `.onnx` path was written to @p out_path.
 */
bool jarvis_kws_find_model(const char *wake_word, const char *explicit_path,
                           char *out_path, size_t out_sz);

/**
 * @brief Create a tiered KWS engine for @p wake_word.
 *
 * If an ONNX KWS model is available (via @p model_override or auto-discovery)
 * and `libonnxruntime` loads cleanly, opens a Tier 2 ONNX session.
 * Otherwise returns a lightweight Tier 1 handle (Silero VAD gate + universal
 * STT fallback) — never returns NULL on missing optional ONNX assets.
 */
jarvis_kws_t *jarvis_kws_open(const char *wake_word,
                              const char *model_override,
                              const jarvis_kws_cfg_t *cfg);

void              jarvis_kws_close(jarvis_kws_t *kws);
void              jarvis_kws_reset(jarvis_kws_t *kws);
jarvis_kws_tier_t jarvis_kws_tier(const jarvis_kws_t *kws);
const char       *jarvis_kws_tier_str(const jarvis_kws_t *kws);
const char       *jarvis_kws_model_path(const jarvis_kws_t *kws);
uint64_t          jarvis_kws_onnx_runs(const jarvis_kws_t *kws);

/**
 * @brief Tier 1 Silero VAD speech-segment gate for `ctx_wake` execution.
 *
 * When neural Silero VAD is active (@p vad_neural != 0), returns true only if
 * the candidate wake slice contains a genuine speech segment:
 *   - peak Silero probability >= @p min_peak_prob (default 0.55f if <= 0), AND
 *   - at least @p min_speech_frames frames with vad_prob >= 0.45f (default 3 if <= 0).
 *
 * When @p vad_neural == 0 (no Silero model loaded), returns true (no regression).
 */
bool jarvis_kws_vad_gate_slice(const float *vad_probs,
                               int n_frames,
                               int vad_neural,
                               float min_peak_prob,
                               int min_speech_frames);

/**
 * @brief Feed a 20 ms float32 PCM frame (@p n_samples <= 320, normalized [-1, 1])
 *        into Tier 2 ONNX KWS.
 *
 * Returns:
 *   1 if wake keyword triggered (score >= threshold for min_hits outside cooldown),
 *   0 if not triggered (or gated off by VAD / cooldown / Tier 1 mode).
 * Writes the latest probability score to @p out_score when non-NULL.
 */
int jarvis_kws_process_frame(jarvis_kws_t *kws,
                             const float *pcm_f32,
                             int n_samples,
                             float vad_prob,
                             int vad_neural,
                             float *out_score);

/* ---------------------------------------------------------------------------
 * Adaptive Syntactic & Silero-Aware Endpointing State Machine
 * ------------------------------------------------------------------------- */

typedef struct {
	int   eff_silence_ms;    /* configured cmd_silence_ms or steer_silence_ms */
	int   frame_ms;          /* frame duration in ms (20) */
	int   operating_mode;    /* 1 during active agent steering */
	int   vad_neural;        /* 1 when Silero VAD is active */
	float last_vad_prob;     /* latest Silero speech probability [0..1], or < 0 if unavailable */
	int   ends_continuation; /* 1 if current live hypothesis ends with a locale continuation token */
	int   ptt_held;          /* 1 while JARVIS_PTT_START is active */
} jarvis_endpoint_in_t;

typedef struct {
	int spec_silence_f;      /* silence frames before triggering speculative final decode */
	int spec_early_f;        /* silence frames before accepting completed speculative final */
	int final_silence_ms;    /* hard silence endpoint threshold in ms */
	int suppress_endpoint;   /* 1 when endpointing must wait (PTT held or active Silero speech) */
	int grace_ms_applied;    /* continuation grace added in ms (0 or 450) */
} jarvis_endpoint_plan_t;

/**
 * @brief Compute adaptive endpointing thresholds from Silero VAD, syntactic
 *        continuation state, and PTT hold state.
 */
void jarvis_endpoint_compute(const jarvis_endpoint_in_t *in,
                             jarvis_endpoint_plan_t *out);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_KWS_H */
