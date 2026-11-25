#define _GNU_SOURCE
#include "jarvis-stt-backend.h"
#include "jarvis-stt.h"
#include "../include/ttypt/jarvis.h"
#include "parakeet.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void
parakeet_quiet_log(enum ggml_log_level level, const char *text, void *user_data)
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

static bool
str_contains_ci(const char *hay, const char *needle)
{
	if (!hay || !needle || !*needle)
		return false;
	size_t nlen = strlen(needle);
	for (const char *p = hay; *p; p++) {
		size_t i = 0;
		while (i < nlen && p[i] &&
		       tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == nlen)
			return true;
	}
	return false;
}

bool
jarvis_stt_parakeet_supports_lang(const char *lang)
{
	/* Parakeet-TDT-0.6B-v3 supports 25 European languages */
	static const char *const k_langs[] = {
		"bg", "hr", "cs", "da", "nl",
		"en", "et", "fi", "fr", "de",
		"el", "hu", "it", "lv", "lt",
		"mt", "pl", "pt", "ro", "ru",
		"sk", "sl", "es", "sv", "uk",
		NULL
	};
	if (!lang || !*lang || strcasecmp(lang, "auto") == 0)
		return true;

	char base[16] = {0};
	size_t i = 0;
	while (lang[i] && lang[i] != '-' && lang[i] != '_' && i + 1 < sizeof(base)) {
		base[i] = (char)tolower((unsigned char)lang[i]);
		i++;
	}
	base[i] = '\0';

	for (size_t k = 0; k_langs[k]; k++) {
		if (strcmp(base, k_langs[k]) == 0)
			return true;
	}
	return false;
}

bool
jarvis_stt_model_looks_parakeet(const char *model_path)
{
	if (!model_path || !*model_path)
		return false;
	if (str_contains_ci(model_path, "parakeet"))
		return true;

	FILE *fp = fopen(model_path, "rb");
	if (!fp)
		return false;
	uint32_t magic = 0;
	int32_t n_vocab = 0;
	bool ok = (fread(&magic, sizeof(magic), 1, fp) == 1 &&
	           fread(&n_vocab, sizeof(n_vocab), 1, fp) == 1);
	fclose(fp);

	/* Both Whisper and Parakeet GGML binaries start with GGML_FILE_MAGIC
	 * (0x67676d6c) followed by n_vocab. Whisper models always have
	 * n_vocab >= 50000; Parakeet BPE vocab is <= 16384.
	 */
	return ok && magic == 0x67676d6cU && n_vocab > 0 && n_vocab < 32768;
}

static bool
parakeet_is_multilingual(void *impl)
{
	(void)impl;
	return true;
}

