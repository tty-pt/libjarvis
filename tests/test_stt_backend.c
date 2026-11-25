#define _POSIX_C_SOURCE 200809L
#include "jarvis-stt-backend.h"
#include "jarvis-stt.h"
#include "jarvis-stream.h"
#include "jarvis-ipc-srv.h"

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void
test_lang_support(void)
{
	/* Whisper supports all languages */
	assert(jarvis_stt_whisper_supports_lang("en"));
	assert(jarvis_stt_whisper_supports_lang("pt-PT"));
	assert(jarvis_stt_whisper_supports_lang("ja"));
	assert(jarvis_stt_whisper_supports_lang("zh"));
	assert(jarvis_stt_whisper_supports_lang("ar"));
	assert(jarvis_stt_whisper_supports_lang("auto"));
	assert(jarvis_stt_whisper_supports_lang(NULL));

	/* Parakeet-TDT-0.6B-v3 supports 25 European languages + region tags + auto */
	assert(jarvis_stt_parakeet_supports_lang("en"));
	assert(jarvis_stt_parakeet_supports_lang("en-GB"));
	assert(jarvis_stt_parakeet_supports_lang("pt"));
	assert(jarvis_stt_parakeet_supports_lang("pt-PT"));
	assert(jarvis_stt_parakeet_supports_lang("pt_BR"));
	assert(jarvis_stt_parakeet_supports_lang("es"));
	assert(jarvis_stt_parakeet_supports_lang("fr-FR"));
	assert(jarvis_stt_parakeet_supports_lang("de"));
	assert(jarvis_stt_parakeet_supports_lang("uk"));
	assert(jarvis_stt_parakeet_supports_lang("el"));
	assert(jarvis_stt_parakeet_supports_lang("auto"));
	assert(jarvis_stt_parakeet_supports_lang(""));

	/* Non-European languages must route to Whisper fallback */
	assert(!jarvis_stt_parakeet_supports_lang("ja"));
	assert(!jarvis_stt_parakeet_supports_lang("zh"));
	assert(!jarvis_stt_parakeet_supports_lang("zh-CN"));
	assert(!jarvis_stt_parakeet_supports_lang("ko"));
	assert(!jarvis_stt_parakeet_supports_lang("ar"));
	assert(!jarvis_stt_parakeet_supports_lang("hi"));
	assert(!jarvis_stt_parakeet_supports_lang("he"));
	assert(!jarvis_stt_parakeet_supports_lang("th"));
	assert(!jarvis_stt_parakeet_supports_lang("vi"));

	printf("[PASS] test_lang_support\n");
}

static int
copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb");
	if (!in) return -1;
	FILE *out = fopen(dst, "wb");
	if (!out) { fclose(in); return -1; }
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return -1;
		}
	}
	fclose(in);
	fclose(out);
	return 0;
}

static void
test_model_format_detection(void)
{
	const char *pk_path = "../submodules/whisper.cpp/models/for-tests-ggml-parakeet-tdt.bin";
	const char *wh_path = "../submodules/whisper.cpp/models/for-tests-ggml-tiny.bin";

	assert(jarvis_stt_model_looks_parakeet(pk_path));
	assert(!jarvis_stt_model_looks_parakeet(wh_path));
	assert(!jarvis_stt_model_looks_parakeet("/nonexistent/model.bin"));

	/* Verify binary GGML header detection works even without "parakeet" in filename */
	char tmp_pk[] = "/tmp/jarvis_test_m1_XXXXXX";
	char tmp_wh[] = "/tmp/jarvis_test_m2_XXXXXX";
	int fd1 = mkstemp(tmp_pk);
	int fd2 = mkstemp(tmp_wh);
	assert(fd1 >= 0 && fd2 >= 0);
	close(fd1);
	close(fd2);

	assert(copy_file(pk_path, tmp_pk) == 0);
	assert(copy_file(wh_path, tmp_wh) == 0);
	assert(jarvis_stt_model_looks_parakeet(tmp_pk));
	assert(!jarvis_stt_model_looks_parakeet(tmp_wh));

	unlink(tmp_pk);
	unlink(tmp_wh);
	printf("[PASS] test_model_format_detection\n");
}

