/* Standalone unit test suite for jarvis-dsp.c (adaptive stereo coherence + HPF)
 * and jarvis-stt.c (soft downward expander + dynamic-range-aware AGC).
 */
#include "jarvis-dsp.h"
#include "jarvis-stt.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint32_t rng_state = 0x12345678u;

static float
rand_uniform(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return ((float)(rng_state >> 8) / (float)(1u << 24)) * 2.0f - 1.0f;
}

static int16_t
f2s16(float x)
{
	if (x > 1.0f) x = 1.0f;
	if (x < -1.0f) x = -1.0f;
	return (int16_t)lrintf(x * 32767.0f);
}

static double
calc_rms(const float *x, size_t n)
{
	double s = 0.0;
	for (size_t i = 0; i < n; i++)
		s += (double)x[i] * (double)x[i];
	return sqrt(s / (double)n);
}

static void
test_mono_cola_and_hpf(void)
{
	jarvis_dsp_t dsp;
	jarvis_dsp_init(&dsp, 16000, 1);

	/* 1. Mono speech-band tone (1000 Hz) + DC offset + 50 Hz rumble */
	const int n_frames = 30; /* 600ms */
	const int flen = 320;
	int16_t in[320];
	float out[320];
	double speech_out_sq = 0.0;
	int speech_cnt = 0;

	for (int f = 0; f < n_frames; f++) {
		for (int i = 0; i < flen; i++) {
			int t = f * flen + i;
			float sig = 0.30f * sinf(2.0f * (float)M_PI * 1000.0f * (float)t / 16000.0f);
			in[i] = f2s16(sig);
		}
		float ns = 0.0f;
		jarvis_dsp_process_frame(&dsp, in, (size_t)flen, out, &ns);
		assert(ns == 1.0f);
		assert(jarvis_dsp_is_stereo_active(&dsp) == 0);
		if (f >= 5) {
			for (int i = 0; i < flen; i++) {
				speech_out_sq += (double)out[i] * (double)out[i];
				speech_cnt++;
			}
		}
	}
	double rms_1khz = sqrt(speech_out_sq / (double)speech_cnt);
	double expected_rms = 0.30 / sqrt(2.0);
	assert(fabs(rms_1khz - expected_rms) < 0.01);

	/* 2. Sub-110 Hz rumble (50 Hz) + DC should be strongly attenuated (> 20 dB) */
	jarvis_dsp_reset(&dsp);
	double low_sq = 0.0;
	int low_cnt = 0;
	for (int f = 0; f < n_frames; f++) {
		for (int i = 0; i < flen; i++) {
			int t = f * flen + i;
			float sig = 0.25f + 0.30f * sinf(2.0f * (float)M_PI * 50.0f * (float)t / 16000.0f);
			in[i] = f2s16(sig);
		}
		jarvis_dsp_process_frame(&dsp, in, (size_t)flen, out, NULL);
		if (f >= 5) {
			for (int i = 0; i < flen; i++) {
				low_sq += (double)out[i] * (double)out[i];
				low_cnt++;
			}
		}
	}
	double rms_low = sqrt(low_sq / (double)low_cnt);
	assert(rms_low < 0.02);
	printf("[PASS] Mono COLA unity passthrough (rms=%.4f vs %.4f) + 50Hz/DC HPF (rms=%.4f)\n",
	       rms_1khz, expected_rms, rms_low);
}

static void
test_mono_summed_stereo_auto_bypass(void)
{
	jarvis_dsp_t dsp;
	jarvis_dsp_init(&dsp, 16000, 2);

	int16_t in[320 * 2];
	float out[320];
	for (int f = 0; f < 40; f++) {
		for (int i = 0; i < 320; i++) {
			int16_t s = f2s16(0.20f * rand_uniform());
			in[i * 2] = s;
			in[i * 2 + 1] = s; /* L == R identical mono-summed hardware */
		}
		float ns = 0.0f;
		jarvis_dsp_process_frame(&dsp, in, 320, out, &ns);
		assert(jarvis_dsp_is_stereo_active(&dsp) == 0);
		assert(ns == 1.0f);
	}
	printf("[PASS] Mono-summed 2ch hardware auto-bypasses spatial gate (stereo_active=0, near=1.0)\n");
}

