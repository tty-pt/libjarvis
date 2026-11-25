/*
 * jarvis-kws.c — Tiered Wake-Word Engine & Adaptive Syntactic Endpointing
 *
 * Tier 1 (Zero new models):
 *   Strictly gates `ctx_wake` STT calls on Silero VAD speech segments
 *   (`peak vad_prob >= 0.55` and `>= 3` frames `>= 0.45`) so idle ambient room
 *   noise / typing invokes 0 GPU/CPU wake decodes.
 *
 * Tier 2 (Sub-millisecond ONNX KWS):
 *   Reuses system `libonnxruntime` via lazy `stl_dlopen()` (same verified ORT
 *   1.17 C API vtable indices as jarvis-tts.c; zero link-time dependency) when
 *   a matching `<wake_word>.onnx` model is installed. Supports optional
 *   `<model>.cfg` sidecar for custom input/output tensor names, window size,
 *   hop size, and detection threshold.
 *
 * Universal Fallback:
 *   If no ONNX KWS model exists for the active custom wake word or ORT is not
 *   installed, cleanly operates in Tier 1 (Silero-gated Whisper/Parakeet).
 */

#include "jarvis-kws.h"
#include "jarvis-platform.h"
#include "../include/ttypt/jarvis.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Minimal ORT 1.17 C API vtable subset (identical verified indices to jarvis-tts.c)
 * ------------------------------------------------------------------------- */

typedef struct {
	const void *(*GetApi)(uint32_t version);
	const char *(*GetVersionString)(void);
} kws_ort_api_base_t;

typedef const kws_ort_api_base_t *(*kws_ort_get_api_base_fn)(void);
typedef void *(*kws_ort_create_env_fn)(int log_level, const char *logid, void **out);
typedef void *(*kws_ort_create_sess_opts_fn)(void **out);
typedef void *(*kws_ort_disable_arena_fn)(void *options);
typedef void *(*kws_ort_set_opt_level_fn)(void *options, int level);
typedef void *(*kws_ort_set_threads_fn)(void *options, int num_threads);
typedef void *(*kws_ort_create_session_fn)(const void *env, const char *model_path,
                                           const void *options, void **out);
typedef void *(*kws_ort_run_fn)(void *sess, const void *run_options,
                                const char *const *input_names, const void *const *inputs, size_t input_len,
                                const char *const *output_names, size_t output_names_len, void **outputs);
typedef void *(*kws_ort_create_cpu_mi_fn)(int alloc_type, int mem_type, void **out);
typedef void *(*kws_ort_create_tensor_fn)(const void *info, void *p_data, size_t p_data_len,
                                          const int64_t *shape, size_t shape_len, int type, void **out);
typedef void *(*kws_ort_get_tensor_data_fn)(void *value, void **out);
typedef void *(*kws_ort_get_tensor_type_shape_fn)(const void *value, void **out);
typedef void *(*kws_ort_get_tensor_elem_count_fn)(const void *info, size_t *out);
typedef void  (*kws_ort_release_fn)(void *ptr);

#define KWS_ORT_API_VERSION                    17
#define KWS_ORT_IDX_CreateEnv                   3
#define KWS_ORT_IDX_CreateSession               7
#define KWS_ORT_IDX_Run                         9
#define KWS_ORT_IDX_CreateSessionOptions       10
#define KWS_ORT_IDX_DisableCpuMemArena         19
#define KWS_ORT_IDX_SetGraphOptimizationLevel  23
#define KWS_ORT_IDX_SetIntraOpNumThreads       24
#define KWS_ORT_IDX_SetInterOpNumThreads       25
#define KWS_ORT_IDX_CreateTensorWithData       49
#define KWS_ORT_IDX_GetTensorMutableData       51
#define KWS_ORT_IDX_GetTensorShapeElementCount 64
#define KWS_ORT_IDX_GetTensorTypeAndShape      65
#define KWS_ORT_IDX_CreateCpuMemoryInfo        69
#define KWS_ORT_IDX_ReleaseEnv                 92
#define KWS_ORT_IDX_ReleaseStatus              93
#define KWS_ORT_IDX_ReleaseMemoryInfo          94
#define KWS_ORT_IDX_ReleaseSession             95
#define KWS_ORT_IDX_ReleaseValue               96
#define KWS_ORT_IDX_ReleaseTensorTypeAndShape  99
#define KWS_ORT_IDX_ReleaseSessionOptions     100
#define KWS_ONNX_TENSOR_FLOAT                   1

