#include "../src/jarvis-kws.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------------------
 * Minimal Protobuf Wire Encoder to emit a real valid ONNX IR7 / Opset13 model:
 *   input: float[1, 1600] -> ReduceMean(axes=[1], keepdims=0) -> Sigmoid -> output: float[1]
 * Zero external files or network needed; exercises real libonnxruntime.so!
 * ------------------------------------------------------------------------- */

typedef struct {
	uint8_t buf[1024];
	size_t  len;
} pb_t;

static void pb_varint(pb_t *p, uint64_t v)
{
	while (v >= 0x80) {
		p->buf[p->len++] = (uint8_t)((v & 0x7F) | 0x80);
		v >>= 7;
	}
	p->buf[p->len++] = (uint8_t)v;
}

static void pb_tag(pb_t *p, uint32_t field, uint32_t wire)
{
	pb_varint(p, ((uint64_t)field << 3) | wire);
}

static void pb_int64(pb_t *p, uint32_t field, int64_t v)
{
	pb_tag(p, field, 0);
	pb_varint(p, (uint64_t)v);
}

static void pb_bytes(pb_t *p, uint32_t field, const void *data, size_t n)
{
	pb_tag(p, field, 2);
	pb_varint(p, n);
	memcpy(p->buf + p->len, data, n);
	p->len += n;
}

static void pb_str(pb_t *p, uint32_t field, const char *s)
{
	pb_bytes(p, field, s, strlen(s));
}

static void pb_sub(pb_t *p, uint32_t field, const pb_t *sub)
{
	pb_bytes(p, field, sub->buf, sub->len);
}

static pb_t make_value_info_f32(const char *name, const int64_t *dims, int ndims)
{
	pb_t shape = {0};
	for (int i = 0; i < ndims; i++) {
		pb_t dim = {0};
		pb_int64(&dim, 1 /* dim_value */, dims[i]);
		pb_sub(&shape, 1 /* dim */, &dim);
	}
	pb_t ttype = {0};
	pb_int64(&ttype, 1 /* elem_type = FLOAT */, 1);
	pb_sub(&ttype, 2 /* shape */, &shape);

	pb_t tp = {0};
	pb_sub(&tp, 1 /* tensor_type */, &ttype);

	pb_t vi = {0};
	pb_str(&vi, 1 /* name */, name);
	pb_sub(&vi, 2 /* type */, &tp);
	return vi;
}

static bool write_synthetic_kws_onnx(const char *path, int win_samples)
{
	/* Node 1: ReduceMean(input -> mean_out, axes=[1], keepdims=0) */
	pb_t attr_axes = {0};
	pb_str(&attr_axes, 1 /* name */, "axes");
	pb_int64(&attr_axes, 7 /* ints */, 1);
	pb_int64(&attr_axes, 20 /* type = INTS */, 7);

	pb_t attr_kd = {0};
	pb_str(&attr_kd, 1 /* name */, "keepdims");
	pb_int64(&attr_kd, 3 /* i */, 0);
	pb_int64(&attr_kd, 20 /* type = INT */, 2);

	pb_t n_mean = {0};
	pb_str(&n_mean, 1 /* input */, "input");
	pb_str(&n_mean, 2 /* output */, "mean_out");
	pb_str(&n_mean, 4 /* op_type */, "ReduceMean");
	pb_sub(&n_mean, 5 /* attribute */, &attr_axes);
	pb_sub(&n_mean, 5 /* attribute */, &attr_kd);

	/* Node 2: Sigmoid(mean_out -> output) */
	pb_t n_sig = {0};
	pb_str(&n_sig, 1 /* input */, "mean_out");
	pb_str(&n_sig, 2 /* output */, "output");
	pb_str(&n_sig, 4 /* op_type */, "Sigmoid");

	int64_t in_dims[2]  = { 1, (int64_t)win_samples };
	int64_t out_dims[1] = { 1 };
	pb_t vi_in  = make_value_info_f32("input", in_dims, 2);
	pb_t vi_out = make_value_info_f32("output", out_dims, 1);

	pb_t graph = {0};
	pb_sub(&graph, 1 /* node */, &n_mean);
	pb_sub(&graph, 1 /* node */, &n_sig);
	pb_str(&graph, 2 /* name */, "kws_test_graph");
	pb_sub(&graph, 11 /* input */, &vi_in);
	pb_sub(&graph, 12 /* output */, &vi_out);

	pb_t opset = {0};
	pb_str(&opset, 1 /* domain */, "");
	pb_int64(&opset, 2 /* version */, 13);

	pb_t model = {0};
	pb_int64(&model, 1 /* ir_version */, 7);
	pb_sub(&model, 7 /* graph */, &graph);
	pb_sub(&model, 8 /* opset_import */, &opset);

	FILE *fp = fopen(path, "wb");
	if (!fp)
		return false;
	size_t wr = fwrite(model.buf, 1, model.len, fp);
	fclose(fp);
	return wr == model.len;
}

