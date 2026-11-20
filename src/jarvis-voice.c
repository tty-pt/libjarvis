#include "jarvis-internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define JARVIS_RING_FRAMES  2000 /* 40 seconds circular float32 PCM buffer at 20ms/frame */
#define JARVIS_RING_SAMPLES (JARVIS_RING_FRAMES * JARVIS_FRAME_SAMPLES)

struct jarvis_vad {
	float   pcm[JARVIS_RING_SAMPLES];
	float   db[JARVIS_RING_FRAMES];
	int64_t total_frames;
	double  noise_floor_db;
	double  threshold_db;
	int     env_frames;
	bool    in_speech;
};

jarvis_vad_t *jarvis_vad_create(double initial_noise_floor_db, double threshold_db)
{
	jarvis_vad_t *v = (jarvis_vad_t *)calloc(1, sizeof(jarvis_vad_t));
	if (!v) return NULL;
	v->noise_floor_db = initial_noise_floor_db < -80.0 ? -48.0 : initial_noise_floor_db;
	v->threshold_db   = threshold_db < -80.0 ? -35.0 : threshold_db;
	return v;
}

void jarvis_vad_destroy(jarvis_vad_t *vad)
{
	free(vad);
}

float jarvis_pcm_rms_db(const float *samples, size_t count)
{
	if (!samples || count == 0) return -96.0f;
	double sum_sq = 0.0;
	for (size_t i = 0; i < count; i++) {
		double s = (double)samples[i];
		sum_sq += s * s;
	}
	double rms = sqrt(sum_sq / (double)count);
	if (rms <= 1e-6) return -96.0f;
	double db = 20.0 * log10(rms);
	if (db < -96.0) db = -96.0;
	return (float)db;
}

float jarvis_pcm_s16_to_f32_db(const int16_t *s16, float *out_f32, size_t count)
{
	if (!s16 || !out_f32 || count == 0) return -96.0f;
	double sum_sq = 0.0;
	for (size_t i = 0; i < count; i++) {
		float f = (float)s16[i] / 32768.0f;
		out_f32[i] = f;
		sum_sq += (double)f * (double)f;
	}
	double rms = sqrt(sum_sq / (double)count);
	if (rms <= 1e-6) return -96.0f;
	double db = 20.0 * log10(rms);
	if (db < -96.0) db = -96.0;
	return (float)db;
}

bool jarvis_vad_push_frame(jarvis_vad_t *vad, const float *frame_samples, size_t count)
{
	if (!vad || !frame_samples || count == 0) return false;
	size_t n = count < JARVIS_FRAME_SAMPLES ? count : JARVIS_FRAME_SAMPLES;

	int64_t fidx = vad->total_frames % JARVIS_RING_FRAMES;
	float *dst = &vad->pcm[fidx * JARVIS_FRAME_SAMPLES];
	memcpy(dst, frame_samples, n * sizeof(float));
	if (n < JARVIS_FRAME_SAMPLES) {
		memset(dst + n, 0, (JARVIS_FRAME_SAMPLES - n) * sizeof(float));
	}

	float db = jarvis_pcm_rms_db(dst, JARVIS_FRAME_SAMPLES);
	vad->db[fidx] = db;
	vad->total_frames++;

	/* Adaptive noise floor tracking */
	double dyn_thresh = vad->noise_floor_db + 8.5;
	if (dyn_thresh < vad->threshold_db) dyn_thresh = vad->threshold_db;
	if (dyn_thresh > -18.0) dyn_thresh = -18.0;

	bool active = ((double)db >= dyn_thresh);
	if (!active && !vad->in_speech) {
		double alpha = (vad->env_frames < 50) ? 0.12 : 0.02;
		vad->noise_floor_db = (1.0 - alpha) * vad->noise_floor_db + alpha * (double)db;
		if (vad->noise_floor_db < -65.0) vad->noise_floor_db = -65.0;
		if (vad->noise_floor_db > -25.0) vad->noise_floor_db = -25.0;
		vad->env_frames++;
	}
	vad->in_speech = active;
	return active;
}

