#define _GNU_SOURCE
#include "jarvis-stt.h"
#include "jarvis-platform.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

jarvis_audio_ring_t g_audio = {
	.mtx            = PTHREAD_MUTEX_INITIALIZER,
	.cond           = PTHREAD_COND_INITIALIZER,
	.total_frames   = 0,
	.noise_floor_db = -42.0,
	.ambient_std_db = 1.5,
	.env_frames     = 0,
	.active_users   = 0,
	.hold_until_ms  = 0,
	.in_speech_hint = 0,
	.pw_open        = 0
};

jarvis_decode_worker_t g_dec = {
	.mtx               = PTHREAD_MUTEX_INITIALIZER,
	.cond              = PTHREAD_COND_INITIALIZER,
	.ctx               = NULL,
	.n_threads         = 4,
	.pcm               = NULL,
	.n_samples         = 0,
	.prompt            = {0},
	.job_end_f         = 0,
	.has_job           = 0,
	.busy              = 0,
	.abort_flag        = 0,
	.has_result        = 0,
	.result_voiced_f   = -1,
	.result_end_f      = -1,
	.result_all_silent = 0,
	.done_at_frame     = 0
};

int
jarvis_stt_ring_extract_pcm(int64_t start_f, int64_t end_f, float *out_pcm, int max_samples)
{
	int n_samples = 0;

	pthread_mutex_lock(&g_audio.mtx);
	if (end_f > g_audio.total_frames)
		end_f = g_audio.total_frames;
	if (end_f - start_f > JARVIS_RING_FRAMES - 4)
		start_f = end_f - (JARVIS_RING_FRAMES - 4);
	if (start_f < 0)
		start_f = 0;

	for (int64_t f = start_f; f < end_f; f++) {
		if (n_samples + JARVIS_FRAME_SAMPLES > max_samples)
			break;
		int slot = (int)(f % JARVIS_RING_FRAMES);
		memcpy(out_pcm + n_samples, g_audio.pcm + (size_t)slot * JARVIS_FRAME_SAMPLES,
		       (size_t)JARVIS_FRAME_SAMPLES * sizeof(float));
		n_samples += JARVIS_FRAME_SAMPLES;
	}
	pthread_mutex_unlock(&g_audio.mtx);
	return n_samples;
}