static void
test_hw_calibration_tiers(void)
{
	jarvis_hw_profile_t p;

	/* Fast discrete/unified GPU */
	memset(&p, 0, sizeof(p));
	p.has_gpu = 1;
	jarvis_hw_calibrate_profile(&p, 180, 380);
	assert(p.tier == JARVIS_HW_FAST_GPU);
	assert(p.final_audio_ctx == 0);
	assert(strcmp(jarvis_hw_tier_str(p.tier), "fast_gpu") == 0);

	/* Laptop iGPU (ac512 ~680ms, ac1500 skipped) */
	memset(&p, 0, sizeof(p));
	p.has_gpu = 1;
	p.is_igpu = 1;
	jarvis_hw_calibrate_profile(&p, 680, 0);
	assert(p.tier == JARVIS_HW_IGPU);
	assert(p.final_audio_ctx == 512);
	assert(strcmp(jarvis_hw_tier_str(p.tier), "igpu") == 0);

	/* Faster iGPU where ac512 <= 450ms but ac1500 > 650ms */
	memset(&p, 0, sizeof(p));
	p.has_gpu = 1;
	p.is_igpu = 1;
	jarvis_hw_calibrate_profile(&p, 380, 900);
	assert(p.tier == JARVIS_HW_IGPU);
	assert(p.final_audio_ctx == 768);

	/* CPU-only slow host */
	memset(&p, 0, sizeof(p));
	p.has_gpu = 0;
	jarvis_hw_calibrate_profile(&p, 1800, 0);
	assert(p.tier == JARVIS_HW_CPU);
	assert(p.final_audio_ctx == 512);
	assert(strcmp(jarvis_hw_tier_str(p.tier), "cpu") == 0);

	/* CPU-only fast host */
	memset(&p, 0, sizeof(p));
	p.has_gpu = 0;
	jarvis_hw_calibrate_profile(&p, 320, 0);
	assert(p.tier == JARVIS_HW_CPU);
	assert(p.final_audio_ctx == 768);

	/* Live host device probe */
	jarvis_hw_probe_devices(&p);
	assert(p.backend[0] != '\0');
	assert(p.device[0] != '\0');
	printf("  [probe] host backend=%s device=\"%s\" tier=%s has_gpu=%d is_igpu=%d\n",
	       p.backend, p.device, jarvis_hw_tier_str(p.tier), p.has_gpu, p.is_igpu);
	printf("[PASS] test_hw_calibration_tiers\n");
}