size_t jarvis_vad_get_samples(
	const jarvis_vad_t *vad,
	int64_t             start_frame,
	int64_t             end_frame,
	float              *out_pcm,
	size_t              max_samples
)
{
	if (!vad || !out_pcm || max_samples == 0) return 0;
	if (end_frame > vad->total_frames) end_frame = vad->total_frames;
	int64_t min_avail = vad->total_frames - JARVIS_RING_FRAMES;
	if (min_avail < 0) min_avail = 0;
	if (start_frame < min_avail) start_frame = min_avail;
	if (start_frame >= end_frame) return 0;

	size_t written = 0;
	for (int64_t f = start_frame; f < end_frame; f++) {
		if (written + JARVIS_FRAME_SAMPLES > max_samples) break;
		int64_t r_idx = f % JARVIS_RING_FRAMES;
		memcpy(out_pcm + written, &vad->pcm[r_idx * JARVIS_FRAME_SAMPLES], JARVIS_FRAME_SAMPLES * sizeof(float));
		written += JARVIS_FRAME_SAMPLES;
	}
	return written;
}

static const jarvis_ontology_t *jarvis_voice_default_ontology(void)
{
	return jarvis_ontology_get_active();
}

static const char *match_folded_wake_span(
	const jarvis_ontology_t *ont,
	const char              *transcript,
	const char              *wake_clean,
	bool                     allow_anywhere,
	bool                     allow_fuzzy
)
{
	if (!transcript || !wake_clean || !*wake_clean) return NULL;
	size_t n_words = 0;
	for (size_t i = 0; wake_clean[i]; i++) {
		if (i == 0 || (wake_clean[i] != ' ' && wake_clean[i - 1] == ' ')) n_words++;
	}
	if (n_words == 0) return NULL;

	const char *p = transcript;
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || strchr(",.!?;:-", *p))) p++;
		if (!*p) break;

		/* If prefix-only mode, strip any leading polite prefixes from ontology before testing */
		if (!allow_anywhere && ont) {
			bool stripped_polite = true;
			while (stripped_polite) {
				stripped_polite = false;
				for (size_t pi = 0; pi < ont->polite_prefix_count; pi++) {
					const char *pp = ont->polite_prefixes[pi];
					if (!pp[0]) continue;
					size_t pw = 0;
					for (size_t k = 0; pp[k]; k++) {
						if (k == 0 || (pp[k] != ' ' && pp[k - 1] == ' ')) pw++;
					}
					const char *cur_p = p;
					for (size_t w = 0; w < pw && *cur_p; w++) {
						if (w > 0) {
							while (*cur_p && (isspace((unsigned char)*cur_p) || strchr(",.!?;:-", *cur_p))) cur_p++;
						}
						while (*cur_p && !isspace((unsigned char)*cur_p) && !strchr(",.!?;:-", *cur_p)) cur_p++;
					}
					size_t plen = (size_t)(cur_p - p);
					if (plen > 0 && plen < JARVIS_ID_LEN) {
						char raw_p[JARVIS_ID_LEN], clean_p[JARVIS_ID_LEN];
						memcpy(raw_p, p, plen);
						raw_p[plen] = '\0';
						jarvis_clean_phrase(raw_p, clean_p, sizeof(clean_p));
						if (strcmp(clean_p, pp) == 0 && strcmp(clean_p, wake_clean) != 0) {
							p = cur_p;
							while (*p && (isspace((unsigned char)*p) || strchr(",.!?;:-", *p))) p++;
							stripped_polite = true;
							break;
						}
					}
				}
			}
		}

		const char *span_start = p;
		const char *cur = p;
		for (size_t w = 0; w < n_words && *cur; w++) {
			if (w > 0) {
				while (*cur && (isspace((unsigned char)*cur) || strchr(",.!?;:-", *cur))) cur++;
			}
			while (*cur && !isspace((unsigned char)*cur) && !strchr(",.!?;:-", *cur)) cur++;
		}
		size_t slen = (size_t)(cur - span_start);
		if (slen > 0 && slen < JARVIS_ID_LEN) {
			char raw_span[JARVIS_ID_LEN], clean_span[JARVIS_ID_LEN];
			memcpy(raw_span, span_start, slen);
			raw_span[slen] = '\0';
			jarvis_clean_phrase(raw_span, clean_span, sizeof(clean_span));
			if (strcmp(clean_span, wake_clean) == 0 ||
			    (allow_fuzzy && n_words == 1 && strlen(wake_clean) >= 5 && strlen(clean_span) >= 5 &&
			     jarvis_token_similarity(clean_span, wake_clean) >= 0.80)) {
				return cur;
			}
		}
		if (!allow_anywhere) break;
		/* Advance one word */
		while (*p && !isspace((unsigned char)*p) && !strchr(",.!?;:-", *p)) p++;
	}
	return NULL;
}

