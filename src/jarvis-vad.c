#include "jarvis-vad.h"
#include "jarvis-platform.h"
#include "whisper.h"
#include <ttypt/jarvis.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int
try_model_path(const char *cand, char *out_path, size_t out_sz)
{
	if (!cand || !*cand)
		return 0;
	if (!jarvis_plat_is_readable(cand))
		return 0;
	snprintf(out_path, out_sz, "%s", cand);
	return 1;
}

int
jarvis_nvad_resolve_model(char *out_path, size_t out_sz)
{
	if (!out_path || out_sz == 0)
		return -1;
	out_path[0] = '\0';

	/* 1. Explicit ~/.jarvisrc override (`vad_model = off|none|-|<path>`) */
	char rc_val[512] = {0};
	if (jarvis_rc_get("vad_model", "", rc_val, sizeof(rc_val)) && rc_val[0]) {
		if (strcmp(rc_val, "-") == 0 || strcasecmp(rc_val, "off") == 0 ||
		    strcasecmp(rc_val, "none") == 0 || strcmp(rc_val, "0") == 0)
			return -1;
		if (try_model_path(rc_val, out_path, out_sz))
			return 0;
	}

	/* 2. Environment variable override */
	const char *env_p = getenv("JARVIS_VAD_MODEL");
	if (env_p && *env_p) {
		if (strcmp(env_p, "-") == 0 || strcasecmp(env_p, "off") == 0 ||
		    strcasecmp(env_p, "none") == 0)
			return -1;
		if (try_model_path(env_p, out_path, out_sz))
			return 0;
	}

	static const char *const names[] = {
		"ggml-silero-v6.2.0.bin",
		"ggml-silero-v5.1.2.bin",
		"silero-vad.bin",
		NULL
	};

	/* 3. XDG_DATA_HOME / $HOME/.local/share/{whisper,jarvis/models} */
	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char base[512];
	char cand[640];

	if (xdg && *xdg) {
		for (int i = 0; names[i]; i++) {
			snprintf(cand, sizeof(cand), "%s/whisper/%s", xdg, names[i]);
			if (try_model_path(cand, out_path, out_sz)) return 0;
			snprintf(cand, sizeof(cand), "%s/jarvis/models/%s", xdg, names[i]);
			if (try_model_path(cand, out_path, out_sz)) return 0;
		}
	}
	if (home && *home) {
		snprintf(base, sizeof(base), "%s/.local/share", home);
		for (int i = 0; names[i]; i++) {
			snprintf(cand, sizeof(cand), "%s/whisper/%s", base, names[i]);
			if (try_model_path(cand, out_path, out_sz)) return 0;
			snprintf(cand, sizeof(cand), "%s/jarvis/models/%s", base, names[i]);
			if (try_model_path(cand, out_path, out_sz)) return 0;
		}
	}

	/* 4. System install directories */
	static const char *const sys_dirs[] = {
		"/usr/local/share/jarvis/models",
		"/usr/share/jarvis/models",
		"/usr/local/share/whisper",
		"/usr/share/whisper",
		NULL
	};
	for (int d = 0; sys_dirs[d]; d++) {
		for (int i = 0; names[i]; i++) {
			snprintf(cand, sizeof(cand), "%s/%s", sys_dirs[d], names[i]);
			if (try_model_path(cand, out_path, out_sz)) return 0;
		}
	}

	/* 5. Source-tree submodule model fallback (dev / tests) */
	static const char *const dev_cands[] = {
		"submodules/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin",
		"../submodules/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin",
		"external/libjarvis/submodules/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin",
		NULL
	};
	for (int i = 0; dev_cands[i]; i++) {
		if (try_model_path(dev_cands[i], out_path, out_sz))
			return 0;
	}

	return -1;
}

int
jarvis_nvad_init(jarvis_nvad_t *vad, const char *model_path, int n_threads)
{
	if (!vad)
		return -1;
	memset(vad, 0, sizeof(*vad));
	vad->last_prob = -1.0f;
	vad->n_threads = (n_threads > 0) ? n_threads : 2;
	if (vad->n_threads > 4)
		vad->n_threads = 4;

	if (!model_path || !*model_path || !jarvis_plat_is_readable(model_path))
		return 0; /* graceful energy-VAD fallback */

	struct whisper_vad_context_params vcp = whisper_vad_default_context_params();
	vcp.n_threads = vad->n_threads;
	vcp.use_gpu = false; /* Silero is < 0.2 ms/chunk on CPU; keep GPU free for ASR */
	vcp.gpu_device = 0;

	struct whisper_vad_context *vctx = whisper_vad_init_from_file_with_params(model_path, vcp);
	if (!vctx)
		return 0; /* graceful fallback on corrupt/incompatible file */

	vad->vctx = vctx;
	whisper_vad_reset_state(vctx);
	vad->available = 1;
	vad->last_prob = 0.0f;
	snprintf(vad->model_path, sizeof(vad->model_path), "%s", model_path);
	return 0;
}

void
jarvis_nvad_reset(jarvis_nvad_t *vad)
{
	if (!vad)
		return;
	vad->buf_fill = 0;
	if (vad->available && vad->vctx) {
		whisper_vad_reset_state(vad->vctx);
		vad->last_prob = 0.0f;
	} else {
		vad->last_prob = -1.0f;
	}
}

float
jarvis_nvad_process(jarvis_nvad_t *vad, const float *pcm, size_t n_samples)
{
	if (!vad || !vad->available || !vad->vctx)
		return -1.0f;
	if (!pcm || n_samples == 0)
		return vad->last_prob;

	size_t off = 0;
	int ran = 0;
	float max_p = 0.0f;

	while (off < n_samples) {
		size_t need = (size_t)(JARVIS_VAD_CHUNK_SAMPLES - vad->buf_fill);
		size_t take = n_samples - off;
		if (take > need)
			take = need;
		memcpy(vad->buf + vad->buf_fill, pcm + off, take * sizeof(float));
		vad->buf_fill += (int)take;
		off += take;

		if (vad->buf_fill == JARVIS_VAD_CHUNK_SAMPLES) {
			if (whisper_vad_detect_speech_no_reset(vad->vctx, vad->buf, JARVIS_VAD_CHUNK_SAMPLES)) {
				int np = whisper_vad_n_probs(vad->vctx);
				const float *probs = whisper_vad_probs(vad->vctx);
				if (np > 0 && probs) {
					float p = probs[np - 1];
					if (p < 0.0f) p = 0.0f;
					if (p > 1.0f) p = 1.0f;
					if (!ran || p > max_p)
						max_p = p;
					ran = 1;
				}
			}
			vad->buf_fill = 0;
		}
	}

	if (ran)
		vad->last_prob = max_p;
	return vad->last_prob;
}

int
jarvis_nvad_is_available(const jarvis_nvad_t *vad)
{
	return (vad && vad->available && vad->vctx) ? 1 : 0;
}

void
jarvis_nvad_free(jarvis_nvad_t *vad)
{
	if (!vad)
		return;
	if (vad->vctx) {
		whisper_vad_free(vad->vctx);
		vad->vctx = NULL;
	}
	vad->available = 0;
	vad->buf_fill = 0;
	vad->last_prob = -1.0f;
}