typedef void (*kws_ort_generic_fn)(void);

struct jarvis_kws {
	jarvis_kws_tier_t tier;
	jarvis_kws_cfg_t  cfg;
	char              wake_word[64];
	char              model_path[1024];

	/* Lazy-loaded ORT handles (non-NULL only in JARVIS_KWS_TIER_ONNX) */
	void             *ort_dl;
	const kws_ort_generic_fn *ort_vtbl;
	void             *ort_env;
	void             *ort_mem_info;
	void             *ort_sess;

	/* Sliding PCM window [window_samples] + contiguous inference scratch */
	float            *pcm_ring;
	float            *pcm_linear;
	int               ring_pos;
	int               ring_filled;
	int               hop_accum;

	/* Detection hysteresis & telemetry */
	int               consec_hits;
	int               cooldown_left;
	float             last_score;
	uint64_t          onnx_runs;
};

static void kws_ort_release_status(const kws_ort_generic_fn *vtbl, void *st)
{
	if (!st || !vtbl || !vtbl[KWS_ORT_IDX_ReleaseStatus])
		return;
	((kws_ort_release_fn)vtbl[KWS_ORT_IDX_ReleaseStatus])(st);
}

void jarvis_kws_default_cfg(jarvis_kws_cfg_t *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	snprintf(cfg->input_name, sizeof(cfg->input_name), "input");
	snprintf(cfg->output_name, sizeof(cfg->output_name), "output");
	cfg->window_samples  = 16000;
	cfg->hop_samples     = 1280;
	cfg->threshold       = 0.50f;
	cfg->min_hits        = 2;
	cfg->cooldown_frames = 40;
	cfg->vad_gate_prob   = 0.55f;
}

static void sanitize_wake_slug(const char *wake_word, char *out, size_t out_sz)
{
	if (!out || out_sz == 0)
		return;
	out[0] = '\0';
	if (!wake_word || !*wake_word)
		return;
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)wake_word; *p && w + 1 < out_sz; p++) {
		if (isalnum(*p)) {
			out[w++] = (char)tolower(*p);
		} else if ((*p == ' ' || *p == '-' || *p == '_') && w > 0 && out[w - 1] != '_') {
			out[w++] = '_';
		}
	}
	while (w > 0 && out[w - 1] == '_')
		w--;
	out[w] = '\0';
}

bool jarvis_kws_find_model(const char *wake_word, const char *explicit_path,
                           char *out_path, size_t out_sz)
{
	if (!out_path || out_sz == 0)
		return false;
	out_path[0] = '\0';

	if (explicit_path && *explicit_path && jarvis_plat_is_readable(explicit_path)) {
		snprintf(out_path, out_sz, "%s", explicit_path);
		return true;
	}

	const char *env_m = getenv("JARVIS_KWS_MODEL");
	if (env_m && *env_m && jarvis_plat_is_readable(env_m)) {
		snprintf(out_path, out_sz, "%s", env_m);
		return true;
	}

	char rc_m[1024] = {0};
	if (jarvis_rc_get("kws_model", "", rc_m, sizeof(rc_m)) && rc_m[0] &&
	    jarvis_plat_is_readable(rc_m)) {
		snprintf(out_path, out_sz, "%s", rc_m);
		return true;
	}

	char slug[64];
	sanitize_wake_slug(wake_word, slug, sizeof(slug));
	if (!slug[0])
		return false;

	char fname[96];
	snprintf(fname, sizeof(fname), "%s.onnx", slug);

	const char *home = getenv("HOME");
	if (home && *home) {
		char cand[1024];
		snprintf(cand, sizeof(cand), "%s/.local/share/jarvis/models/kws/%s", home, fname);
		if (jarvis_plat_is_readable(cand)) {
			snprintf(out_path, out_sz, "%s", cand);
			return true;
		}
		snprintf(cand, sizeof(cand), "%s/.local/share/jarvis/models/kws/hey_%s", home, fname);
		if (jarvis_plat_is_readable(cand)) {
			snprintf(out_path, out_sz, "%s", cand);
			return true;
		}
	}

	char dp[1024];
	if (jarvis_plat_data_file("jarvis/models/kws", fname, dp, sizeof(dp)) == 0 &&
	    jarvis_plat_is_readable(dp)) {
		snprintf(out_path, out_sz, "%s", dp);
		return true;
	}

	return false;
}

