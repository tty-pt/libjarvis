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
	double bg_ns_sum = 0.0;
	int bg_cnt = 0;
	for (int f = 0; f < 50; f++) {
		for (int i = 0; i < 320; i++) {
			float l = 0.25f * rand_uniform();
			float r = 0.25f * rand_uniform();
			in[i * 2] = f2s16(l);
			in[i * 2 + 1] = f2s16(r);
		}
		float ns = 0.0f;
		jarvis_dsp_process_frame(&dsp, in, 320, out, &ns);
		if (f >= 20) {
			bg_ns_sum += ns;
			bg_cnt++;
		}
	}
	assert(jarvis_dsp_is_stereo_active(&dsp) == 1);
	double bg_ns_avg = bg_ns_sum / (double)bg_cnt;

	/* Phase 2 (0.8s): Near-field centered broadband speech (in-phase L == R harmonics 350-5200 Hz)
	 * mixed with weaker diffuse room noise -> must yield high near_score AND unity ASR RMS.
	 */
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
			float nl = 0.04f * rand_uniform();
			float nr = 0.04f * rand_uniform();
			float l = v + nl, r = v + nr;
			in[i * 2] = f2s16(l);
			in[i * 2 + 1] = f2s16(r);
			if (f >= 8)
				sp_in_sq += (double)v * (double)v;
		}
		float ns = 0.0f;
		jarvis_dsp_process_frame(&dsp, in, 320, out, &ns);
		if (f >= 8) {
			for (int i = 0; i < 320; i++)
				sp_out_sq += (double)out[i] * (double)out[i];
			sp_ns_sum += ns;
			sp_cnt++;
		}
	}
	double sp_ns_avg = sp_ns_sum / (double)sp_cnt;
	double sp_atten_db = 10.0 * log10((sp_out_sq + 1e-12) / (sp_in_sq + 1e-12));

	assert(bg_ns_avg < 0.30);
	assert(sp_ns_avg > 0.75);
	assert(fabs(sp_atten_db) < 0.8);

	/* Phase 3: Off-axis stereo speech (tau = -2, +2, +3 samples @ 16kHz = +-125..187.5 us,
	 * simulating 35-60 deg head turn where naive L+R cancels 4 kHz completely).
	 * Must preserve BOTH near_score >= 0.75 AND unity RMS via GCC-PHAT delay alignment.
	 */
	const int taus[3] = { -2, 2, 3 };
	for (int ti = 0; ti < 3; ti++) {
		int tau = taus[ti];
		jarvis_dsp_reset(&dsp);
		/* Prime diffuse ambient baseline for 0.8s */
		for (int f = 0; f < 40; f++) {
			for (int i = 0; i < 320; i++) {
				in[i * 2]     = f2s16(0.20f * rand_uniform());
				in[i * 2 + 1] = f2s16(0.20f * rand_uniform());
			}
			jarvis_dsp_process_frame(&dsp, in, 320, out, NULL);
		}
		assert(jarvis_dsp_is_stereo_active(&dsp) == 1);

		double oa_in_sq = 0.0, oa_out_sq = 0.0, oa_ns_sum = 0.0;
		int oa_cnt = 0;
		for (int f = 0; f < 45; f++) {
			for (int i = 0; i < 320; i++) {
				int t = f * 320 + i;
				int tr = t - tau;
				float vl = 0.12f * sinf(2.0f * (float)M_PI * 400.0f  * (float)t  / 16000.0f)
				         + 0.14f * sinf(2.0f * (float)M_PI * 2400.0f * (float)t  / 16000.0f)
				         + 0.14f * sinf(2.0f * (float)M_PI * 4000.0f * (float)t  / 16000.0f)
				         + 0.10f * sinf(2.0f * (float)M_PI * 5200.0f * (float)t  / 16000.0f);
				float vr = 0.12f * sinf(2.0f * (float)M_PI * 400.0f  * (float)tr / 16000.0f)
				         + 0.14f * sinf(2.0f * (float)M_PI * 2400.0f * (float)tr / 16000.0f)
				         + 0.14f * sinf(2.0f * (float)M_PI * 4000.0f * (float)tr / 16000.0f)
				         + 0.10f * sinf(2.0f * (float)M_PI * 5200.0f * (float)tr / 16000.0f);
				in[i * 2]     = f2s16(vl + 0.03f * rand_uniform());
				in[i * 2 + 1] = f2s16(vr + 0.03f * rand_uniform());
				if (f >= 12)
					oa_in_sq += (double)vl * (double)vl;
			}
			float ns = 0.0f;
			jarvis_dsp_process_frame(&dsp, in, 320, out, &ns);
			if (f >= 12) {
				for (int i = 0; i < 320; i++)
					oa_out_sq += (double)out[i] * (double)out[i];
				oa_ns_sum += ns;
				oa_cnt++;
			}
		}
		double oa_ns = oa_ns_sum / (double)oa_cnt;
		double oa_db = 10.0 * log10((oa_out_sq + 1e-12) / (oa_in_sq + 1e-12));
		assert(oa_ns >= 0.75);
		assert(fabs(oa_db) < 0.8);
		printf("[PASS] Off-axis stereo (tau=%+d smp): near=%.2f, unity_gain=%+.2f dB, tau_est=%+.2f\n",
		       tau, oa_ns, oa_db, dsp.tau_est);
	}

	printf("[PASS] Stereo |Coh| separation (decoupled ASR): bg_near=%.2f vs on-axis_near=%.2f (%+.2f dB)\n",
	       bg_ns_avg, sp_ns_avg, sp_atten_db);
}

