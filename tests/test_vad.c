/* Standalone unit test suite for jarvis-vad.c (streaming Silero VAD abstraction). */
#include "jarvis-vad.h"
#include "whisper.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void
quiet_log(enum ggml_log_level level, const char *text, void *user_data)
{
	(void)level;
	(void)text;
	(void)user_data;
}

static uint32_t rng_s = 0x9e3779b9u;
static float
rand_u(void)
{
	rng_s = rng_s * 1664525u + 1013904223u;
	return ((float)(rng_s >> 8) / (float)(1u << 24)) * 2.0f - 1.0f;
}

static void
test_fallback_when_absent(void)
{
	jarvis_nvad_t vad;
	assert(jarvis_nvad_init(&vad, NULL, 2) == 0);
	assert(jarvis_nvad_is_available(&vad) == 0);
	float zeros[320] = {0};
	assert(jarvis_nvad_process(&vad, zeros, 320) < 0.0f);
	jarvis_nvad_reset(&vad);
	assert(jarvis_nvad_process(&vad, zeros, 320) < 0.0f);
	jarvis_nvad_free(&vad);

	assert(jarvis_nvad_init(&vad, "/nonexistent/silero-missing.bin", 2) == 0);
	assert(jarvis_nvad_is_available(&vad) == 0);
	jarvis_nvad_free(&vad);
	printf("[PASS] VAD graceful fallback when model is absent\n");
}

static void
test_streaming_silero_rejection(const char *model_path)
{
	jarvis_nvad_t vad;
	assert(jarvis_nvad_init(&vad, model_path, 2) == 0);
	assert(jarvis_nvad_is_available(&vad) == 1);

	float frame[320];

	/* 1. Pure silence (1.0s in 20ms frames): max prob < 0.10 */
	float max_sil = 0.0f;
	memset(frame, 0, sizeof(frame));
	for (int f = 0; f < 50; f++) {
		float p = jarvis_nvad_process(&vad, frame, 320);
		assert(p >= 0.0f && p <= 1.0f);
		if (p > max_sil) max_sil = p;
	}
	assert(max_sil < 0.10f);

	/* 2. Broadband white noise / fan hiss (1.2s): steady prob < 0.25 */
	jarvis_nvad_reset(&vad);
	float max_noise = 0.0f;
	for (int f = 0; f < 60; f++) {
		for (int i = 0; i < 320; i++)
			frame[i] = 0.08f * rand_u();
		float p = jarvis_nvad_process(&vad, frame, 320);
		if (f >= 5 && p > max_noise) max_noise = p;
	}
	assert(max_noise < 0.25f);

	/* 3. Sharp keyboard/mouse click impulse bursts (1.2s): prob < 0.25 */
	jarvis_nvad_reset(&vad);
	float max_click = 0.0f;
	for (int f = 0; f < 60; f++) {
		for (int i = 0; i < 320; i++) {
			float v = 0.004f * rand_u();
			if ((f % 6 == 0) && i >= 100 && i < 112)
				v += 0.65f * expf(-0.35f * (float)(i - 100)) * sinf(0.8f * (float)(i - 100));
			frame[i] = v;
		}
		float p = jarvis_nvad_process(&vad, frame, 320);
		if (p > max_click) max_click = p;
	}
	assert(max_click < 0.25f);

	/* 4. Pure single musical tone (440 Hz sine, 1.0s): prob < 0.25 */
	jarvis_nvad_reset(&vad);
	float max_tone = 0.0f;
	for (int f = 0; f < 50; f++) {
		for (int i = 0; i < 320; i++) {
			int t = f * 320 + i;
			frame[i] = 0.20f * sinf(2.0f * (float)M_PI * 440.0f * (float)t / 16000.0f);
		}
		float p = jarvis_nvad_process(&vad, frame, 320);
		if (f >= 5 && p > max_tone) max_tone = p;
	}
	assert(max_tone < 0.25f);

	printf("[PASS] Streaming Silero VAD non-speech rejection: silence=%.3f noise=%.3f clicks=%.3f tone=%.3f\n",
	       max_sil, max_noise, max_click, max_tone);

	/* 5. Optional real speech WAV fixture (from whisper.cpp samples/jfk.wav if present) */
	const char *jfk = "../submodules/whisper.cpp/samples/jfk.wav";
	FILE *fp = fopen(jfk, "rb");
	if (fp) {
		uint8_t hdr[44];
		if (fread(hdr, 1, 44, fp) == 44) {
			jarvis_nvad_reset(&vad);
			int16_t raw[320];
			float max_sp = 0.0f;
			int sp_frames = 0;
			while (fread(raw, sizeof(int16_t), 320, fp) == 320) {
				for (int i = 0; i < 320; i++)
					frame[i] = (float)raw[i] / 32768.0f;
				float p = jarvis_nvad_process(&vad, frame, 320);
				if (p > max_sp) max_sp = p;
				if (p >= 0.70f) sp_frames++;
			}
			assert(max_sp > 0.85f);
			assert(sp_frames >= 50);
			printf("[PASS] Streaming Silero VAD speech detection (jfk.wav): max_p=%.3f, frames>=0.70: %d\n",
			       max_sp, sp_frames);
		}
		fclose(fp);
	}

	jarvis_nvad_free(&vad);
}

int
main(void)
{
	whisper_log_set(quiet_log, NULL);
	test_fallback_when_absent();

	char model_path[512];
	if (jarvis_nvad_resolve_model(model_path, sizeof(model_path)) == 0) {
		test_streaming_silero_rejection(model_path);
	} else {
		printf("[SKIP] Silero VAD GGML model not present; verified fallback path only\n");
	}
	printf("[PASS] All jarvis-vad unit tests passed!\n");
	return 0;
}