int
jarvis_stt_load_wav_f32(const char *path, float **out_pcm, int *out_samples)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long fsize = ftell(fp);
	if (fsize <= 44 || fsize > 64 * 1024 * 1024) {
		fclose(fp);
		return -1;
	}
	if (fseek(fp, 44, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	size_t pcm_bytes = (size_t)(fsize - 44);
	int n_samples = (int)(pcm_bytes / sizeof(int16_t));
	int16_t *raw = malloc((size_t)n_samples * sizeof(int16_t));
	float *f32 = malloc((size_t)n_samples * sizeof(float));
	if (!raw || !f32) {
		free(raw);
		free(f32);
		fclose(fp);
		return -1;
	}

	size_t n_read = fread(raw, sizeof(int16_t), (size_t)n_samples, fp);
	fclose(fp);
	n_samples = (int)n_read;

	for (int i = 0; i < n_samples; i++)
		f32[i] = (float)raw[i] / 32768.0f;

	free(raw);
	*out_pcm = f32;
	*out_samples = n_samples;
	return 0;
}

int
jarvis_stt_compute_audio_ctx_ex(int n_samples, int is_wake, int is_partial, int final_min_ctx)
{
	if (!is_wake && !is_partial && final_min_ctx == 0)
		return 0; /* Full native Whisper encoder window (1500 frames) */

	int frames = (n_samples / 320) + (is_wake ? 24 : (is_partial ? 24 : 32));
	int aligned = ((frames + 31) / 32) * 32;
	int min_ctx;
	if (is_wake) {
		min_ctx = 128;
	} else if (is_partial) {
		min_ctx = 256;
	} else {
		min_ctx = (final_min_ctx > 0) ? final_min_ctx : 512;
		if (min_ctx > 1500)
			min_ctx = 1500;
	}
	if (aligned < min_ctx)
		aligned = min_ctx;
	if (aligned >= 1500)
		return (!is_wake && !is_partial) ? 0 : 1500;
	return aligned;
}

int
jarvis_stt_compute_audio_ctx(int n_samples, int is_wake, int is_partial)
{
	return jarvis_stt_compute_audio_ctx_ex(n_samples, is_wake, is_partial, 512);
}

void
jarvis_stt_apply_hpf_inplace(float *pcm, int n_samples)
{
	if (!pcm || n_samples <= 0)
		return;
	const float alpha = 0.9695f; /* ~80 Hz 1st-order DC/rumble blocker @ 16 kHz */
	float prev_x = pcm[0];
	float prev_y = 0.0f;
	for (int i = 0; i < n_samples; i++) {
		float x = pcm[i];
		float y = alpha * (prev_y + x - prev_x);
		prev_x = x;
		prev_y = y;
		pcm[i] = y;
	}
}

/* C1-continuous soft-knee limiter above 0.85f asymptotically bounded at +-0.98f. */
static inline float
stt_soft_limit(float v)
{
	const float knee = 0.85f;
	const float ceiling = 0.98f;
	const float range = ceiling - knee;
	float av = fabsf(v);
	if (av <= knee)
		return v;
	float limited = knee + range * tanhf((av - knee) / range);
	return (v < 0.0f) ? -limited : limited;
}

float *
jarvis_stt_normalize_pcm(const float *pcm, int n_samples, int apply_hpf)
{
	if (!pcm || n_samples <= 0)
		return NULL;
	float *buf = malloc((size_t)n_samples * sizeof(float));
	if (!buf)
		return NULL;

	float peak = 0.0f;
	double sum_sq = 0.0;

	if (apply_hpf) {
		const float alpha = 0.9695f;
		float prev_x = pcm[0];
		float prev_y = 0.0f;
		for (int i = 0; i < n_samples; i++) {
			float x = pcm[i];
			float y = alpha * (prev_y + x - prev_x);
			prev_x = x;
			prev_y = y;
			buf[i] = y;
			float ay = fabsf(y);
			if (ay > peak)
				peak = ay;
			sum_sq += (double)y * (double)y;
		}
	} else {
		for (int i = 0; i < n_samples; i++) {
			float y = pcm[i];
			buf[i] = y;
			float ay = fabsf(y);
			if (ay > peak)
				peak = ay;
			sum_sq += (double)y * (double)y;
		}
	}

	double total_rms = sqrt(sum_sq / (double)n_samples);

	/* O(N) per-20ms-frame RMS stats + 128-bin log-dB histogram (-80..0 dBFS,
	 * 0.625 dB/bin, zero heap allocations) for quiet-floor estimation and
	 * active-speech RMS. Never downward-expands quiet speech onsets.
	 */
	const int flen = 320;
	int n_frames = n_samples / flen;
	double active_sq = 0.0;
	int active_n = 0;
	float frame_thresh = peak * 0.08f;
	if (frame_thresh < 0.008f)
		frame_thresh = 0.008f;

	int hist[128] = {0};
	float peak_frame_rms = 0.0f;

	for (int f = 0; f < n_frames; f++) {
		int base = f * flen;
		double f_sq = 0.0;
		for (int k = 0; k < flen; k++) {
			float v = buf[base + k];
			f_sq += (double)v * (double)v;
		}
		float f_rms = (float)sqrt(f_sq / (double)flen);
		if (f_rms > peak_frame_rms)
			peak_frame_rms = f_rms;
		if (f_rms >= frame_thresh) {
			active_sq += f_sq;
			active_n += flen;
		}
		float db = 20.0f * log10f(f_rms + 1e-7f);
		int bin = (int)((db + 80.0f) * 1.6f);
		if (bin < 0) bin = 0;
		if (bin > 127) bin = 127;
		hist[bin]++;
	}

	/* 20th-percentile quiet-frame floor in O(1) (128 histogram bins) */
	float floor_frame_rms = 0.0f;
	if (n_frames >= 4) {
		int target = (n_frames * 20) / 100;
		int acc = 0;
		int b20 = 0;
		for (int b = 0; b < 128; b++) {
			acc += hist[b];
			if (acc > target) {
				b20 = b;
				break;
			}
		}
		float db20 = ((float)b20 / 1.6f) - 80.0f;
		floor_frame_rms = powf(10.0f, db20 / 20.0f);
	}

	double rms = (active_n >= 1600) ? sqrt(active_sq / (double)active_n) : total_rms;

	/* Dynamic-range-aware AGC cap:
	 * Avoid boosting flat steady background noise by +14 dB into hallucinations;
	 * modulated speech (even quiet mics) still receives full up-to-5.0x gain.
	 */
	float max_gain = 5.0f;
	if (n_frames >= 6 && floor_frame_rms > 1e-4f &&
	    peak_frame_rms < floor_frame_rms * 1.6f && peak < 0.08f) {
		max_gain = 1.5f;
	}

	if (peak > 0.010f && rms > 0.0020) {
		float gain_peak = 0.78f / peak;
		float gain_rms = (float)(0.11 / rms);
		float gain = (gain_rms < gain_peak) ? gain_rms : gain_peak;
		if (gain > max_gain)
			gain = max_gain;
		if (gain < 0.85f)
			gain = 0.85f;
		if (fabsf(gain - 1.0f) > 0.05f || peak > 0.85f) {
			for (int i = 0; i < n_samples; i++)
				buf[i] = stt_soft_limit(buf[i] * gain);
		}
	} else if (peak > 0.85f) {
		for (int i = 0; i < n_samples; i++)
			buf[i] = stt_soft_limit(buf[i]);
	}
	return buf;
}

float *
jarvis_stt_apply_preemphasis_and_agc(const float *pcm, int n_samples)
{
	return jarvis_stt_normalize_pcm(pcm, n_samples, 1);
}

static size_t
stt_word_norm_len(const char *w)
{
	size_t l = strlen(w);
	while (l > 0 && ispunct((unsigned char)w[l - 1]))
		l--;
	return l;
}

static uint32_t
stt_word_hash_ci(const char *w, size_t len)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)tolower((unsigned char)w[i]);
		h ^= c;
		h *= 16777619u;
	}
	return h;
}

