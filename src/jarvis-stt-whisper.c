#define _GNU_SOURCE
#include "jarvis-stt-backend.h"
#include "jarvis-stt.h"
#include "jarvis-platform.h"
#include "../include/ttypt/jarvis.h"
#include "whisper.h"
#include "ggml-backend.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void
whisper_quiet_log(enum ggml_log_level level, const char *text, void *user_data)
{
	(void)level;
	(void)text;
	(void)user_data;
}

static void
trim_ws_inplace(char *s)
{
	if (!s || !*s)
		return;
	char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
}

bool
jarvis_stt_whisper_supports_lang(const char *lang)
{
	(void)lang;
	return true;
}

static bool
whisper_backend_is_multilingual(void *impl)
{
	return impl ? whisper_is_multilingual((struct whisper_context *)impl) : false;
}

static void
normalize_whisper_lang(const char *req_lang, char *out, size_t out_sz)
{
	char resolved[32] = {0};
	if (!req_lang || !*req_lang || strcmp(req_lang, "kbd") == 0) {
		jarvis_resolve_lang(NULL, resolved, sizeof(resolved));
		req_lang = resolved;
	}
	if (!req_lang || !*req_lang || strcasecmp(req_lang, "auto") == 0) {
		snprintf(out, out_sz, "auto");
		return;
	}
	if (strlen(req_lang) >= 2 && (req_lang[2] == '-' || req_lang[2] == '_')) {
		char base[4] = {
			(char)tolower((unsigned char)req_lang[0]),
			(char)tolower((unsigned char)req_lang[1]),
			'\0', '\0'
		};
		snprintf(out, out_sz, "%s", base);
		return;
	}
	snprintf(out, out_sz, "%s", req_lang);
}

static void
append_word_chunk(jarvis_stt_word_t *out, int max_words, int *io_n,
                  const char *piece, int start_ms, int end_ms,
                  int force_new, int slice_ms)
{
	if (!out || !io_n || !piece || !*piece || max_words <= 0)
		return;
	if (start_ms < 0)
		start_ms = 0;
	if (end_ms < start_ms)
		end_ms = start_ms;
	if (slice_ms > 0) {
		if (start_ms > slice_ms)
			start_ms = slice_ms;
		if (end_ms > slice_ms)
			end_ms = slice_ms;
	}

	const char *p = piece;
	int first = 1;
	while (*p) {
		int saw_ws = 0;
		while (*p && isspace((unsigned char)*p)) {
			saw_ws = 1;
			p++;
		}
		if (!*p)
			break;
		const char *wstart = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		size_t wlen = (size_t)(p - wstart);
		int start_new = (saw_ws || !first || force_new || *io_n == 0);
		first = 0;

		if (start_new) {
			if (*io_n >= max_words)
				return;
			jarvis_stt_word_t *w = &out[*io_n];
			size_t copy = (wlen < sizeof(w->text) - 1) ? wlen : (sizeof(w->text) - 1);
			memcpy(w->text, wstart, copy);
			w->text[copy] = '\0';
			w->start_ms = start_ms;
			w->end_ms = end_ms;
			if (*io_n > 0 && w->start_ms < out[*io_n - 1].start_ms)
				w->start_ms = out[*io_n - 1].start_ms;
			if (w->end_ms < w->start_ms)
				w->end_ms = w->start_ms;
			(*io_n)++;
		} else {
			jarvis_stt_word_t *w = &out[*io_n - 1];
			size_t cur = strlen(w->text);
			size_t avail = sizeof(w->text) - 1 - cur;
			size_t copy = (wlen < avail) ? wlen : avail;
			if (copy > 0) {
				memcpy(w->text + cur, wstart, copy);
				w->text[cur + copy] = '\0';
			}
			if (end_ms > w->end_ms)
				w->end_ms = end_ms;
		}
	}
}

static int
word_eq_ci_alnum(const char *a, const char *b)
{
	while (*a && ispunct((unsigned char)*a))
		a++;
	while (*b && ispunct((unsigned char)*b))
		b++;
	size_t la = strlen(a), lb = strlen(b);
	while (la > 0 && ispunct((unsigned char)a[la - 1]))
		la--;
	while (lb > 0 && ispunct((unsigned char)b[lb - 1]))
		lb--;
	if (la == 0 || la != lb)
		return 0;
	for (size_t i = 0; i < la; i++) {
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
			return 0;
	}
	return 1;
}

