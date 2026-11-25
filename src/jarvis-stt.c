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
	.prompt            = NULL,
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
jarvis_stt_compute_audio_ctx(int n_samples, int is_wake, int is_partial)
{
	int frames = (n_samples / 320) + (is_wake ? 24 : (is_partial ? 24 : 32));
	int aligned = ((frames + 31) / 32) * 32;
	int min_ctx = is_wake ? 128 : (is_partial ? 192 : 256);
	if (aligned < min_ctx)
		aligned = min_ctx;
	if (aligned > 1500)
		aligned = 1500;
	return aligned;
}

float *
jarvis_stt_apply_preemphasis_and_agc(const float *pcm, int n_samples)
{
	if (!pcm || n_samples <= 0)
		return NULL;
	float *buf = malloc((size_t)n_samples * sizeof(float));
	if (!buf)
		return NULL;

	const float alpha = 0.9695f;
	float prev_x = pcm[0];
	float prev_y = 0.0f;
	float peak = 0.0f;
	double sum_sq = 0.0;

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

	double total_rms = sqrt(sum_sq / (double)n_samples);
	/* Compute per-20ms-frame RMS envelope for self-referenced downward expansion
	 * and active speech RMS measurement (works identically on any mic/gain).
	 */
	const int flen = 320;
	int n_frames = n_samples / flen;
	double active_sq = 0.0;
	int active_n = 0;
	float frame_thresh = peak * 0.08f;
	if (frame_thresh < 0.008f)
		frame_thresh = 0.008f;

	float *f_rms_arr = (n_frames >= 4) ? malloc((size_t)n_frames * sizeof(float)) : NULL;
	float peak_frame_rms = 0.0f;

	for (int f = 0; f < n_frames; f++) {
		int base = f * flen;
		double f_sq = 0.0;
		for (int k = 0; k < flen; k++) {
			float v = buf[base + k];
			f_sq += (double)v * (double)v;
		}
		float f_rms = (float)sqrt(f_sq / (double)flen);
		if (f_rms_arr)
			f_rms_arr[f] = f_rms;
		if (f_rms > peak_frame_rms)
			peak_frame_rms = f_rms;
		if (f_rms >= frame_thresh) {
			active_sq += f_sq;
			active_n += flen;
		}
	}

	/* Estimate quiet-frame floor (20th percentile of 20ms frame RMS) */
	float floor_frame_rms = 0.0f;
	if (f_rms_arr && n_frames >= 4) {
		float *sorted = malloc((size_t)n_frames * sizeof(float));
		if (sorted) {
			memcpy(sorted, f_rms_arr, (size_t)n_frames * sizeof(float));
			for (int i = 1; i < n_frames; i++) {
				float key = sorted[i];
				int j = i - 1;
				while (j >= 0 && sorted[j] > key) {
					sorted[j + 1] = sorted[j];
					j--;
				}
				sorted[j + 1] = key;
			}
			floor_frame_rms = sorted[(n_frames * 20) / 100];
			free(sorted);
		}
	}

	/* Soft downward expander (relative to utterance peak):
	 * When near-field speech stands out clearly over inter-word background
	 * (peak_frame_rms >= 2.2x floor_frame_rms), smoothly attenuate quiet
	 * inter-word frames (>14 dB below speech peak) so background talkers
	 * between the user's words are not boosted into Whisper.
	 */
	if (f_rms_arr && n_frames >= 6 && peak_frame_rms > 0.012f &&
	    peak_frame_rms > floor_frame_rms * 2.2f) {
		float knee_rms = peak_frame_rms * 0.20f; /* -14 dB below speech peak */
		float prev_g = 1.0f;
		for (int f = 0; f < n_frames; f++) {
			/* 1-frame lookahead prevents clipping plosive/consonant onsets */
			float env = f_rms_arr[f];
			if (f + 1 < n_frames && f_rms_arr[f + 1] > env)
				env = f_rms_arr[f + 1];
			float target_g = 1.0f;
			if (env < knee_rms && knee_rms > 1e-6f) {
				float r = env / knee_rms;
				target_g = 0.15f + 0.85f * r * sqrtf(r);
			}
			/* Fast attack (open immediately), smooth release (~55ms) */
			float cur_g = (target_g > prev_g)
			            ? (0.25f * prev_g + 0.75f * target_g)
			            : (0.65f * prev_g + 0.35f * target_g);
			int base = f * flen;
			float step = (cur_g - prev_g) / (float)flen;
			for (int k = 0; k < flen; k++)
				buf[base + k] *= (prev_g + step * (float)k);
			prev_g = cur_g;
		}
		for (int i = n_frames * flen; i < n_samples; i++)
			buf[i] *= prev_g;
	}
	free(f_rms_arr);

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
		if (fabsf(gain - 1.0f) > 0.05f) {
			for (int i = 0; i < n_samples; i++) {
				float v = buf[i] * gain;
				if (v > 0.98f)
					v = 0.98f;
				else if (v < -0.98f)
					v = -0.98f;
				buf[i] = v;
			}
		}
	}
	return buf;
}