static void
test_whisper_and_parakeet_fixtures(void)
{
	const char *pk_path  = "../submodules/whisper.cpp/models/for-tests-ggml-parakeet-tdt.bin";
	const char *pk_bad   = "../submodules/whisper.cpp/models/for-tests-ggml-parakeet-tdt-bad-nfft0.bin";
	const char *wh_path  = "../submodules/whisper.cpp/models/for-tests-ggml-tiny.bin";
	static float silence[16000];
	jarvis_stt_res_t res;

	assert(jarvis_stt_open_auto("/nonexistent/model.bin", false) == NULL);
	assert(jarvis_stt_open_parakeet(pk_bad, false) == NULL);

	/* Whisper fixture via auto-opener */
	jarvis_stt_ctx_t *wctx = jarvis_stt_open_auto(wh_path, false);
	assert(wctx != NULL);
	assert(strcmp(jarvis_stt_backend_name(wctx), "whisper") == 0);
	assert(jarvis_stt_is_multilingual(wctx));
	assert(jarvis_stt_supports_lang(wctx, "ja"));

	jarvis_stt_word_t words[JARVIS_STT_MAX_WORDS];
	jarvis_stt_req_t wreq;
	memset(&wreq, 0, sizeof(wreq));
	wreq.pcm = silence;
	wreq.n_samples = 16000;
	wreq.n_threads = 2;
	wreq.lang = "en";
	wreq.max_tok = 4;
	wreq.audio_ctx = 128;
	wreq.beam_size = 1;
	wreq.is_partial = 1;
	wreq.use_context = 1;
	wreq.out_words = words;
	wreq.max_words = JARVIS_STT_MAX_WORDS;
	assert(jarvis_stt_transcribe(wctx, &wreq, &res) == 0);
	assert(res.n_words >= 0 && res.n_words <= JARVIS_STT_MAX_WORDS);
	for (int k = 0; k < res.n_words; k++) {
		assert(words[k].start_ms >= 0 && words[k].end_ms >= words[k].start_ms);
		assert(words[k].end_ms <= 1000);
		if (k > 0)
			assert(words[k].start_ms >= words[k - 1].start_ms);
	}
	jarvis_stt_close(wctx);

	/* Parakeet fixture via auto-opener */
	jarvis_stt_ctx_t *pctx = jarvis_stt_open_auto(pk_path, false);
	assert(pctx != NULL);
	assert(strcmp(jarvis_stt_backend_name(pctx), "parakeet") == 0);
	assert(jarvis_stt_is_multilingual(pctx));
	assert(jarvis_stt_supports_lang(pctx, "pt-PT"));
	assert(!jarvis_stt_supports_lang(pctx, "ja"));

	jarvis_stt_req_t preq;
	memset(&preq, 0, sizeof(preq));
	preq.pcm = silence;
	preq.n_samples = 16000;
	preq.n_threads = 2;
	preq.lang = "pt";
	preq.max_tok = 16;
	preq.use_context = 1;
	preq.out_words = words;
	preq.max_words = JARVIS_STT_MAX_WORDS;
	assert(jarvis_stt_transcribe(pctx, &preq, &res) == 0);
	assert(res.n_words >= 0 && res.n_words <= JARVIS_STT_MAX_WORDS);

	/* Exercise warmup/calibration path on Parakeet fixture */
	jarvis_hw_profile_t prof;
	jarvis_hw_warmup_and_calibrate(&prof, pctx, NULL, 2, "pt");
	assert(prof.final_audio_ctx == 0);
	assert(prof.ac512_ms >= 1);

	jarvis_stt_close(pctx);
	printf("[PASS] test_whisper_and_parakeet_fixtures\n");
}

static void
touch_file(const char *path)
{
	FILE *fp = fopen(path, "wb");
	assert(fp != NULL);
	fputc(0, fp);
	fclose(fp);
}

static void
write_rc(const char *rc_path, const char *content)
{
	FILE *fp = fopen(rc_path, "wb");
	assert(fp != NULL);
	fputs(content, fp);
	fclose(fp);
}