static void
collapse_word_repetitions(jarvis_stt_word_t *words, int *io_n)
{
	if (!words || !io_n || *io_n < 3)
		return;
	int n = *io_n;
	int out = 0;
	int i = 0;
	while (i < n) {
		int collapsed = 0;
		for (int k = 1; k <= 12 && i + 2 * k <= n; k++) {
			int min_reps = (k == 1) ? 3 : 2;
			int reps = 1;
			while (i + (reps + 1) * k <= n) {
				int match = 1;
				for (int j = 0; j < k; j++) {
					if (!word_eq_ci_alnum(words[i + j].text,
					                      words[i + reps * k + j].text)) {
						match = 0;
						break;
					}
				}
				if (!match)
					break;
				reps++;
			}
			if (reps >= min_reps) {
				for (int j = 0; j < k; j++)
					words[out++] = words[i + j];
				i += reps * k;
				collapsed = 1;
				break;
			}
		}
		if (!collapsed)
			words[out++] = words[i++];
	}
	*io_n = out;
}

static int
whisper_backend_transcribe(void *impl, const jarvis_stt_req_t *req, jarvis_stt_res_t *res)
{
	struct whisper_context *wctx = (struct whisper_context *)impl;
	memset(res, 0, sizeof(*res));
	if (!wctx || !req->pcm || req->n_samples < 1600) {
		res->all_silent = 1;
		return 0;
	}

	const float *pcm = req->pcm;
	int n_samples = req->n_samples;
	int max_tok = req->max_tok;
	bool want_words = (req->out_words != NULL && req->max_words > 0);

	char eff_lang[32] = {0};
	normalize_whisper_lang(req->lang, eff_lang, sizeof(eff_lang));

	if (req->is_partial && n_samples > 30 * JARVIS_SAMPLE_RATE) {
		pcm += (n_samples - 30 * JARVIS_SAMPLE_RATE);
		n_samples = 30 * JARVIS_SAMPLE_RATE;
	}
	int slice_ms = (int)(((int64_t)n_samples * 1000) / JARVIS_SAMPLE_RATE);

	if (req->is_partial) {
		int cap = (n_samples / 2200) + 6;
		if (max_tok <= 0 || cap < max_tok)
			max_tok = cap;
	}

	bool is_final_cmd = (!req->is_wake && !req->is_partial);
	int beam_sz = is_final_cmd ? req->beam_size : 1;
	if (beam_sz < 1)
		beam_sz = 1;
	if (beam_sz > 5)
		beam_sz = 5;
	int use_beam = (is_final_cmd && beam_sz > 1);

	struct whisper_full_params wparams = whisper_full_default_params(
		use_beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
	wparams.n_threads        = (req->n_threads > 0) ? req->n_threads : 4;
	/* Partials stay single_segment=true (matching whisper.cpp stream example):
	 * multi-segment decoding on short partials with temperature_inc=0 hallucinates
	 * extra repeating segments into zero-padded encoder frames, whereas
	 * single_segment=true + no_timestamps=false + token_timestamps=true still
	 * computes full token-level timestamps in whisper_full() (line 7905). */
	wparams.single_segment   = is_final_cmd ? false : true;
	/* whisper.cpp only uses no_context to clear prior whisper_full() prompt_past;
	 * initial_prompt is always tokenized into a fresh prompt_past1 afterwards.
	 * Keeping no_context=false across overlapping partials accumulates duplicate
	 * history into prompt_past1 and triggers hallucination loops. */
	wparams.no_context       = (req->use_context && !req->is_partial &&
	                            (!req->prompt || !req->prompt[0])) ? false : true;
	wparams.no_timestamps    = (is_final_cmd || want_words) ? false : true;
	wparams.token_timestamps = want_words ? true : false;
	wparams.suppress_blank   = true;
	wparams.suppress_nst     = true;
	wparams.temperature      = 0.0f;
	wparams.temperature_inc  = is_final_cmd ? 0.2f : 0.0f;
	wparams.entropy_thold    = 2.4f;
	wparams.logprob_thold    = -1.0f;
	wparams.no_speech_thold  = 0.6f;
	if (use_beam) {
		wparams.beam_search.beam_size = beam_sz;
	} else {
		wparams.greedy.best_of = 1;
	}
	wparams.max_tokens = max_tok;
	wparams.audio_ctx  = req->audio_ctx;

	if (req->abort_cb) {
		wparams.abort_callback = req->abort_cb;
		wparams.abort_callback_user_data = req->abort_ud;
	}

	if (!whisper_is_multilingual(wctx)) {
		wparams.language = "en";
		wparams.detect_language = false;
	} else if (!eff_lang[0] || strcmp(eff_lang, "auto") == 0) {
		wparams.language = "auto";
		wparams.detect_language = false;
	} else {
		wparams.language = eff_lang;
	}

	if (req->prompt && *req->prompt && strcmp(req->prompt, "-") != 0)
		wparams.initial_prompt = req->prompt;

	float *agc_pcm = req->apply_agc ? jarvis_stt_normalize_pcm(pcm, n_samples, 0) : NULL;
	const float *eff_pcm = agc_pcm ? agc_pcm : pcm;

	int rc = whisper_full(wctx, wparams, eff_pcm, n_samples);
	free(agc_pcm);

	if (rc != 0) {
		res->all_silent = 1;
		return rc;
	}

	int lid = whisper_full_lang_id(wctx);
	const char *lstr = (lid >= 0) ? whisper_lang_str(lid) : NULL;
	if (lstr && *lstr)
		snprintf(res->detected_lang, sizeof(res->detected_lang), "%s", lstr);

	int n_seg = whisper_full_n_segments(wctx);
	int valid_seg = 0;
	float nsp_limit = req->is_wake ? 0.68f : 0.58f;
	whisper_token eot_id = whisper_token_eot(wctx);
	char prev_seg[1024] = {0};

	for (int i = 0; i < n_seg; i++) {
		float nsp = whisper_full_get_segment_no_speech_prob(wctx, i);
		if (nsp > nsp_limit)
			continue;

		int n_tok = whisper_full_n_tokens(wctx, i);
		float sum_p = 0.0f;
		int word_toks = 0;
		int end_tok = n_tok;

		for (int j = 0; j < n_tok; j++) {
			whisper_token_data td = whisper_full_get_token_data(wctx, i, j);
			const char *tt = whisper_full_get_token_text(wctx, i, j);
			if (td.id >= eot_id || !tt || !*tt)
				continue;
			if (tt[0] == '[' && tt[strlen(tt) - 1] == ']')
				continue;
			sum_p += td.p;
			word_toks++;
		}
		if (word_toks > 0 && (sum_p / (float)word_toks) < (req->is_wake ? 0.20f : 0.18f))
			continue;

		if (!req->is_wake && word_toks >= 2) {
			while (end_tok > 1) {
				whisper_token_data td = whisper_full_get_token_data(wctx, i, end_tok - 1);
				const char *tt = whisper_full_get_token_text(wctx, i, end_tok - 1);
				if (td.id >= eot_id || !tt || !*tt) {
					end_tok--;
					continue;
				}
				int has_alnum = 0;
				for (const char *c = tt; *c; c++) {
					unsigned char uc = (unsigned char)*c;
					if (isalnum(uc) || (uc >= 0xC2 && uc <= 0xDF) || uc >= 0xE3) {
						has_alnum = 1;
						break;
					}
				}
				if (!has_alnum) {
					end_tok--;
					continue;
				}
				if (td.p < 0.03f) {
					end_tok--;
					continue;
				}
				break;
			}
		}

		char seg_clean[1024] = {0};
		if (!req->is_wake && end_tok < n_tok && end_tok > 0) {
			for (int j = 0; j < end_tok; j++) {
				whisper_token_data td = whisper_full_get_token_data(wctx, i, j);
				if (td.id >= eot_id)
					continue;
				const char *tt = whisper_full_get_token_text(wctx, i, j);
				if (tt && *tt)
					strncat(seg_clean, tt, sizeof(seg_clean) - strlen(seg_clean) - 1);
			}
		} else {
			const char *seg = whisper_full_get_segment_text(wctx, i);
			if (!seg)
				continue;
			snprintf(seg_clean, sizeof(seg_clean), "%s", seg);
		}
		trim_ws_inplace(seg_clean);
		if (jarvis_is_non_speech_hallucination(seg_clean))
			continue;
		if (prev_seg[0] != '\0' && strcasecmp(prev_seg, seg_clean) == 0)
			continue;
		snprintf(prev_seg, sizeof(prev_seg), "%s", seg_clean);
		valid_seg++;
		if (res->text[0] != '\0')
			strncat(res->text, " ", sizeof(res->text) - strlen(res->text) - 1);
		strncat(res->text, seg_clean, sizeof(res->text) - strlen(res->text) - 1);

		if (want_words && res->n_words < req->max_words) {
			int seg_word_start = res->n_words;
			int seg_t0_ms = (int)(whisper_full_get_segment_t0(wctx, i) * 10);
			int seg_t1_ms = (int)(whisper_full_get_segment_t1(wctx, i) * 10);
			if (seg_t0_ms < 0)
				seg_t0_ms = 0;
			if (seg_t1_ms <= seg_t0_ms || seg_t1_ms > slice_ms)
				seg_t1_ms = slice_ms;

			int tok_limit = (!req->is_wake && end_tok < n_tok && end_tok > 0) ? end_tok : n_tok;
			int any_nonzero_ts = 0;
			int first_seg_tok = 1;
			for (int j = 0; j < tok_limit && res->n_words < req->max_words; j++) {
				whisper_token_data td = whisper_full_get_token_data(wctx, i, j);
				const char *tt = whisper_full_get_token_text(wctx, i, j);
				if (td.id >= eot_id || !tt || !*tt)
					continue;
				if (tt[0] == '[' && tt[strlen(tt) - 1] == ']')
					continue;
				int t0_ms = (int)(td.t0 * 10);
				int t1_ms = (int)(td.t1 * 10);
				if (t0_ms > 0 || t1_ms > 0)
					any_nonzero_ts = 1;
				append_word_chunk(req->out_words, req->max_words, &res->n_words,
				                  tt, t0_ms, t1_ms, first_seg_tok, slice_ms);
				first_seg_tok = 0;
			}
			int seg_nw = res->n_words - seg_word_start;
			if (seg_nw > 0 && !any_nonzero_ts) {
				int span = (seg_t1_ms > seg_t0_ms) ? (seg_t1_ms - seg_t0_ms) : 1;
				for (int k = 0; k < seg_nw; k++) {
					req->out_words[seg_word_start + k].start_ms = seg_t0_ms + (span * k) / seg_nw;
					req->out_words[seg_word_start + k].end_ms   = seg_t0_ms + (span * (k + 1)) / seg_nw;
				}
			}
		}
	}

	if (valid_seg == 0)
		res->all_silent = 1;

	trim_ws_inplace(res->text);
	jarvis_stt_collapse_repetitions(res->text, sizeof(res->text));
	if (want_words && res->n_words > 0)
		collapse_word_repetitions(req->out_words, &res->n_words);
	return 0;
}

static void
whisper_backend_destroy(void *impl)
{
	if (impl)
		whisper_free((struct whisper_context *)impl);
}

static const jarvis_stt_backend_ops_t k_whisper_ops = {
	.name            = "whisper",
	.supports_lang   = jarvis_stt_whisper_supports_lang,
	.is_multilingual = whisper_backend_is_multilingual,
	.transcribe      = whisper_backend_transcribe,
	.destroy         = whisper_backend_destroy
};

jarvis_stt_ctx_t *
jarvis_stt_open_whisper(const char *model_path, bool use_gpu)
{
	if (!model_path || !*model_path)
		return NULL;

	whisper_log_set(whisper_quiet_log, NULL);

	struct whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = use_gpu;
	cparams.flash_attn = true;

	struct whisper_context *wctx = whisper_init_from_file_with_params(model_path, cparams);
	if (!wctx)
		return NULL;

	jarvis_stt_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		whisper_free(wctx);
		return NULL;
	}
	ctx->ops = &k_whisper_ops;
	ctx->impl = wctx;
	snprintf(ctx->model_path, sizeof(ctx->model_path), "%s", model_path);
	return ctx;
}