static void kws_load_sidecar_cfg(const char *model_path, jarvis_kws_cfg_t *cfg)
{
	if (!model_path || !*model_path || !cfg)
		return;
	char cfg_path[1040];
	snprintf(cfg_path, sizeof(cfg_path), "%s.cfg", model_path);
	FILE *fp = fopen(cfg_path, "r");
	if (!fp)
		return;

	char line[256];
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		while (*p && isspace((unsigned char)*p))
			p++;
		if (*p == '#' || *p == '\0')
			continue;
		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *k = p;
		char *v = eq + 1;
		char *ke = eq;
		while (ke > k && isspace((unsigned char)ke[-1]))
			*--ke = '\0';
		while (*v && isspace((unsigned char)*v))
			v++;
		char *ve = v + strlen(v);
		while (ve > v && isspace((unsigned char)ve[-1]))
			*--ve = '\0';

		if (strcmp(k, "input_name") == 0 && *v)
			snprintf(cfg->input_name, sizeof(cfg->input_name), "%s", v);
		else if (strcmp(k, "output_name") == 0 && *v)
			snprintf(cfg->output_name, sizeof(cfg->output_name), "%s", v);
		else if (strcmp(k, "window_samples") == 0 && atoi(v) >= 320)
			cfg->window_samples = atoi(v);
		else if (strcmp(k, "hop_samples") == 0 && atoi(v) >= 160)
			cfg->hop_samples = atoi(v);
		else if (strcmp(k, "threshold") == 0 && strtof(v, NULL) > 0.0f)
			cfg->threshold = strtof(v, NULL);
		else if (strcmp(k, "min_hits") == 0 && atoi(v) >= 1)
			cfg->min_hits = atoi(v);
		else if (strcmp(k, "cooldown_frames") == 0 && atoi(v) >= 0)
			cfg->cooldown_frames = atoi(v);
	}
	fclose(fp);
}

static void kws_unload_onnx(jarvis_kws_t *kws)
{
	if (!kws)
		return;
	if (kws->ort_vtbl) {
		kws_ort_release_fn rel_sess = (kws_ort_release_fn)kws->ort_vtbl[KWS_ORT_IDX_ReleaseSession];
		kws_ort_release_fn rel_mi   = (kws_ort_release_fn)kws->ort_vtbl[KWS_ORT_IDX_ReleaseMemoryInfo];
		kws_ort_release_fn rel_env  = (kws_ort_release_fn)kws->ort_vtbl[KWS_ORT_IDX_ReleaseEnv];
		if (kws->ort_sess && rel_sess) rel_sess(kws->ort_sess);
		if (kws->ort_mem_info && rel_mi) rel_mi(kws->ort_mem_info);
		if (kws->ort_env && rel_env) rel_env(kws->ort_env);
	}
	kws->ort_sess     = NULL;
	kws->ort_mem_info = NULL;
	kws->ort_env      = NULL;
	kws->ort_vtbl     = NULL;
	if (kws->ort_dl) {
		stl_dlclose(kws->ort_dl);
		kws->ort_dl = NULL;
	}
}