static void
test_stereo_coherence_separation(void)
{
	jarvis_dsp_t dsp;
	jarvis_dsp_init(&dsp, 16000, 2);

	int16_t in[320 * 2];
	float out[320];

	/* Phase 1 (1.0s): Diffuse uncorrelated background noise/chatter */
	double bg_in_sq = 0.0, bg_out_sq = 0.0;
	double bg_ns_sum = 0.0;
	int bg_cnt = 0;
	for (int f = 0; f < 50; f++) {
		for (int i = 0; i < 320; i++) {
			float l = 0.25f * rand_uniform();
			float r = 0.25f * rand_uniform();
			in[i * 2] = f2s16(l);
			in[i * 2 + 1] = f2s16(r);
			if (f >= 15)
				bg_in_sq += (double)(0.5f * (l + r)) * (double)(0.5f * (l + r));
		}
		float ns = 0.0f;
		jarvis_dsp_process_frame(&dsp, in, 320, out, &ns);
		if (f >= 15) {
			for (int i = 0; i < 320; i++)
				bg_out_sq += (double)out[i] * (double)out[i];
			bg_ns_sum += ns;
			bg_cnt++;
		}
	}
	assert(jarvis_dsp_is_stereo_active(&dsp) == 1);
	double bg_ns_avg = bg_ns_sum / (double)bg_cnt;
	double bg_atten_db = 10.0 * log10((bg_out_sq + 1e-12) / (bg_in_sq + 1e-12));

	/* Phase 2 (0.8s): Near-field centered broadband speech (in-phase L == R harmonics 250-5500 Hz)
	 * mixed with weaker diffuse room noise */
	double sp_in_sq = 0.0, sp_out_sq = 0.0;
	double sp_ns_sum = 0.0;
	int sp_cnt = 0;
	for (int f = 0; f < 40; f++) {
		for (int i = 0; i < 320; i++) {
			int t = f * 320 + i;
			float v = 0.12f * sinf(2.0f * (float)M_PI * 350.0f * (float)t / 16000.0f)
			        + 0.12f * sinf(2.0f * (float)M_PI * 2200.0f * (float)t / 16000.0f)
			        + 0.12f * sinf(2.0f * (float)M_PI * 3800.0f * (float)t / 16000.0f)
			        + 0.10f * sinf(2.0f * (float)M_PI * 5200.0f * (float)t / 16000.0f);
			float nl = 0.05f * rand_uniform();
			float nr = 0.05f * rand_uniform();
			float l = v + nl, r = v + nr;
			in[i * 2] = f2s16(l);
			in[i * 2 + 1] = f2s16(r);
			if (f >= 6)
				sp_in_sq += (double)v * (double)v;
		}
		float ns = 0.0f;
		jarvis_dsp_process_frame(&dsp, in, 320, out, &ns);
		if (f >= 6) {
			for (int i = 0; i < 320; i++)
				sp_out_sq += (double)out[i] * (double)out[i];
			sp_ns_sum += ns;
			sp_cnt++;
		}
	}
	double sp_ns_avg = sp_ns_sum / (double)sp_cnt;
	double sp_atten_db = 10.0 * log10((sp_out_sq + 1e-12) / (sp_in_sq + 1e-12));
	double snr_gain_db = sp_atten_db - bg_atten_db;

	assert(bg_ns_avg < 0.25);
	assert(sp_ns_avg > 0.70);
	assert(snr_gain_db > 10.0);
	printf("[PASS] Stereo coherence separation: bg_near=%.2f (%.1f dB) vs speech_near=%.2f (%.1f dB), SNR gain=+%.1f dB\n",
	       bg_ns_avg, bg_atten_db, sp_ns_avg, sp_atten_db, snr_gain_db);
}

static void
test_downward_expander_and_agc(void)
{
	/* 1. Speech burst (0.5s @ 0.25 amp) flanked by quiet background chatter (0.5s @ 0.015 amp) */
	const int n = 16000; /* 1.0s */
	float *pcm = calloc((size_t)n, sizeof(float));
	assert(pcm != NULL);
	for (int i = 0; i < n; i++) {
		float amp = (i >= 4000 && i < 12000) ? 0.25f : 0.015f;
		pcm[i] = amp * sinf(2.0f * (float)M_PI * 700.0f * (float)i / 16000.0f);
	}
	float *out = jarvis_stt_apply_preemphasis_and_agc(pcm, n);
	assert(out != NULL);

	double bg_in  = calc_rms(pcm, 3200);
	double sp_in  = calc_rms(pcm + 5000, 6000);
	double bg_out = calc_rms(out, 3200);
	double sp_out = calc_rms(out + 5000, 6000);
	double in_ratio_db  = 20.0 * log10((sp_in + 1e-9) / (bg_in + 1e-9));
	double out_ratio_db = 20.0 * log10((sp_out + 1e-9) / (bg_out + 1e-9));
	assert(out_ratio_db > in_ratio_db + 6.0);
	free(out);

	/* 2. Flat steady quiet noise should NOT be boosted by full 5x (+14 dB) AGC */
	for (int i = 0; i < n; i++)
		pcm[i] = 0.020f * sinf(2.0f * (float)M_PI * 500.0f * (float)i / 16000.0f);
	out = jarvis_stt_apply_preemphasis_and_agc(pcm, n);
	assert(out != NULL);
	double flat_in = calc_rms(pcm, (size_t)n);
	double flat_out = calc_rms(out, (size_t)n);
	double flat_gain = flat_out / (flat_in + 1e-9);
	assert(flat_gain < 2.2);
	free(out);
	free(pcm);

	printf("[PASS] Soft downward expander improved speech/gap ratio %.1f dB -> %.1f dB (+%.1f dB); flat-noise AGC capped (gain=%.2fx)\n",
	       in_ratio_db, out_ratio_db, out_ratio_db - in_ratio_db, flat_gain);
}

int
main(void)
{
	test_mono_cola_and_hpf();
	test_mono_summed_stereo_auto_bypass();
	test_stereo_coherence_separation();
	test_downward_expander_and_agc();
	printf("[PASS] All jarvis-dsp + jarvis-stt DSP unit tests passed!\n");
	return 0;
}