/* Collapse immediate repeated word sequences (1..24 words) in-place using
 * precomputed normalized word hashes so long Whisper repetition loops collapse
 * cleanly in linear time without quadratic string scans.
 */
void
jarvis_stt_collapse_repetitions(char *s, size_t s_sz)
{
	char copy[4096];
	char *words[1024];
	size_t norm_len[1024];
	uint32_t hashes[1024];
	int n_words = 0;

	if (!s || !*s || s_sz == 0)
		return;
	if (snprintf(copy, sizeof(copy), "%s", s) >= (int)sizeof(copy))
		return;

	char *p = copy;
	while (*p) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (n_words >= 1024)
			return;
		char *tok = p;
		while (*p && *p != ' ')
			p++;
		if (*p == ' ')
			*p++ = '\0';
		size_t nl = stt_word_norm_len(tok);
		words[n_words] = tok;
		norm_len[n_words] = nl;
		hashes[n_words] = stt_word_hash_ci(tok, nl);
		n_words++;
	}
	if (n_words < 2)
		return;

	int max_span = n_words / 2;
	if (max_span > 24)
		max_span = 24;

	for (int span = max_span; span >= 1; span--) {
		for (int i = 0; i + 2 * span <= n_words; ) {
			int match = 1;
			for (int k = 0; k < span; k++) {
				int ia = i + k;
				int ib = i + span + k;
				if (hashes[ia] != hashes[ib] ||
				    norm_len[ia] == 0 ||
				    norm_len[ia] != norm_len[ib] ||
				    strncasecmp(words[ia], words[ib], norm_len[ia]) != 0) {
					match = 0;
					break;
				}
			}
			if (match && (span >= 2 || strlen(words[i]) >= 4)) {
				int tail = n_words - (i + 2 * span);
				if (tail > 0) {
					memmove(&words[i + span], &words[i + 2 * span], (size_t)tail * sizeof(words[0]));
					memmove(&norm_len[i + span], &norm_len[i + 2 * span], (size_t)tail * sizeof(norm_len[0]));
					memmove(&hashes[i + span], &hashes[i + 2 * span], (size_t)tail * sizeof(hashes[0]));
				}
				n_words -= span;
			} else {
				i++;
			}
		}
	}

	/* Strip trailing truncated prefix repetition (e.g. "open up miles davis open up miles"
	 * or "Open T-MUX in this terminal. Open T").
	 */
	int max_tail_k = (n_words - 1) / 2;
	if (max_tail_k > 12)
		max_tail_k = 12;
	for (int k = max_tail_k; k >= 1; k--) {
		if (n_words >= 2 * k + 1) {
			int tail_match = 1;
			for (int j = 0; j < k; j++) {
				int ia = j;
				int ib = n_words - k + j;
				size_t la = norm_len[ia];
				size_t lb = norm_len[ib];
				if (la == 0 || lb == 0) {
					tail_match = 0;
					break;
				}
				if (j == k - 1 && k >= 2 && lb < la) {
					if (strncasecmp(words[ia], words[ib], lb) != 0) {
						tail_match = 0;
						break;
					}
				} else if (hashes[ia] != hashes[ib] || la != lb ||
				           strncasecmp(words[ia], words[ib], la) != 0) {
					tail_match = 0;
					break;
				}
			}
			if (tail_match) {
				/* For k == 1, only strip if preceded by sentence punctuation */
				if (k == 1) {
					const char *prev_w = words[n_words - 2];
					size_t pl = strlen(prev_w);
					if (pl == 0 || (prev_w[pl - 1] != '.' && prev_w[pl - 1] != '!' && prev_w[pl - 1] != '?'))
						continue;
				}
				n_words -= k;
				break;
			}
		}
	}

	size_t off = 0;
	s[0] = '\0';
	for (int i = 0; i < n_words; i++) {
		int n = snprintf(s + off, s_sz - off, "%s%s", (i > 0) ? " " : "", words[i]);
		if (n < 0 || (size_t)n >= s_sz - off) {
			s[s_sz - 1] = '\0';
			break;
		}
		off += (size_t)n;
	}
}