int jarvis_voice_match_wake(
	const jarvis_ontology_t *ont,
	const char              *text,
	const char              *custom_wake_word,
	bool                     allow_anywhere,
	char                    *out_cmd,
	size_t                   out_sz
)
{
	if (out_cmd && out_sz > 0) out_cmd[0] = '\0';
	if (!text || !*text) return 0;
	if (!ont) ont = jarvis_voice_default_ontology();

	const char *ww = (custom_wake_word && *custom_wake_word) ? custom_wake_word : (ont ? ont->wake_word : "");
	char ww_clean[JARVIS_ID_LEN];
	jarvis_clean_phrase(ww, ww_clean, sizeof(ww_clean));
	if (!ww_clean[0] && ont && ont->wake_word[0]) {
		jarvis_clean_phrase(ont->wake_word, ww_clean, sizeof(ww_clean));
	}
	if (!ww_clean[0]) return 0;

	const char *matched_end = match_folded_wake_span(ont, text, ww_clean, allow_anywhere, true);

	char ont_ww_clean[JARVIS_ID_LEN] = {0};
	if (ont && ont->wake_word[0]) {
		jarvis_clean_phrase(ont->wake_word, ont_ww_clean, sizeof(ont_ww_clean));
	}
	bool use_ont_variants = ont && (!custom_wake_word || !*custom_wake_word ||
	                                (ont_ww_clean[0] && strcmp(ww_clean, ont_ww_clean) == 0));

	if (!matched_end && use_ont_variants) {
		for (size_t i = 0; i < ont->wake_variant_count; i++) {
			char var_clean[JARVIS_ID_LEN];
			jarvis_clean_phrase(ont->wake_variants[i], var_clean, sizeof(var_clean));
			if (!var_clean[0]) continue;
			bool var_anywhere = allow_anywhere && (strncmp(var_clean, ww_clean, 3) == 0);
			matched_end = match_folded_wake_span(ont, text, var_clean, var_anywhere, false);
			if (matched_end) break;
		}
	}

	if (!matched_end) return 0;

	while (*matched_end && (isspace((unsigned char)*matched_end) || strchr(",.!?;:-", *matched_end))) {
		matched_end++;
	}

	if (!*matched_end) return 1;

	if (out_cmd && out_sz > 0) {
		snprintf(out_cmd, out_sz, "%s", matched_end);
		size_t clen = strlen(out_cmd);
		while (clen > 0 && (isspace((unsigned char)out_cmd[clen - 1]) || strchr(".!?;,", out_cmd[clen - 1]))) {
			out_cmd[--clen] = '\0';
		}
		if (!out_cmd[0]) return 1;
	}
	return 2;
}

bool jarvis_voice_extract_wake_command(
	const char *transcript,
	const char *wake_word,
	char       *out_command,
	size_t      out_sz
)
{
	return jarvis_voice_match_wake(NULL, transcript, wake_word, true, out_command, out_sz) != 0;
}