static void
test_tier_and_locale_model_resolution(void)
{
	char tmpdir[] = "/tmp/jarvis_models_test_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	char wh_dir[512], jm_dir[512], j_dir[512], rc_path[512];
	char f_q8[600], f_q5[600], f_sm[600], f_base[600], f_pk[600], f_ptpt[600];
	snprintf(wh_dir, sizeof(wh_dir), "%s/whisper", tmpdir);
	snprintf(j_dir, sizeof(j_dir), "%s/jarvis", tmpdir);
	snprintf(jm_dir, sizeof(jm_dir), "%s/jarvis/models", tmpdir);
	snprintf(rc_path, sizeof(rc_path), "%s/jarvisrc", tmpdir);
	assert(mkdir(wh_dir, 0755) == 0);
	assert(mkdir(j_dir, 0755) == 0);
	assert(mkdir(jm_dir, 0755) == 0);

	snprintf(f_q8, sizeof(f_q8), "%s/ggml-large-v3-turbo-q8_0.bin", wh_dir);
	snprintf(f_q5, sizeof(f_q5), "%s/ggml-large-v3-turbo-q5_0.bin", wh_dir);
	snprintf(f_sm, sizeof(f_sm), "%s/ggml-small-q8_0.bin", wh_dir);
	snprintf(f_base, sizeof(f_base), "%s/ggml-base-q8_0.bin", wh_dir);
	snprintf(f_pk, sizeof(f_pk), "%s/ggml-parakeet-tdt-0.6b-v3-q8_0.bin", jm_dir);
	snprintf(f_ptpt, sizeof(f_ptpt), "%s/whisper-lv3-ep.bin", wh_dir);

	touch_file(f_q8);
	touch_file(f_q5);
	touch_file(f_sm);
	touch_file(f_base);
	touch_file(f_ptpt);

	const char *old_xdg = getenv("XDG_DATA_HOME");
	const char *old_rc  = getenv("JARVISRC");
	char saved_xdg[512] = {0}, saved_rc[512] = {0};
	if (old_xdg) snprintf(saved_xdg, sizeof(saved_xdg), "%s", old_xdg);
	if (old_rc)  snprintf(saved_rc, sizeof(saved_rc), "%s", old_rc);

	setenv("XDG_DATA_HOME", tmpdir, 1);
	setenv("JARVISRC", rc_path, 1);

	char main_m[512], wake_m[512];

	/* 1. Tier-aware Whisper selection (no Parakeet installed yet) */
	write_rc(rc_path, "lang = pt\n");
	jarvis_resolve_stt_models(JARVIS_HW_FAST_GPU, main_m, sizeof(main_m), wake_m, sizeof(wake_m));
	assert(strstr(main_m, "ggml-large-v3-turbo-q8_0.bin") != NULL);
	jarvis_resolve_stt_models(JARVIS_HW_IGPU, main_m, sizeof(main_m), wake_m, sizeof(wake_m));
	assert(strstr(main_m, "ggml-large-v3-turbo-q5_0.bin") != NULL);
	jarvis_resolve_stt_models(JARVIS_HW_CPU, main_m, sizeof(main_m), wake_m, sizeof(wake_m));
	assert(strstr(main_m, "ggml-small-q8_0.bin") != NULL);

	/* 2. Per-locale override whisper_model.pt-pt */
	char rc_buf[1024];
	snprintf(rc_buf, sizeof(rc_buf), "lang = pt-PT\nwhisper_model.pt-pt = %s\n", f_ptpt);
	write_rc(rc_path, rc_buf);
	jarvis_resolve_stt_models(JARVIS_HW_IGPU, main_m, sizeof(main_m), wake_m, sizeof(wake_m));
	assert(strcmp(main_m, f_ptpt) == 0);

	/* 3. Parakeet auto-selection for EU lang vs Whisper fallback for ja & stt_backend=whisper */
	touch_file(f_pk);
	write_rc(rc_path, "lang = pt\n");
	jarvis_resolve_stt_models(JARVIS_HW_IGPU, main_m, sizeof(main_m), wake_m, sizeof(wake_m));
	assert(strcmp(main_m, f_pk) == 0);

	write_rc(rc_path, "lang = ja\n");
	jarvis_resolve_stt_models(JARVIS_HW_IGPU, main_m, sizeof(main_m), wake_m, sizeof(wake_m));
	assert(strstr(main_m, "ggml-large-v3-turbo-q5_0.bin") != NULL);

	write_rc(rc_path, "lang = pt\nstt_backend = whisper\n");
	jarvis_resolve_stt_models(JARVIS_HW_IGPU, main_m, sizeof(main_m), wake_m, sizeof(wake_m));
	assert(strstr(main_m, "ggml-large-v3-turbo-q5_0.bin") != NULL);

	/* Restore env and cleanup temp files */
	if (old_xdg) setenv("XDG_DATA_HOME", saved_xdg, 1); else unsetenv("XDG_DATA_HOME");
	if (old_rc)  setenv("JARVISRC", saved_rc, 1); else unsetenv("JARVISRC");

	unlink(f_q8);
	unlink(f_q5);
	unlink(f_sm);
	unlink(f_base);
	unlink(f_pk);
	unlink(f_ptpt);
	unlink(rc_path);
	rmdir(jm_dir);
	rmdir(j_dir);
	rmdir(wh_dir);
	rmdir(tmpdir);
	printf("[PASS] test_tier_and_locale_model_resolution\n");
}

static int
load_wav_16k_f32(const char *path, float **out_pcm, size_t *out_n)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;

	unsigned char hdr[12];
	if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
	    memcmp(hdr + 8, "WAVE", 4) != 0) {
		fclose(fp);
		return -1;
	}

	uint32_t data_bytes = 0;
	unsigned char ch[8];
	while (fread(ch, 1, 8, fp) == 8) {
		uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
		              ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
		if (memcmp(ch, "data", 4) == 0) {
			data_bytes = sz;
			break;
		}
		if (fseek(fp, (long)sz + (long)(sz & 1U), SEEK_CUR) != 0)
			break;
	}
	if (data_bytes < 32000) {
		fclose(fp);
		return -1;
	}

	size_t n = data_bytes / 2;
	int16_t *raw = (int16_t *)malloc(n * sizeof(int16_t));
	float *pcm = (float *)malloc(n * sizeof(float));
	if (!raw || !pcm) {
		free(raw);
		free(pcm);
		fclose(fp);
		return -1;
	}
	size_t got = fread(raw, sizeof(int16_t), n, fp);
	fclose(fp);
	for (size_t i = 0; i < got; i++)
		pcm[i] = (float)raw[i] / 32768.0f;
	free(raw);
	*out_pcm = pcm;
	*out_n = got;
	return 0;
}