static bool kws_init_onnx(jarvis_kws_t *kws, const char *model_path)
{
	int saved_stderr = -1;
	jarvis_plat_suppress_stderr_begin(&saved_stderr);
	kws->ort_dl = stl_dlopen(jarvis_plat_ort_lib_name(0), 0);
	if (!kws->ort_dl)
		kws->ort_dl = stl_dlopen(jarvis_plat_ort_lib_name(1), 0);
	jarvis_plat_suppress_stderr_end(saved_stderr);
	if (!kws->ort_dl)
		return false;

	kws_ort_get_api_base_fn get_base = NULL;
	void *sym = stl_dlsym(kws->ort_dl, "OrtGetApiBase");
	memcpy(&get_base, &sym, sizeof(get_base));
	if (!get_base || !get_base()) {
		kws_unload_onnx(kws);
		return false;
	}
	const kws_ort_api_base_t *base = get_base();
	kws->ort_vtbl = (const kws_ort_generic_fn *)base->GetApi(KWS_ORT_API_VERSION);
	if (!kws->ort_vtbl ||
	    !kws->ort_vtbl[KWS_ORT_IDX_CreateEnv] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_CreateSession] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_Run] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_CreateSessionOptions] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_DisableCpuMemArena] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_SetGraphOptimizationLevel] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_SetIntraOpNumThreads] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_SetInterOpNumThreads] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_CreateTensorWithData] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_GetTensorMutableData] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_GetTensorShapeElementCount] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_GetTensorTypeAndShape] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_CreateCpuMemoryInfo] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_ReleaseEnv] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_ReleaseStatus] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_ReleaseMemoryInfo] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_ReleaseSession] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_ReleaseValue] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_ReleaseTensorTypeAndShape] ||
	    !kws->ort_vtbl[KWS_ORT_IDX_ReleaseSessionOptions]) {
		kws_unload_onnx(kws);
		return false;
	}

	kws_ort_create_env_fn create_env =
		(kws_ort_create_env_fn)kws->ort_vtbl[KWS_ORT_IDX_CreateEnv];
	kws_ort_create_cpu_mi_fn create_mi =
		(kws_ort_create_cpu_mi_fn)kws->ort_vtbl[KWS_ORT_IDX_CreateCpuMemoryInfo];

	int saved2 = -1;
	jarvis_plat_suppress_stderr_begin(&saved2);
	void *st = create_env(3 /* ORT_LOGGING_LEVEL_ERROR */, "jarvis-kws", &kws->ort_env);
	jarvis_plat_suppress_stderr_end(saved2);
	if (st) {
		kws_ort_release_status(kws->ort_vtbl, st);
		kws_unload_onnx(kws);
		return false;
	}

	st = create_mi(1 /* OrtArenaAllocator */, 0 /* OrtMemTypeDefault */, &kws->ort_mem_info);
	if (st) {
		kws_ort_release_status(kws->ort_vtbl, st);
		kws_unload_onnx(kws);
		return false;
	}

	void *so = NULL;
	kws_ort_create_sess_opts_fn create_so =
		(kws_ort_create_sess_opts_fn)kws->ort_vtbl[KWS_ORT_IDX_CreateSessionOptions];
	st = create_so(&so);
	if (st || !so) {
		kws_ort_release_status(kws->ort_vtbl, st);
		kws_unload_onnx(kws);
		return false;
	}

	((kws_ort_set_opt_level_fn)kws->ort_vtbl[KWS_ORT_IDX_SetGraphOptimizationLevel])(so, 99);
	((kws_ort_set_threads_fn)kws->ort_vtbl[KWS_ORT_IDX_SetIntraOpNumThreads])(so, 1);
	((kws_ort_set_threads_fn)kws->ort_vtbl[KWS_ORT_IDX_SetInterOpNumThreads])(so, 1);
	((kws_ort_disable_arena_fn)kws->ort_vtbl[KWS_ORT_IDX_DisableCpuMemArena])(so);

	kws_ort_create_session_fn create_sess =
		(kws_ort_create_session_fn)kws->ort_vtbl[KWS_ORT_IDX_CreateSession];
	st = create_sess(kws->ort_env, model_path, so, &kws->ort_sess);
	((kws_ort_release_fn)kws->ort_vtbl[KWS_ORT_IDX_ReleaseSessionOptions])(so);
	if (st || !kws->ort_sess) {
		kws_ort_release_status(kws->ort_vtbl, st);
		kws_unload_onnx(kws);
		return false;
	}

	return true;
}