static const char *lookup_dsl_reply_var(const jarvis_process_result_t *res, const char *var, char *num_buf, size_t num_sz)
{
	if (!res || !var) return "";
	const jarvis_dsl_t *d = &res->last_dsl;
	if (strcmp(var, "output") == 0) return res->combined_result.output;
	if (strcmp(var, "error") == 0) return res->error[0] ? res->error : res->combined_result.error;
	if (strcmp(var, "op") == 0) return d->op;
	if (strcmp(var, "target.identifier") == 0) return d->target.identifier;
	if (strcmp(var, "target.command") == 0) return d->target.command;
	if (strcmp(var, "target.kind") == 0) return d->target.kind;
	if (strcmp(var, "target.relation") == 0) return d->target.relation;
	if (strcmp(var, "target.url") == 0) return d->target.url;
	if (strcmp(var, "target.query") == 0) return d->target.query;
	if (strcmp(var, "args.answer") == 0) return d->args.answer[0] ? d->args.answer : res->combined_result.output;
	if (strcmp(var, "args.text") == 0) return d->args.text;
	if (strcmp(var, "args.action") == 0) return d->args.action;
	if (strcmp(var, "args.direction") == 0) return d->args.direction;
	if (strcmp(var, "args.mode") == 0) return d->args.mode;
	if (strcmp(var, "args.key_combo") == 0) return d->args.key_combo;
	if (strcmp(var, "args.workspace") == 0) {
		snprintf(num_buf, num_sz, "%d", d->args.workspace);
		return num_buf;
	}
	if (strcmp(var, "args.amount") == 0) {
		snprintf(num_buf, num_sz, "%d", d->args.amount);
		return num_buf;
	}
	return "";
}

void jarvis_format_voice_reply(
	const jarvis_ontology_t       *ont,
	const jarvis_process_result_t *res,
	char                          *out,
	size_t                         out_sz
)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!res) return;
	if (!ont) ont = jarvis_voice_default_ontology();

	const char *op = res->last_dsl.op;
	if (strcmp(op, "tts_interrupt") == 0) return;

	const char *tpl = NULL;
	if (!res->combined_result.success) {
		tpl = jarvis_ontology_get_reply_template(ont, "error");
	} else {
		if (strcmp(op, "media_play") == 0 && !res->last_dsl.target.query[0]) {
			tpl = jarvis_ontology_get_reply_template(ont, "media_play_empty");
		}
		if (!tpl && strcmp(op, "conversation_control") == 0 && res->last_dsl.args.action[0]) {
			char subkey[128];
			snprintf(subkey, sizeof(subkey), "conversation_control_%s", res->last_dsl.args.action);
			tpl = jarvis_ontology_get_reply_template(ont, subkey);
		}
		if (!tpl && op[0]) {
			tpl = jarvis_ontology_get_reply_template(ont, op);
		}
		if (!tpl) {
			tpl = jarvis_ontology_get_reply_template(ont, "default");
		}
	}

	if (!tpl) {
		if (strcmp(op, "answer_question") == 0 || strcmp(op, "tts_control") == 0 || strcmp(op, "wm_help") == 0) {
			snprintf(out, out_sz, "%s", res->combined_result.output);
		}
		return;
	}
	if (!tpl[0]) return;

	size_t w = 0;
	for (size_t i = 0; tpl[i] && w + 1 < out_sz; ) {
		if (tpl[i] == '$' && tpl[i + 1] == '{') {
			const char *end = strchr(tpl + i + 2, '}');
			if (end) {
				char vname[64] = {0}, num_buf[32] = {0};
				size_t vlen = (size_t)(end - (tpl + i + 2));
				if (vlen >= sizeof(vname)) vlen = sizeof(vname) - 1;
				memcpy(vname, tpl + i + 2, vlen);
				vname[vlen] = '\0';
				const char *val = lookup_dsl_reply_var(res, vname, num_buf, sizeof(num_buf));
				w += (size_t)snprintf(out + w, out_sz - w, "%s", val ? val : "");
				i = (size_t)(end - tpl) + 1;
				continue;
			}
		}
		out[w++] = tpl[i++];
	}
	out[w] = '\0';
}