jarvis_stt_ctx_t *
jarvis_stt_open_auto(const char *model_path, bool use_gpu)
{
	if (!model_path || !*model_path)
		return NULL;
	if (jarvis_stt_model_looks_parakeet(model_path)) {
		jarvis_stt_ctx_t *ctx = jarvis_stt_open_parakeet(model_path, use_gpu);
		if (ctx)
			return ctx;
		return jarvis_stt_open_whisper(model_path, use_gpu);
	}
	jarvis_stt_ctx_t *ctx = jarvis_stt_open_whisper(model_path, use_gpu);
	if (ctx)
		return ctx;
	return jarvis_stt_open_parakeet(model_path, use_gpu);
}

void
jarvis_stt_close(jarvis_stt_ctx_t *ctx)
{
	if (!ctx)
		return;
	if (ctx->ops && ctx->ops->destroy && ctx->impl)
		ctx->ops->destroy(ctx->impl);
	ctx->impl = NULL;
	ctx->ops = NULL;
	free(ctx);
}

/* -------------------------------------------------------------------------
 * Hardware discovery & startup latency calibration
 * ------------------------------------------------------------------------- */

const char *
jarvis_hw_tier_str(jarvis_hw_tier_t tier)
{
	switch (tier) {
	case JARVIS_HW_FAST_GPU: return "fast_gpu";
	case JARVIS_HW_IGPU:     return "igpu";
	default:                 return "cpu";
	}
}