static void
append_parakeet_word_chunk(jarvis_stt_word_t *out, int max_words, int *io_n,
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

	const unsigned char *p = (const unsigned char *)piece;
	int first = 1;
	while (*p) {
		int saw_sep = 0;
		while (*p) {
			if (isspace(*p)) {
				saw_sep = 1;
				p++;
			} else if (p[0] == 0xE2 && p[1] == 0x96 && p[2] == 0x81) {
				/* SentencePiece U+2581 LOWER ONE EIGHTH BLOCK word boundary */
				saw_sep = 1;
				p += 3;
			} else {
				break;
			}
		}
		if (!*p)
			break;
		const unsigned char *wstart = p;
		while (*p && !isspace(*p) && !(p[0] == 0xE2 && p[1] == 0x96 && p[2] == 0x81))
			p++;
		size_t wlen = (size_t)(p - wstart);
		int start_new = (saw_sep || !first || force_new || *io_n == 0);
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
parakeet_transcribe(void *impl, const jarvis_stt_req_t *req, jarvis_stt_res_t *res)
{
	struct parakeet_context *pctx = (struct parakeet_context *)impl;
	memset(res, 0, sizeof(*res));
	if (!pctx || !req->pcm || req->n_samples < 1600) {
		res->all_silent = 1;
		return 0;
	}

	bool want_words = (req->out_words != NULL && req->max_words > 0);
	int slice_ms = (int)(((int64_t)req->n_samples * 1000) / JARVIS_SAMPLE_RATE);

	struct parakeet_full_params params = parakeet_full_default_params(PARAKEET_SAMPLING_GREEDY);
	params.n_threads = (req->n_threads > 0) ? req->n_threads : 4;
	params.no_context = req->use_context ? false : true;
	params.audio_ctx = 0; /* length-proportional dynamic encoder */
	if (req->abort_cb) {
		params.abort_callback = req->abort_cb;
		params.abort_callback_user_data = req->abort_ud;
	}

	float *agc_pcm = req->apply_agc ? jarvis_stt_normalize_pcm(req->pcm, req->n_samples, 0) : NULL;
	const float *eff_pcm = agc_pcm ? agc_pcm : req->pcm;

	int rc = parakeet_full(pctx, params, eff_pcm, req->n_samples);
	free(agc_pcm);

	if (rc != 0) {
		res->all_silent = 1;
		return rc;
	}

	int n_seg = parakeet_full_n_segments(pctx);
	int valid_seg = 0;
	char prev_seg[1024] = {0};

	for (int i = 0; i < n_seg; i++) {
		const char *seg = parakeet_full_get_segment_text(pctx, i);
		if (!seg || !*seg)
			continue;

		char seg_clean[1024] = {0};
		snprintf(seg_clean, sizeof(seg_clean), "%s", seg);
		trim_ws_inplace(seg_clean);
		if (!seg_clean[0])
			continue;
		if (jarvis_is_non_speech_hallucination(seg_clean))
			continue;
		if (prev_seg[0] && strcasecmp(prev_seg, seg_clean) == 0)
			continue;
		snprintf(prev_seg, sizeof(prev_seg), "%s", seg_clean);

		valid_seg++;
		if (res->text[0] != '\0')
			strncat(res->text, " ", sizeof(res->text) - strlen(res->text) - 1);
		strncat(res->text, seg_clean, sizeof(res->text) - strlen(res->text) - 1);

		if (want_words && res->n_words < req->max_words) {
			int seg_word_start = res->n_words;
			int seg_t0_ms = (int)(parakeet_full_get_segment_t0(pctx, i) * 10);
			int seg_t1_ms = (int)(parakeet_full_get_segment_t1(pctx, i) * 10);
			if (seg_t0_ms < 0)
				seg_t0_ms = 0;
			if (seg_t1_ms <= seg_t0_ms || seg_t1_ms > slice_ms)
				seg_t1_ms = slice_ms;

			int n_tok = parakeet_full_n_tokens(pctx, i);
			int any_nonzero_ts = 0;
			int first_seg_tok = 1;
			for (int j = 0; j < n_tok && res->n_words < req->max_words; j++) {
				parakeet_token_data td = parakeet_full_get_token_data(pctx, i, j);
				const char *tt = parakeet_full_get_token_text(pctx, i, j);
				if (!tt || !*tt)
					continue;
				int t0_ms = (int)(td.t0 * 10);
				int t1_ms = (int)(td.t1 * 10);
				if (t0_ms > 0 || t1_ms > 0)
					any_nonzero_ts = 1;
				append_parakeet_word_chunk(req->out_words, req->max_words, &res->n_words,
				                           tt, t0_ms, t1_ms,
				                           (first_seg_tok || td.is_word_start) ? 1 : 0,
				                           slice_ms);
				first_seg_tok = 0;
			}
			int seg_nw = res->n_words - seg_word_start;
			if (seg_nw == 0) {
				append_parakeet_word_chunk(req->out_words, req->max_words, &res->n_words,
				                           seg_clean, seg_t0_ms, seg_t1_ms, 1, slice_ms);
				seg_nw = res->n_words - seg_word_start;
				any_nonzero_ts = 0;
			}
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
	if (!res->text[0])
		res->all_silent = 1;

	if (req->lang && *req->lang && strcasecmp(req->lang, "auto") != 0)
		snprintf(res->detected_lang, sizeof(res->detected_lang), "%s", req->lang);

	return 0;
}

static void
parakeet_destroy(void *impl)
{
	if (impl)
		parakeet_free((struct parakeet_context *)impl);
}

static const jarvis_stt_backend_ops_t k_parakeet_ops = {
	.name            = "parakeet",
	.supports_lang   = jarvis_stt_parakeet_supports_lang,
	.is_multilingual = parakeet_is_multilingual,
	.transcribe      = parakeet_transcribe,
	.destroy         = parakeet_destroy
};

jarvis_stt_ctx_t *
jarvis_stt_open_parakeet(const char *model_path, bool use_gpu)
{
	if (!model_path || !*model_path)
		return NULL;

	parakeet_log_set(parakeet_quiet_log, NULL);

	struct parakeet_context_params cparams = parakeet_context_default_params();
	cparams.use_gpu = use_gpu;

	struct parakeet_context *pctx = parakeet_init_from_file_with_params(model_path, cparams);
	if (!pctx)
		return NULL;

	jarvis_stt_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		parakeet_free(pctx);
		return NULL;
	}
	ctx->ops = &k_parakeet_ops;
	ctx->impl = pctx;
	snprintf(ctx->model_path, sizeof(ctx->model_path), "%s", model_path);
	return ctx;
}