bool jarvis_voice_is_lockdown_phrase(const char *phrase)
{
	if (!phrase) return false;
	char clean[JARVIS_STR_LEN];
	jarvis_clean_phrase(phrase, clean, sizeof(clean));
	if (!clean[0]) return false;

	const jarvis_ontology_t *ont = jarvis_voice_default_ontology();
	if (!ont) return false;

	/* Also normalize ASR polite tails and wake prefixes before matching */
	char norm[JARVIS_STR_LEN], stripped[JARVIS_STR_LEN];
	jarvis_ontology_normalize_asr(ont, NULL, clean, norm, sizeof(norm));
	stripped[0] = '\0';
	if (jarvis_voice_match_wake(ont, norm[0] ? norm : clean, NULL, false, stripped, sizeof(stripped)) != 2) {
		stripped[0] = '\0';
	}
	const char *cands[3] = { clean, norm[0] ? norm : clean, stripped[0] ? stripped : clean };

	for (int c = 0; c < 3; c++) {
		const char *p = cands[c];
		for (size_t i = 0; i < ont->lockdown_phrase_count; i++) {
			const char *lp = ont->lockdown_phrases[i];
			if (!lp[0]) continue;
			if (strcmp(p, lp) == 0) return true;
			if (strlen(p) >= 6 && strlen(lp) >= 6 &&
			    jarvis_token_similarity(p, lp) >= 0.86) {
				return true;
			}
		}
	}
	return false;
}

bool jarvis_voice_write_earcon_wav(const char *wav_path, bool is_done)
{
#ifdef __wasm__
	(void)wav_path; (void)is_done;
	return false;
#else
	if (!wav_path || !*wav_path) return false;
	FILE *fp = fopen(wav_path, "wb");
	if (!fp) return false;

	const double freqs[2] = {
		is_done ? 659.25 : 587.33,
		is_done ? 523.25 : 880.00
	};
	const double durs[2] = {
		is_done ? 0.032 : 0.040,
		is_done ? 0.060 : 0.045
	};
	const double amp = is_done ? 5200.0 : 7200.0;

	int n0 = (int)(JARVIS_SAMPLE_RATE * durs[0]);
	int n1 = (int)(JARVIS_SAMPLE_RATE * durs[1]);
	uint32_t total_samples = (uint32_t)(n0 + n1);
	uint32_t data_bytes = total_samples * 2;
	uint32_t riff_size = 36 + data_bytes;

	uint8_t hdr[44];
	memcpy(hdr + 0, "RIFF", 4);
	hdr[4] = (uint8_t)(riff_size & 0xFF);
	hdr[5] = (uint8_t)((riff_size >> 8) & 0xFF);
	hdr[6] = (uint8_t)((riff_size >> 16) & 0xFF);
	hdr[7] = (uint8_t)((riff_size >> 24) & 0xFF);
	memcpy(hdr + 8, "WAVEfmt ", 8);
	hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0; /* PCM chunk size */
	hdr[20] = 1;  hdr[21] = 0;                           /* AudioFormat = 1 (PCM) */
	hdr[22] = 1;  hdr[23] = 0;                           /* NumChannels = 1 */
	uint32_t sr = JARVIS_SAMPLE_RATE;
	hdr[24] = (uint8_t)(sr & 0xFF);
	hdr[25] = (uint8_t)((sr >> 8) & 0xFF);
	hdr[26] = (uint8_t)((sr >> 16) & 0xFF);
	hdr[27] = (uint8_t)((sr >> 24) & 0xFF);
	uint32_t br = sr * 2;
	hdr[28] = (uint8_t)(br & 0xFF);
	hdr[29] = (uint8_t)((br >> 8) & 0xFF);
	hdr[30] = (uint8_t)((br >> 16) & 0xFF);
	hdr[31] = (uint8_t)((br >> 24) & 0xFF);
	hdr[32] = 2;  hdr[33] = 0;                           /* BlockAlign = 2 */
	hdr[34] = 16; hdr[35] = 0;                           /* BitsPerSample = 16 */
	memcpy(hdr + 36, "data", 4);
	hdr[40] = (uint8_t)(data_bytes & 0xFF);
	hdr[41] = (uint8_t)((data_bytes >> 8) & 0xFF);
	hdr[42] = (uint8_t)((data_bytes >> 16) & 0xFF);
	hdr[43] = (uint8_t)((data_bytes >> 24) & 0xFF);

	fwrite(hdr, 1, sizeof(hdr), fp);

	int counts[2] = {n0, n1};
	for (int note = 0; note < 2; note++) {
		int n = counts[note];
		for (int i = 0; i < n; i++) {
			double env = sin(M_PI * (double)i / (double)n);
			double s = amp * env * sin(2.0 * M_PI * freqs[note] * (double)i / (double)JARVIS_SAMPLE_RATE);
			int16_t val = (int16_t)s;
			uint8_t le[2] = {(uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF)};
			fwrite(le, 1, 2, fp);
		}
	}

	fclose(fp);
	return true;
#endif
}