jarvis_kws_t *jarvis_kws_open(const char *wake_word,
                              const char *model_override,
                              const jarvis_kws_cfg_t *cfg)
{
	jarvis_kws_t *kws = (jarvis_kws_t *)calloc(1, sizeof(*kws));
	if (!kws)
		return NULL;

	jarvis_kws_default_cfg(&kws->cfg);
	if (cfg) {
		if (cfg->input_name[0])
			snprintf(kws->cfg.input_name, sizeof(kws->cfg.input_name), "%s", cfg->input_name);
		if (cfg->output_name[0])
			snprintf(kws->cfg.output_name, sizeof(kws->cfg.output_name), "%s", cfg->output_name);
		if (cfg->window_samples >= 320) kws->cfg.window_samples  = cfg->window_samples;
		if (cfg->hop_samples >= 160)    kws->cfg.hop_samples     = cfg->hop_samples;
		if (cfg->threshold > 0.0f)      kws->cfg.threshold       = cfg->threshold;
		if (cfg->min_hits >= 1)         kws->cfg.min_hits        = cfg->min_hits;
		if (cfg->cooldown_frames >= 0)  kws->cfg.cooldown_frames = cfg->cooldown_frames;
		if (cfg->vad_gate_prob > 0.0f)  kws->cfg.vad_gate_prob   = cfg->vad_gate_prob;
	}
	if (wake_word && *wake_word)
		snprintf(kws->wake_word, sizeof(kws->wake_word), "%s", wake_word);

	kws->tier = JARVIS_KWS_TIER_VAD_GATE;

	char resolved[1024] = {0};
	if (jarvis_kws_find_model(kws->wake_word, model_override, resolved, sizeof(resolved))) {
		kws_load_sidecar_cfg(resolved, &kws->cfg);
		if (kws_init_onnx(kws, resolved)) {
			kws->pcm_ring   = (float *)calloc((size_t)kws->cfg.window_samples, sizeof(float));
			kws->pcm_linear = (float *)calloc((size_t)kws->cfg.window_samples, sizeof(float));
			if (kws->pcm_ring && kws->pcm_linear) {
				snprintf(kws->model_path, sizeof(kws->model_path), "%s", resolved);
				kws->tier = JARVIS_KWS_TIER_ONNX;
			} else {
				free(kws->pcm_ring);
				free(kws->pcm_linear);
				kws->pcm_ring = kws->pcm_linear = NULL;
				kws_unload_onnx(kws);
			}
		}
	}

	return kws;
}

void jarvis_kws_close(jarvis_kws_t *kws)
{
	if (!kws)
		return;
	kws_unload_onnx(kws);
	free(kws->pcm_ring);
	free(kws->pcm_linear);
	free(kws);
}

void jarvis_kws_reset(jarvis_kws_t *kws)
{
	if (!kws)
		return;
	if (kws->pcm_ring && kws->cfg.window_samples > 0)
		memset(kws->pcm_ring, 0, (size_t)kws->cfg.window_samples * sizeof(float));
	kws->ring_pos      = 0;
	kws->ring_filled   = 0;
	kws->hop_accum     = 0;
	kws->consec_hits   = 0;
	kws->cooldown_left = 0;
	kws->last_score    = 0.0f;
}

jarvis_kws_tier_t jarvis_kws_tier(const jarvis_kws_t *kws)
{
	return kws ? kws->tier : JARVIS_KWS_TIER_VAD_GATE;
}

const char *jarvis_kws_tier_str(const jarvis_kws_t *kws)
{
	return (kws && kws->tier == JARVIS_KWS_TIER_ONNX) ? "onnx" : "vad";
}

const char *jarvis_kws_model_path(const jarvis_kws_t *kws)
{
	return (kws && kws->model_path[0]) ? kws->model_path : "";
}

uint64_t jarvis_kws_onnx_runs(const jarvis_kws_t *kws)
{
	return kws ? kws->onnx_runs : 0;
}