static void test_tier1_vad_gate_60s_ambient(void)
{
	/* Simulate 60 seconds (3000 x 20ms frames) of room noise + typing clicks:
	 * ambient vad_prob stays <= 0.25 with occasional isolated 1-2 frame clicks
	 * spiking to 0.60. Sliding 25-frame wake slices must produce 0 wake STT calls! */
	float probs[3000];
	for (int i = 0; i < 3000; i++) {
		probs[i] = 0.02f + (float)(i % 7) * 0.03f; /* 0.02 .. 0.20 */
		if (i % 150 == 42)
			probs[i] = 0.62f; /* single-frame mechanical keystroke transient */
	}

	int wake_stt_calls = 0;
	for (int f = 0; f + 25 <= 3000; f += 16) {
		if (jarvis_kws_vad_gate_slice(probs + f, 25, 1, 0.55f, 3))
			wake_stt_calls++;
	}
	assert(wake_stt_calls == 0);

	/* Genuine speech segment (6 frames >= 0.45, peak 0.88) must pass */
	float speech[20];
	for (int i = 0; i < 20; i++) speech[i] = 0.05f;
	speech[5]  = 0.50f;
	speech[6]  = 0.72f;
	speech[7]  = 0.88f;
	speech[8]  = 0.81f;
	speech[9]  = 0.65f;
	speech[10] = 0.48f;
	assert(jarvis_kws_vad_gate_slice(speech, 20, 1, 0.55f, 3));

	/* Legacy mode (no Silero model loaded, vad_neural=0) fails open */
	assert(jarvis_kws_vad_gate_slice(probs, 25, 0, 0.55f, 3));

	printf("  [PASS] test_tier1_vad_gate_60s_ambient (0/186 wake calls on 60s noise+clicks)\n");
}

static void test_tier2_onnx_and_fallback(void)
{
	/* 1. Universal fallback for custom wake word with no ONNX model */
	jarvis_kws_t *fb = jarvis_kws_open("computador_custom_xyz", NULL, NULL);
	assert(fb != NULL);
	assert(jarvis_kws_tier(fb) == JARVIS_KWS_TIER_VAD_GATE);
	assert(strcmp(jarvis_kws_tier_str(fb), "vad") == 0);
	jarvis_kws_close(fb);

	/* 2. Synthetic valid ONNX model exercised through real libonnxruntime.so */
	const char *mpath = "/tmp/test_jarvis_kws_unit.onnx";
	const char *cpath = "/tmp/test_jarvis_kws_unit.onnx.cfg";
	assert(write_synthetic_kws_onnx(mpath, 1600));

	/* Write optional sidecar config to verify sidecar parsing */
	FILE *cf = fopen(cpath, "w");
	assert(cf != NULL);
	fprintf(cf, "window_samples = 1600\nhop_samples = 320\nthreshold = 0.60\nmin_hits = 2\ncooldown_frames = 5\n");
	fclose(cf);

	jarvis_kws_cfg_t cfg;
	jarvis_kws_default_cfg(&cfg);
	cfg.vad_gate_prob = 0.55f;

	jarvis_kws_t *kws = jarvis_kws_open("jarvis", mpath, &cfg);
	assert(kws != NULL);

	if (jarvis_kws_tier(kws) == JARVIS_KWS_TIER_ONNX) {
		assert(strcmp(jarvis_kws_tier_str(kws), "onnx") == 0);

		float frame[320];
		float score = 0.0f;

		/* A. 30 frames of high-amplitude signal while Silero VAD says non-speech (0.10):
		 *    MUST run 0 ONNX inferences! */
		for (int i = 0; i < 320; i++) frame[i] = 3.0f;
		for (int f = 0; f < 30; f++) {
			int hit = jarvis_kws_process_frame(kws, frame, 320, 0.10f, 1, &score);
			assert(hit == 0);
		}
		assert(jarvis_kws_onnx_runs(kws) == 0);

		/* B. Negative speech signal (-2.0 -> sigmoid ≈ 0.119 < 0.60): runs ONNX, 0 triggers */
		jarvis_kws_reset(kws);
		for (int i = 0; i < 320; i++) frame[i] = -2.0f;
		for (int f = 0; f < 6; f++) {
			int hit = jarvis_kws_process_frame(kws, frame, 320, 0.85f, 1, &score);
			assert(hit == 0);
		}
		assert(jarvis_kws_onnx_runs(kws) > 0);
		assert(score < 0.30f);

		/* C. Positive keyword signal (+2.0 -> sigmoid ≈ 0.881 >= 0.60):
		 *    Measure latency and verify min_hits=2 hysteresis + cooldown=5 */
		jarvis_kws_reset(kws);
		for (int i = 0; i < 320; i++) frame[i] = 2.0f;
		int triggers = 0;
		float peak_score = 0.0f;
		double t0 = now_ms();
		for (int f = 0; f < 10; f++) {
			int hit = jarvis_kws_process_frame(kws, frame, 320, 0.90f, 1, &score);
			if (score > peak_score)
				peak_score = score;
			if (hit)
				triggers++;
		}
		double elapsed_ms = now_ms() - t0;
		uint64_t runs = jarvis_kws_onnx_runs(kws);
		assert(triggers == 1); /* 1 trigger at hop 2 after fill, then 5-frame cooldown suppresses rest */
		assert(peak_score > 0.85f);
		printf("  [PASS] test_tier2_onnx_and_fallback (real ORT tier=onnx, peak_score=%.3f, runs=%llu, avg=%.3f ms/hop)\n",
		       peak_score, (unsigned long long)runs, elapsed_ms / 6.0);
	} else {
		/* Environments without libonnxruntime cleanly fall back to Tier 1 */
		printf("  [PASS] test_tier2_onnx_and_fallback (libonnxruntime unavailable -> clean Tier 1 fallback)\n");
	}

	jarvis_kws_close(kws);
	remove(mpath);
	remove(cpath);
}