static int
count_word_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle)
		return 0;
	size_t nlen = strlen(needle);
	int cnt = 0;
	for (const char *p = haystack; *p; p++) {
		size_t k = 0;
		while (k < nlen && p[k] &&
		       tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k]))
			k++;
		if (k == nlen) {
			int left_ok = (p == haystack || !isalpha((unsigned char)p[-1]));
			int right_ok = (!isalpha((unsigned char)p[nlen]));
			if (left_ok && right_ok)
				cnt++;
		}
	}
	return cnt;
}

static double
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void
test_long_form_real_whisper_stream(void)
{
	const char *wav_path = "../submodules/whisper.cpp/samples/jfk.wav";
	const char *home = getenv("HOME");
	char model_path[512] = {0};
	const char *candidates[] = {
		"ggml-base.en-q8_0.bin",
		"ggml-tiny-q5_1.bin",
		"ggml-tiny.bin",
		"ggml-base-q8_0.bin",
		NULL
	};
	if (home && *home) {
		for (int i = 0; candidates[i]; i++) {
			char p[512];
			snprintf(p, sizeof(p), "%s/.local/share/whisper/%s", home, candidates[i]);
			if (access(p, R_OK) == 0) {
				snprintf(model_path, sizeof(model_path), "%s", p);
				break;
			}
		}
	}
	if (!model_path[0] || access(wav_path, R_OK) != 0) {
		printf("[SKIP] test_long_form_real_whisper_stream (no local Whisper model or jfk.wav)\n");
		return;
	}

	float *jfk_pcm = NULL;
	size_t jfk_n = 0;
	assert(load_wav_16k_f32(wav_path, &jfk_pcm, &jfk_n) == 0);

	/* Build a 55+ second continuous utterance: 5x jfk.wav with 250ms clean silence gaps */
	const int reps = 5;
	const size_t gap_n = 4000; /* 250 ms at 16 kHz */
	size_t total_n = (size_t)reps * jfk_n + (size_t)(reps - 1) * gap_n;
	float *audio = (float *)calloc(total_n, sizeof(float));
	assert(audio != NULL);
	size_t off = 0;
	for (int r = 0; r < reps; r++) {
		memcpy(audio + off, jfk_pcm, jfk_n * sizeof(float));
		off += jfk_n;
		if (r + 1 < reps)
			off += gap_n;
	}
	free(jfk_pcm);

	int total_ms = (int)((total_n * 1000ULL) / 16000ULL);
	assert(total_ms > 50000);

	jarvis_stt_ctx_t *wctx = jarvis_stt_open_auto(model_path, true);
	assert(wctx != NULL);

	jarvis_stream_t *st = (jarvis_stream_t *)calloc(1, sizeof(jarvis_stream_t));
	jarvis_stt_word_t *words = (jarvis_stt_word_t *)calloc(JARVIS_STT_MAX_WORDS, sizeof(jarvis_stt_word_t));
	assert(st && words);
	jarvis_stream_init(st, 0, 20);

	int max_slice_ms = 0;
	double max_partial_ms = 0.0;
	double sum_partial_ms = 0.0;
	int n_partials = 0;
	char prompt_buf[512];
	jarvis_stt_res_t res;

	/* Stream incrementally every 1500 ms across the 55+ second utterance */
	for (int now_ms = 1800; now_ms < total_ms; now_ms += 1500) {
		int win_start_ms = (int)(st->window_start_f * st->frame_ms);
		if (now_ms - win_start_ms < 800)
			continue;
		size_t s0 = (size_t)win_start_ms * 16U;
		size_t s1 = (size_t)now_ms * 16U;
		if (s1 > total_n)
			s1 = total_n;
		int slice_ms = (int)((s1 - s0) / 16U);
		if (slice_ms > max_slice_ms)
			max_slice_ms = slice_ms;

		jarvis_stream_build_prompt(st,
		                           "The quick brown fox jumps over the lazy dog.",
		                           NULL, 18.0, 40, prompt_buf, sizeof(prompt_buf));

		jarvis_stt_req_t req;
		memset(&req, 0, sizeof(req));
		req.pcm = audio + s0;
		req.n_samples = (int)(s1 - s0);
		req.n_threads = 4;
		req.lang = "en";
		req.prompt = prompt_buf[0] ? prompt_buf : NULL;
		req.max_tok = 112;
		req.audio_ctx = jarvis_stt_compute_audio_ctx(req.n_samples, 0, 1);
		req.beam_size = 1;
		req.is_partial = 1;
		req.use_context = 1;
		req.out_words = words;
		req.max_words = JARVIS_STT_MAX_WORDS;

		double t0 = mono_ms();
		assert(jarvis_stt_transcribe(wctx, &req, &res) == 0);
		double dt = mono_ms() - t0;
		if (dt > max_partial_ms)
			max_partial_ms = dt;
		sum_partial_ms += dt;
		n_partials++;

		jarvis_stream_update_partial(st, words, res.n_words, slice_ms);
	}

	/* Final trailing window decode */
	int fin_start_ms = (int)(st->window_start_f * st->frame_ms);
	size_t fs0 = (size_t)fin_start_ms * 16U;
	if (fs0 < total_n) {
		int fin_slice_ms = (int)((total_n - fs0) / 16U);
		jarvis_stream_build_prompt(st, NULL, NULL, 18.0, 40,
		                           prompt_buf, sizeof(prompt_buf));
		jarvis_stt_req_t req;
		memset(&req, 0, sizeof(req));
		req.pcm = audio + fs0;
		req.n_samples = (int)(total_n - fs0);
		req.n_threads = 4;
		req.lang = "en";
		req.prompt = prompt_buf[0] ? prompt_buf : NULL;
		req.max_tok = 112;
		req.audio_ctx = jarvis_stt_compute_audio_ctx(req.n_samples, 0, 0);
		req.beam_size = 2;
		req.is_partial = 0;
		req.use_context = 1;
		req.out_words = words;
		req.max_words = JARVIS_STT_MAX_WORDS;
		assert(jarvis_stt_transcribe(wctx, &req, &res) == 0);
		jarvis_stream_finalize(st, words, res.n_words, fin_slice_ms);
	} else {
		jarvis_stream_finalize(st, NULL, 0, 0);
	}

	int n_country = count_word_ci(st->committed, "country");
	int n_americans = count_word_ci(st->committed, "americans");
	printf("  [55s real Whisper stream] dur=%.1fs partials=%d slides=%d max_slice=%dms "
	       "avg_lat=%.0fms max_lat=%.0fms words=%d country=%d americans=%d\n",
	       (double)total_ms / 1000.0, n_partials, st->window_slides, max_slice_ms,
	       n_partials > 0 ? sum_partial_ms / n_partials : 0.0, max_partial_ms,
	       st->committed_word_count, n_country, n_americans);
	if (!(n_country >= 8 && n_country <= 13 && n_americans >= 4 && n_americans <= 7))
		printf("  [COMMITTED]: %s\n", st->committed);

	/* Bounded sliding window: never grows past 30s single-window cap */
	assert(st->window_slides >= 3);
	assert(max_slice_ms <= st->window_max_ms + 2000);
	/* Continuity past 30s: 5 repetitions -> 10 "country" and 5 "Americans" expected */
	assert(n_country >= 8 && n_country <= 13);
	assert(n_americans >= 4 && n_americans <= 7);

	free(words);
	free(st);
	free(audio);
	jarvis_stt_close(wctx);
	printf("[PASS] test_long_form_real_whisper_stream\n");
}

int
main(void)
{
	test_lang_support();
	test_model_format_detection();
	test_hw_calibration_tiers();
	test_tier_and_locale_model_resolution();
	test_whisper_and_parakeet_fixtures();
	test_long_form_real_whisper_stream();
	printf("[PASS] All jarvis-stt-backend unit tests passed!\n");
	return 0;
}