void jarvis_tts_clean_text(const char *raw_text, char *out_clean, size_t out_sz)
{
	if (!out_clean || out_sz == 0) return;
	out_clean[0] = '\0';
	if (!raw_text) return;

	size_t w = 0;
	bool prev_space = true;
	const char *p = raw_text;

	while (*p && w + 1 < out_sz) {
		if (strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0) {
			/* Strip scheme and keep only the bare domain for language-neutral TTS reading */
			p += (p[4] == 's') ? 8 : 7;
			if (!prev_space && w + 1 < out_sz) {
				out_clean[w++] = ' ';
				prev_space = true;
			}
			while (*p && !isspace((unsigned char)*p) && *p != '/' && *p != '?' && *p != '#' && w + 1 < out_sz) {
				out_clean[w++] = *p++;
				prev_space = false;
			}
			while (*p && !isspace((unsigned char)*p)) p++;
			continue;
		}
		char c = *p++;
		if (strchr("`*_#[]<>", c)) continue;
		if (isspace((unsigned char)c)) {
			if (!prev_space && w + 1 < out_sz) {
				out_clean[w++] = ' ';
				prev_space = true;
			}
		} else {
			out_clean[w++] = c;
			prev_space = false;
		}
	}
	if (w > 0 && out_clean[w - 1] == ' ') w--;
	out_clean[w] = '\0';
}

size_t jarvis_tts_split_segments(
	const char *text,
	char        out_segments[][JARVIS_STR_LEN],
	size_t      max_segments
)
{
	if (!text || !out_segments || max_segments == 0) return 0;
	char clean[JARVIS_TEXT_LEN];
	jarvis_tts_clean_text(text, clean, sizeof(clean));
	if (!clean[0]) return 0;

	size_t count = 0;
	char rem_buf[JARVIS_TEXT_LEN];
	snprintf(rem_buf, sizeof(rem_buf), "%s", clean);

	while (rem_buf[0] && count < max_segments) {
		char clause[JARVIS_STR_LEN], next_rem[JARVIS_TEXT_LEN];
		if (jarvis_pop_ready_tts_clause(rem_buf, count == 0, clause, sizeof(clause), next_rem, sizeof(next_rem))) {
			char *slot = out_segments[count++];
			snprintf(slot, sizeof(out_segments[0]), "%s", clause);
			snprintf(rem_buf, sizeof(rem_buf), "%s", next_rem);
		} else {
			char *slot = out_segments[count++];
			snprintf(slot, sizeof(out_segments[0]), "%.*s",
			         (int)(sizeof(out_segments[0]) - 1), rem_buf);
			break;
		}
	}
	return count;
}