void
jarvis_hw_probe_devices(jarvis_hw_profile_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->tier = JARVIS_HW_CPU;
	snprintf(out->backend, sizeof(out->backend), "CPU");
	snprintf(out->device, sizeof(out->device), "CPU");
	out->final_audio_ctx = 512;
	out->beam_size = 1;

	ggml_backend_load_all();

	ggml_backend_dev_t best = NULL;
	int best_rank = -1; /* 0=CPU, 1=IGPU, 2=GPU */

	size_t n_dev = ggml_backend_dev_count();
	for (size_t i = 0; i < n_dev; i++) {
		ggml_backend_dev_t dev = ggml_backend_dev_get(i);
		if (!dev)
			continue;
		enum ggml_backend_dev_type dtype = ggml_backend_dev_type(dev);
		int rank = -1;
		if (dtype == GGML_BACKEND_DEVICE_TYPE_GPU)
			rank = 2;
		else if (dtype == GGML_BACKEND_DEVICE_TYPE_IGPU)
			rank = 1;
		else if (dtype == GGML_BACKEND_DEVICE_TYPE_CPU)
			rank = 0;

		if (rank > best_rank) {
			best_rank = rank;
			best = dev;
		}
	}

	if (!best)
		return;

	ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(best);
	const char *rname = reg ? ggml_backend_reg_name(reg) : NULL;
	const char *dname = ggml_backend_dev_description(best);
	if (!dname || !*dname)
		dname = ggml_backend_dev_name(best);

	if (rname && *rname)
		snprintf(out->backend, sizeof(out->backend), "%s", rname);
	if (dname && *dname)
		snprintf(out->device, sizeof(out->device), "%s", dname);

	if (best_rank == 2) {
		out->has_gpu = 1;
		out->is_igpu = 0;
		out->tier = JARVIS_HW_FAST_GPU;
		out->final_audio_ctx = 0;
	} else if (best_rank == 1) {
		out->has_gpu = 1;
		out->is_igpu = 1;
		out->tier = JARVIS_HW_IGPU;
		out->final_audio_ctx = 512;
	} else {
		out->has_gpu = 0;
		out->is_igpu = 0;
		out->tier = JARVIS_HW_CPU;
		out->final_audio_ctx = 512;
	}
}