bool jarvis_kws_vad_gate_slice(const float *vad_probs,
                               int n_frames,
                               int vad_neural,
                               float min_peak_prob,
                               int min_speech_frames)
{
	if (!vad_neural)
		return true;
	if (!vad_probs || n_frames <= 0)
		return false;

	float peak_req = (min_peak_prob > 0.0f) ? min_peak_prob : 0.55f;
	int   cnt_req  = (min_speech_frames > 0) ? min_speech_frames : 3;
	float frame_th = (peak_req > 0.10f) ? (peak_req - 0.10f) : 0.45f;

	float max_p = -1.0f;
	int   speech_cnt = 0;
	int   valid_cnt = 0;
	for (int i = 0; i < n_frames; i++) {
		float p = vad_probs[i];
		if (p < 0.0f)
			continue;
		valid_cnt++;
		if (p > max_p)
			max_p = p;
		if (p >= frame_th)
			speech_cnt++;
	}
	/* If no Silero frames were populated in this slice yet, fail open safely */
	if (valid_cnt == 0)
		return true;

	return (max_p >= peak_req && speech_cnt >= cnt_req);
}

static bool kws_run_onnx_window(jarvis_kws_t *kws, float *out_score)
{
	int W = kws->cfg.window_samples;
	int first = W - kws->ring_pos;
	memcpy(kws->pcm_linear, kws->pcm_ring + kws->ring_pos, (size_t)first * sizeof(float));
	if (kws->ring_pos > 0)
		memcpy(kws->pcm_linear + first, kws->pcm_ring, (size_t)kws->ring_pos * sizeof(float));

	int64_t shape[2] = { 1, (int64_t)W };
	void *in_val = NULL;
	kws_ort_create_tensor_fn create_tensor =
		(kws_ort_create_tensor_fn)kws->ort_vtbl[KWS_ORT_IDX_CreateTensorWithData];
	void *st = create_tensor(kws->ort_mem_info, kws->pcm_linear,
	                         (size_t)W * sizeof(float), shape, 2,
	                         KWS_ONNX_TENSOR_FLOAT, &in_val);
	if (st || !in_val) {
		kws_ort_release_status(kws->ort_vtbl, st);
		return false;
	}

	const char *in_names[1]  = { kws->cfg.input_name };
	const char *out_names[1] = { kws->cfg.output_name };
	const void *in_vals[1]   = { in_val };
	void *out_vals[1]        = { NULL };

	kws_ort_run_fn ort_run = (kws_ort_run_fn)kws->ort_vtbl[KWS_ORT_IDX_Run];
	st = ort_run(kws->ort_sess, NULL, in_names, in_vals, 1, out_names, 1, out_vals);
	((kws_ort_release_fn)kws->ort_vtbl[KWS_ORT_IDX_ReleaseValue])(in_val);
	if (st || !out_vals[0]) {
		kws_ort_release_status(kws->ort_vtbl, st);
		return false;
	}

	bool ok = false;
	void *type_info = NULL;
	size_t n_elems = 0;
	kws_ort_get_tensor_type_shape_fn get_ts =
		(kws_ort_get_tensor_type_shape_fn)kws->ort_vtbl[KWS_ORT_IDX_GetTensorTypeAndShape];
	kws_ort_get_tensor_elem_count_fn get_cnt =
		(kws_ort_get_tensor_elem_count_fn)kws->ort_vtbl[KWS_ORT_IDX_GetTensorShapeElementCount];
	kws_ort_get_tensor_data_fn get_data =
		(kws_ort_get_tensor_data_fn)kws->ort_vtbl[KWS_ORT_IDX_GetTensorMutableData];

	st = get_ts(out_vals[0], &type_info);
	if (!st && type_info) {
		st = get_cnt(type_info, &n_elems);
		((kws_ort_release_fn)kws->ort_vtbl[KWS_ORT_IDX_ReleaseTensorTypeAndShape])(type_info);
	}
	if (st) {
		kws_ort_release_status(kws->ort_vtbl, st);
		n_elems = 0;
	}

	float *data = NULL;
	if (n_elems >= 1) {
		st = get_data(out_vals[0], (void **)&data);
		if (!st && data) {
			/* Support either [1, 1] probability or [1, 2] [bg, wake] output */
			*out_score = (n_elems == 2) ? data[1] : data[0];
			ok = true;
		} else {
			kws_ort_release_status(kws->ort_vtbl, st);
		}
	}

	((kws_ort_release_fn)kws->ort_vtbl[KWS_ORT_IDX_ReleaseValue])(out_vals[0]);
	if (ok)
		kws->onnx_runs++;
	return ok;
}