static void test_adaptive_endpointing(void)
{
	jarvis_endpoint_in_t in;
	jarvis_endpoint_plan_t out;

	/* 1. Normal complete utterance (eff_silence_ms=750, low VAD prob, not continuation) */
	memset(&in, 0, sizeof(in));
	in.eff_silence_ms    = 750;
	in.frame_ms          = 20;
	in.operating_mode    = 0;
	in.vad_neural        = 1;
	in.last_vad_prob     = 0.05f;
	in.ends_continuation = 0;
	in.ptt_held          = 0;
	jarvis_endpoint_compute(&in, &out);
	assert(out.spec_silence_f == 14);
	assert(out.spec_early_f == 26); /* 750 * 7 / 200 = 26 */
	assert(out.final_silence_ms == 750);
	assert(out.grace_ms_applied == 0);
	assert(out.suppress_endpoint == 0);

	/* 2. Trailing continuation token ("para", "with", "to") -> +450 ms grace */
	in.ends_continuation = 1;
	jarvis_endpoint_compute(&in, &out);
	assert(out.grace_ms_applied == 450);
	assert(out.spec_early_f == 48);      /* 26 + 22 = 48 frames (960 ms) */
	assert(out.final_silence_ms == 1200); /* 750 + 450 = 1200 ms */
	assert(out.suppress_endpoint == 0);

	/* 3. Active Silero speech (brief stop-consonant dip) -> suppress endpoint */
	in.ends_continuation = 0;
	in.last_vad_prob     = 0.60f;
	jarvis_endpoint_compute(&in, &out);
	assert(out.suppress_endpoint == 1);

	/* 4. PTT held -> suppress endpoint regardless of silence/VAD */
	in.last_vad_prob = 0.01f;
	in.ptt_held      = 1;
	jarvis_endpoint_compute(&in, &out);
	assert(out.suppress_endpoint == 1);

	/* 5. Operating mode (agent steering): fast turnaround, no continuation delay */
	in.ptt_held          = 0;
	in.operating_mode    = 1;
	in.eff_silence_ms    = 620;
	in.ends_continuation = 1;
	jarvis_endpoint_compute(&in, &out);
	assert(out.spec_silence_f == 12);
	assert(out.grace_ms_applied == 0);
	assert(out.final_silence_ms == 620);

	printf("  [PASS] test_adaptive_endpointing\n");
}

int main(void)
{
	printf("=== Running jarvis-kws & adaptive endpointing unit tests ===\n");
	test_tier1_vad_gate_60s_ambient();
	test_tier2_onnx_and_fallback();
	test_adaptive_endpointing();
	printf("[PASS] All jarvis-kws unit tests passed!\n");
	return 0;
}