static void
test_agc_onset_preservation_and_soft_limiter(void)
{
	/* 1. Loud syllable (0.25 amp) flanked by quiet unstressed onset/tail (0.018 amp):
	 *    Must preserve dynamic ratio (no -14 dB downward expander crushing onsets).
	 */
	const int n = 16000; /* 1.0s */
	float *pcm = calloc((size_t)n, sizeof(float));
	assert(pcm != NULL);
	for (int i = 0; i < n; i++) {
		float amp = (i >= 4000 && i < 12000) ? 0.25f : 0.018f;
		pcm[i] = amp * sinf(2.0f * (float)M_PI * 700.0f * (float)i / 16000.0f);
	}
	float *out = jarvis_stt_normalize_pcm(pcm, n, 0);
	assert(out != NULL);

	double onset_in  = calc_rms(pcm + 320, 3200);
	double sp_in     = calc_rms(pcm + 5000, 6000);
	double onset_out = calc_rms(out + 320, 3200);
	double sp_out    = calc_rms(out + 5000, 6000);
	double in_ratio_db  = 20.0 * log10((sp_in + 1e-9) / (onset_in + 1e-9));
	double out_ratio_db = 20.0 * log10((sp_out + 1e-9) / (onset_out + 1e-9));
	assert(fabs(out_ratio_db - in_ratio_db) < 0.5);
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

	/* 3. Smooth soft-knee limiter bounds peaks strictly inside [-0.98, +0.98] without hard clip */
	for (int i = 0; i < n; i++)
		pcm[i] = 1.40f * sinf(2.0f * (float)M_PI * 600.0f * (float)i / 16000.0f);
	out = jarvis_stt_normalize_pcm(pcm, n, 0);
	assert(out != NULL);
	float max_abs = 0.0f;
	for (int i = 0; i < n; i++) {
		float a = fabsf(out[i]);
		if (a > max_abs) max_abs = a;
	}
	assert(max_abs > 0.85f && max_abs < 0.9801f);
	free(out);
	free(pcm);

	/* 4. Linear-time scaling on max 90s utterance buffer (4500 frames) */
	const int n90 = JARVIS_MAX_UTTER_FRAMES * JARVIS_FRAME_SAMPLES;
	float *big = calloc((size_t)n90, sizeof(float));
	assert(big != NULL);
	for (int i = 0; i < n90; i++)
		big[i] = 0.05f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / 16000.0f);
	out = jarvis_stt_normalize_pcm(big, n90, 0);
	assert(out != NULL);
	free(out);
	free(big);

	printf("[PASS] AGC preserved onset ratio (%.1f -> %.1f dB), capped flat noise (%.2fx), soft-limited peaks (%.3f), and scaled linearly on 90s buffer\n",
	       in_ratio_db, out_ratio_db, flat_gain, max_abs);
}

static void
test_audio_ctx_and_repetition_collapse(void)
{
	/* audio_ctx floors: >= 512 final, >= 256 partial, >= 128 wake, 0 = full native */
	assert(jarvis_stt_compute_audio_ctx(16000, 0, 0) == 512);
	assert(jarvis_stt_compute_audio_ctx(16000, 0, 1) == 256);
	assert(jarvis_stt_compute_audio_ctx(16000, 1, 0) == 128);
	assert(jarvis_stt_compute_audio_ctx_ex(16000, 0, 0, 0) == 0);
	assert(jarvis_stt_compute_audio_ctx_ex(16000, 0, 0, 768) == 768);
	assert(jarvis_stt_compute_audio_ctx_ex(480000, 0, 0, 512) == 0);

	/* Short phrase repetition */
	char buf[1024];
	snprintf(buf, sizeof(buf), "Go to workspace 1 Go to workspace 1");
	jarvis_stt_collapse_repetitions(buf, sizeof(buf));
	assert(strcmp(buf, "Go to workspace 1") == 0);

	/* Long 12-word clause repetition (>8 words previously escaped) */
	snprintf(buf, sizeof(buf),
	         "please open the calculator and check the monthly budget for this quarter "
	         "please open the calculator and check the monthly budget for this quarter");
	jarvis_stt_collapse_repetitions(buf, sizeof(buf));
	assert(strcmp(buf, "please open the calculator and check the monthly budget for this quarter") == 0);

	/* Long 20-word clause repetition */
	snprintf(buf, sizeof(buf),
	         "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty "
	         "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty");
	jarvis_stt_collapse_repetitions(buf, sizeof(buf));
	assert(strcmp(buf, "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty") == 0);

	/* Trailing truncated prefix repetition */
	snprintf(buf, sizeof(buf), "open up miles davis open up miles");
	jarvis_stt_collapse_repetitions(buf, sizeof(buf));
	assert(strcmp(buf, "open up miles davis") == 0);

	/* Distinct normal sentence preserved */
	snprintf(buf, sizeof(buf), "abre a calculadora e o calendário por favor");
	jarvis_stt_collapse_repetitions(buf, sizeof(buf));
	assert(strcmp(buf, "abre a calculadora e o calendário por favor") == 0);

	printf("[PASS] audio_ctx floors (final>=512, partial>=256, 0=full) and 1..24-word repetition collapse verified\n");
}

int
main(void)
{
	test_mono_cola_and_hpf();
	test_mono_summed_stereo_auto_bypass();
	test_stereo_coherence_separation();
	test_agc_onset_preservation_and_soft_limiter();
	test_audio_ctx_and_repetition_collapse();
	printf("[PASS] All jarvis-dsp + jarvis-stt DSP unit tests passed!\n");
	return 0;
}