int jarvis_kws_process_frame(jarvis_kws_t *kws,
                             const float *pcm_f32,
                             int n_samples,
                             float vad_prob,
                             int vad_neural,
                             float *out_score)
{
	if (out_score)
		*out_score = 0.0f;
	if (!kws || kws->tier != JARVIS_KWS_TIER_ONNX || !pcm_f32 || n_samples <= 0)
		return 0;

	int W = kws->cfg.window_samples;
	for (int i = 0; i < n_samples; i++) {
		kws->pcm_ring[kws->ring_pos] = pcm_f32[i];
		kws->ring_pos++;
		if (kws->ring_pos >= W)
			kws->ring_pos = 0;
	}
	if (kws->ring_filled < W) {
		kws->ring_filled += n_samples;
		if (kws->ring_filled > W)
			kws->ring_filled = W;
	}

	if (kws->cooldown_left > 0) {
		kws->cooldown_left--;
		kws->consec_hits = 0;
		return 0;
	}

	/* Gate ONNX inference on Silero VAD when available so idle CPU stays near 0% */
	if (vad_neural && vad_prob >= 0.0f && vad_prob < kws->cfg.vad_gate_prob) {
		kws->consec_hits = 0;
		return 0;
	}

	kws->hop_accum += n_samples;
	if (kws->ring_filled < W || kws->hop_accum < kws->cfg.hop_samples) {
		if (out_score)
			*out_score = kws->last_score;
		return 0;
	}
	kws->hop_accum = 0;

	float score = 0.0f;
	if (!kws_run_onnx_window(kws, &score))
		return 0;

	kws->last_score = score;
	if (out_score)
		*out_score = score;

	if (score >= kws->cfg.threshold) {
		kws->consec_hits++;
		if (kws->consec_hits >= kws->cfg.min_hits) {
			kws->consec_hits = 0;
			kws->cooldown_left = kws->cfg.cooldown_frames;
			return 1;
		}
	} else {
		kws->consec_hits = 0;
	}
	return 0;
}

void jarvis_endpoint_compute(const jarvis_endpoint_in_t *in,
                             jarvis_endpoint_plan_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!in)
		return;

	int frame_ms = (in->frame_ms > 0) ? in->frame_ms : 20;
	int eff_ms   = (in->eff_silence_ms > 0) ? in->eff_silence_ms : 750;

	out->spec_silence_f = in->operating_mode ? 12 : 14;

	int early_f = (eff_ms * 7) / (10 * frame_ms);
	if (early_f < 25) early_f = 25;
	if (early_f > 45) early_f = 45;

	int final_ms = eff_ms;
	int grace_ms = 0;

	/* Adaptive syntactic continuation grace (+450 ms when trailing word is a
	 * locale preposition/conjunction/article from ontology.json). */
	if (in->ends_continuation && !in->operating_mode) {
		grace_ms = 450;
		int max_final_ms = eff_ms + grace_ms;
		if (max_final_ms > 1800)
			max_final_ms = 1800;
		final_ms = max_final_ms;
		int grace_f = grace_ms / frame_ms;
		early_f += grace_f;
		int cap_f = final_ms / frame_ms;
		if (early_f > cap_f)
			early_f = cap_f;
	}

	out->spec_early_f     = early_f;
	out->final_silence_ms = final_ms;
	out->grace_ms_applied = grace_ms;

	/* Suppress premature endpointing while PTT is held or while Silero VAD
	 * still reports active speech (distinguishes brief stop-consonant closures
	 * from true end-of-utterance pauses). */
	if (in->ptt_held || (in->vad_neural && in->last_vad_prob >= 0.45f)) {
		out->suppress_endpoint = 1;
	}
}