void
jarvis_hw_calibrate_profile(jarvis_hw_profile_t *prof, int ac512_ms, int ac1500_ms)
{
	if (!prof)
		return;
	prof->ac512_ms = ac512_ms;
	prof->ac1500_ms = ac1500_ms;
	prof->beam_size = 1;

	if (prof->has_gpu && ac1500_ms > 0 && ac1500_ms <= 650) {
		prof->tier = JARVIS_HW_FAST_GPU;
		prof->final_audio_ctx = 0;
		return;
	}

	if (prof->has_gpu && ac512_ms > 0 && ac512_ms <= 900) {
		prof->tier = JARVIS_HW_IGPU;
		prof->final_audio_ctx = (ac512_ms <= 450) ? 768 : 512;
		return;
	}

	if (!prof->has_gpu && ac512_ms > 0 && ac512_ms <= 450) {
		prof->tier = JARVIS_HW_CPU;
		prof->final_audio_ctx = 768;
		return;
	}

	prof->tier = prof->has_gpu ? JARVIS_HW_IGPU : JARVIS_HW_CPU;
	prof->final_audio_ctx = 512;
}

void
jarvis_hw_warmup_and_calibrate(jarvis_hw_profile_t *prof,
                               jarvis_stt_ctx_t    *main_ctx,
                               jarvis_stt_ctx_t    *wake_ctx,
                               int                  n_threads,
                               const char          *lang)
{
	if (!prof)
		return;
	jarvis_hw_probe_devices(prof);

	if (!main_ctx)
		return;

	static float warm_pcm[16000]; /* 1.0s silence in BSS (zero stack pressure) */
	const char *eff_lang = (lang && *lang) ? lang : "en";
	jarvis_stt_res_t res;

	/* Warm wake model (short 200ms pass so pipelines & arena buffers are hot) */
	if (wake_ctx && wake_ctx != main_ctx) {
		jarvis_stt_req_t wreq;
		memset(&wreq, 0, sizeof(wreq));
		wreq.pcm = warm_pcm;
		wreq.n_samples = 3200;
		wreq.n_threads = n_threads;
		wreq.lang = jarvis_stt_is_multilingual(wake_ctx) ? eff_lang : "en";
		wreq.max_tok = 1;
		wreq.audio_ctx = 128;
		wreq.beam_size = 1;
		wreq.is_wake = 1;
		jarvis_stt_transcribe(wake_ctx, &wreq, &res);
	}

	/* Length-proportional backends (Parakeet-TDT) don't use Whisper audio_ctx */
	if (strcmp(jarvis_stt_backend_name(main_ctx), "parakeet") == 0) {
		jarvis_stt_req_t preq;
		memset(&preq, 0, sizeof(preq));
		preq.pcm = warm_pcm;
		preq.n_samples = 16000;
		preq.n_threads = n_threads;
		preq.lang = eff_lang;
		preq.max_tok = 1;
		preq.beam_size = 1;
		long t0 = (long)stl_now_ms();
		jarvis_stt_transcribe(main_ctx, &preq, &res);
		int ms = (int)((long)stl_now_ms() - t0);
		if (ms < 1)
			ms = 1;
		prof->ac512_ms = ms;
		prof->ac1500_ms = 0;
		prof->final_audio_ctx = 0;
		prof->beam_size = 1;
		return;
	}

	/* First tiny warmup pass compiles GPU shaders / warms arena without polluting timing */
	jarvis_stt_req_t req;
	memset(&req, 0, sizeof(req));
	req.pcm = warm_pcm;
	req.n_samples = 3200;
	req.n_threads = n_threads;
	req.lang = jarvis_stt_is_multilingual(main_ctx) ? eff_lang : "en";
	req.max_tok = 1;
	req.audio_ctx = 128;
	req.beam_size = 1;
	req.is_partial = 1;
	jarvis_stt_transcribe(main_ctx, &req, &res);

	/* Timed steady-state audio_ctx=512 probe */
	req.n_samples = 16000;
	req.audio_ctx = 512;
	long t0 = (long)stl_now_ms();
	jarvis_stt_transcribe(main_ctx, &req, &res);
	int ac512_ms = (int)((long)stl_now_ms() - t0);
	if (ac512_ms < 1)
		ac512_ms = 1;

	/* Only probe full audio_ctx=0 (1500) on fast GPUs / Apple Silicon where
	 * ac512 is already <= 550ms, avoiding a 2s startup stall on laptop iGPUs/CPUs.
	 */
	int ac1500_ms = 0;
	if (prof->has_gpu && ac512_ms <= 550) {
		req.audio_ctx = 0;
		long t1 = (long)stl_now_ms();
		jarvis_stt_transcribe(main_ctx, &req, &res);
		ac1500_ms = (int)((long)stl_now_ms() - t1);
		if (ac1500_ms < 1)
			ac1500_ms = 1;
	}

	jarvis_hw_calibrate_profile(prof, ac512_ms, ac1500_ms);
}
