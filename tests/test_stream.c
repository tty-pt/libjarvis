#define _POSIX_C_SOURCE 200809L
#include "jarvis-stream.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
is_valid_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p < 0x80) {
			p++;
		} else if ((*p & 0xE0) == 0xC0) {
			if ((p[1] & 0xC0) != 0x80)
				return false;
			p += 2;
		} else if ((*p & 0xF0) == 0xE0) {
			if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
				return false;
			p += 3;
		} else if ((*p & 0xF8) == 0xF0) {
			if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
				return false;
			p += 4;
		} else {
			return false;
		}
	}
	return true;
}

static jarvis_stt_word_t
mkword(const char *txt, int t0, int t1)
{
	jarvis_stt_word_t w;
	memset(&w, 0, sizeof(w));
	snprintf(w.text, sizeof(w.text), "%s", txt);
	w.start_ms = t0;
	w.end_ms = t1;
	return w;
}

static void
test_word_agreement_utf8(void)
{
	assert(jarvis_stream_words_agree("hello", "Hello,"));
	assert(jarvis_stream_words_agree("world!", "world."));
	assert(jarvis_stream_words_agree("Olá,", "olá"));
	assert(jarvis_stream_words_agree("Cão?", "cão"));
	assert(jarvis_stream_words_agree("AÇÃO", "ação."));
	assert(!jarvis_stream_words_agree("project", "projects"));
	assert(!jarvis_stream_words_agree("", "hello"));
	assert(!jarvis_stream_words_agree(NULL, "hello"));
	printf("[PASS] test_word_agreement_utf8\n");
}

static void
test_local_agreement_and_margin(void)
{
	jarvis_stream_t st;
	jarvis_stream_init(&st, 100, 20);

	/* Pass 1 at 1600ms: no previous hypothesis -> nothing commits yet */
	jarvis_stt_word_t p1[] = {
		mkword("olá", 100, 450),
		mkword("hoje", 500, 900),
		mkword("estou", 950, 1450)
	};
	assert(jarvis_stream_update_partial(&st, p1, 3, 1600) == 0);
	assert(st.committed_len == 0);

	/* Pass 2 at 1800ms: "olá" (450ms <= 1200ms) & "hoje" (900ms <= 1200ms) agree and clear 600ms margin;
	 * "estou" ends at 1450ms > 1800-600=1200ms so it stays tentative.
	 * Also verify newer pass's punctuation/capitalization ("Olá,") is committed!
	 */
	jarvis_stt_word_t p2[] = {
		mkword("Olá,", 100, 450),
		mkword("hoje", 500, 900),
		mkword("estou", 950, 1450),
		mkword("bem", 1500, 1750)
	};
	int c2 = jarvis_stream_update_partial(&st, p2, 4, 1800);
	assert(c2 == 2);
	assert(strcmp(st.committed, "Olá, hoje") == 0);
	assert(st.cur_tail_count == 2);
	assert(strcmp(st.cur_tail[0].text, "estou") == 0);
	assert(strcmp(st.cur_tail[1].text, "bem") == 0);

	char preview[256];
	jarvis_stream_format_preview(&st, preview, sizeof(preview));
	assert(strcmp(preview, "Olá, hoje estou bem") == 0);

	/* Pass 3 at 2600ms: "estou" & "bem" now clear 2600-600=2000ms margin and agree */
	jarvis_stt_word_t p3[] = {
		mkword("Olá,", 100, 450),
		mkword("hoje", 500, 900),
		mkword("estou", 950, 1450),
		mkword("bem,", 1500, 1750),
		mkword("obrigado", 1900, 2400)
	};
	int c3 = jarvis_stream_update_partial(&st, p3, 5, 2600);
	assert(c3 == 2);
	assert(strcmp(st.committed, "Olá, hoje estou bem,") == 0);

	/* Final pass commits trailing word without margin */
	jarvis_stt_word_t fin[] = {
		mkword("Olá,", 100, 450),
		mkword("hoje", 500, 900),
		mkword("estou", 950, 1450),
		mkword("bem,", 1500, 1750),
		mkword("obrigado.", 1900, 2400)
	};
	int cf = jarvis_stream_finalize(&st, fin, 5, 2800);
	assert(cf == 1);
	assert(strcmp(st.committed, "Olá, hoje estou bem, obrigado.") == 0);
	assert(st.cur_tail_count == 0);

	printf("[PASS] test_local_agreement_and_margin\n");
}

static void
test_bpe_split_alignment(void)
{
	jarvis_stream_t st;
	jarvis_stream_init(&st, 0, 20);

	jarvis_stt_word_t p1[] = {
		mkword("please", 100, 400),
		mkword("set", 450, 700),
		mkword("up", 720, 900),
		mkword("the", 950, 1100),
		mkword("server", 1200, 1700)
	};
	assert(jarvis_stream_update_partial(&st, p1, 5, 2200) == 0);
	assert(jarvis_stream_update_partial(&st, p1, 5, 2200) == 4);
	assert(strcmp(st.committed, "please set up the") == 0);

	/* Next pass merges "set up" into "setup" earlier in the window, shifting index by -1,
	 * while adding "now". Alignment must anchor past "the" and NOT re-commit "the" or drop "server".
	 */
	jarvis_stt_word_t p2[] = {
		mkword("please", 100, 400),
		mkword("setup", 450, 900),
		mkword("the", 950, 1100),
		mkword("server", 1200, 1700),
		mkword("now", 1800, 2100)
	};
	int c2 = jarvis_stream_update_partial(&st, p2, 5, 2600);
	assert(c2 == 1);
	assert(strcmp(st.committed, "please set up the server") == 0);

	int cf = jarvis_stream_finalize(&st, p2, 5, 2600);
	assert(cf == 1);
	assert(strcmp(st.committed, "please set up the server now") == 0);

	printf("[PASS] test_bpe_split_alignment\n");
}

static void
test_90s_multi_slide_zero_loss_zero_dup(void)
{
	/* Simulate 95 seconds (190 distinct words @ 500ms/word = 95,000 ms) with:
	 * - partial decode every 1,200 ms
	 * - trailing live-edge word flickers on first sight, stabilizes once >= 400 ms old
	 * - simulated 1-word overlap echo right after every second window slide
	 * Assert: zero dropped words, zero duplicated words, bounded window <= 12s, >= 12 slides.
	 */
	enum { N_WORDS = 190 };
	static char truth_words[N_WORDS][32];
	static int truth_t0[N_WORDS];
	static int truth_t1[N_WORDS];
	static char expected[16384];
	expected[0] = '\0';

	for (int i = 0; i < N_WORDS; i++) {
		snprintf(truth_words[i], sizeof(truth_words[i]), "palavra%03d", i + 1);
		truth_t0[i] = i * 500 + 40;
		truth_t1[i] = i * 500 + 460;
		if (i > 0)
			strcat(expected, " ");
		strcat(expected, truth_words[i]);
	}

	jarvis_stream_t st;
	jarvis_stream_init(&st, 0, 20);

	int max_window_ms_seen = 0;
	int prev_slides = 0;

	for (int now_ms = 1200; now_ms <= 95000; now_ms += 1200) {
		int win_start_ms = (int)(st.window_start_f * st.frame_ms);
		int win_dur_ms = now_ms - win_start_ms;
		if (win_dur_ms > max_window_ms_seen)
			max_window_ms_seen = win_dur_ms;

		jarvis_stt_word_t hyp[JARVIS_STREAM_MAX_WORDS];
		int nh = 0;

		/* Inject a realistic 1-word or 2-word boundary echo right after slides */
		int just_slid = (st.window_slides > prev_slides);
		prev_slides = st.window_slides;

		int first_idx = -1;
		for (int i = 0; i < N_WORDS; i++) {
			if (truth_t1[i] > win_start_ms + 80 && truth_t0[i] < now_ms) {
				if (first_idx < 0)
					first_idx = i;
			}
		}

		if (just_slid && first_idx >= 2 && (st.window_slides % 2 == 0)) {
			/* 2-word boundary echo */
			hyp[nh++] = mkword(truth_words[first_idx - 2], 0, 120);
			hyp[nh++] = mkword(truth_words[first_idx - 1], 120, 260);
		} else if (just_slid && first_idx >= 1 && (st.window_slides % 2 == 1)) {
			/* 1-word boundary echo right at window start (start_ms <= 450) */
			hyp[nh++] = mkword(truth_words[first_idx - 1], 0, 180);
		}

		for (int i = (first_idx >= 0 ? first_idx : N_WORDS); i < N_WORDS && nh < JARVIS_STREAM_MAX_WORDS; i++) {
			if (truth_t0[i] >= now_ms)
				break;
			int rel_t0 = truth_t0[i] - win_start_ms;
			int rel_t1 = truth_t1[i] - win_start_ms;
			if (rel_t0 < 0) rel_t0 = 0;
			if (rel_t1 > win_dur_ms) rel_t1 = win_dur_ms;
			if (rel_t1 < rel_t0) rel_t1 = rel_t0;

			/* Flicker the very last word if it is still within 400ms of live edge */
			if (now_ms - truth_t1[i] < 400) {
				hyp[nh++] = mkword("ruido_parcial", rel_t0, rel_t1);
			} else {
				hyp[nh++] = mkword(truth_words[i], rel_t0, rel_t1);
			}
		}

		jarvis_stream_update_partial(&st, hyp, nh, win_dur_ms);
	}

	/* Authoritative final pass over remaining uncommitted window */
	int win_start_ms = (int)(st.window_start_f * st.frame_ms);
	int final_dur_ms = 95500 - win_start_ms;
	jarvis_stt_word_t fin[JARVIS_STREAM_MAX_WORDS];
	int nf = 0;
	for (int i = 0; i < N_WORDS && nf < JARVIS_STREAM_MAX_WORDS; i++) {
		if (truth_t1[i] <= win_start_ms + 80)
			continue;
		int rel_t0 = truth_t0[i] - win_start_ms;
		int rel_t1 = truth_t1[i] - win_start_ms;
		if (rel_t0 < 0) rel_t0 = 0;
		fin[nf++] = mkword(truth_words[i], rel_t0, rel_t1);
	}
	jarvis_stream_finalize(&st, fin, nf, final_dur_ms);

	assert(st.window_slides >= 12);
	assert(max_window_ms_seen <= 12500);
	assert(st.committed_word_count == N_WORDS);
	assert(strcmp(st.committed, expected) == 0);

	printf("  [95s stream] slides=%d max_win=%dms words=%d (exact 100%% match)\n",
	       st.window_slides, max_window_ms_seen, st.committed_word_count);
	printf("[PASS] test_90s_multi_slide_zero_loss_zero_dup\n");
}

static void
test_forced_slide_on_hesitant_speech(void)
{
	jarvis_stream_t st;
	jarvis_stream_init(&st, 0, 20);

	/* First word changes every single pass so LocalAgreement-2 never naturally commits,
	 * yet once window hits window_max_ms (12000ms), engine MUST force-slide so window stays bounded.
	 */
	jarvis_stt_word_t p1[] = {
		mkword("alpha1", 500, 2000),
		mkword("beta", 2500, 4000),
		mkword("gamma", 4500, 6000),
		mkword("delta", 11200, 11800)
	};
	jarvis_stream_update_partial(&st, p1, 4, 10000);
	assert(st.window_slides == 0);

	jarvis_stt_word_t p2[] = {
		mkword("alpha2", 500, 2000),
		mkword("beta", 2500, 4000),
		mkword("gamma", 4500, 6000),
		mkword("delta", 11200, 11900)
	};
	int committed = jarvis_stream_update_partial(&st, p2, 4, 12500);
	assert(committed >= 2);
	assert(st.window_slides == 1);
	assert(st.window_start_f > 0);
	assert(strstr(st.committed, "beta") != NULL);

	printf("[PASS] test_forced_slide_on_hesitant_speech\n");
}

static void
test_prompt_tail_utf8_and_snr_gates(void)
{
	jarvis_stream_t st;
	jarvis_stream_init(&st, 0, 20);

	const char *seed = "Hoje estive a planear o projeto e vou registar as notas com calma.";
	const char *ctx = "/home/user/site — libjarvis-phase5 | 12345 | a | b | JarvisStream | JarvisStream | Vulkan | PipeWire | Silero | Parakeet | Ontology | LocalAgreement | Token1 | Token2 | Token3 | Token4 | Token5 | Token6 | Token7 | Token8 | Token9 | Token10 | Token11 | Token12 | Token13";
	char prompt[256];

	/* Low SNR (< 10 dB) MUST disable both dictation_seed and active_context */
	assert(jarvis_stream_build_prompt(&st, seed, ctx, 6.5, 25, prompt, sizeof(prompt)) == 0);
	assert(prompt[0] == '\0');

	/* Tiny voiced_frames (< 10) MUST disable both */
	assert(jarvis_stream_build_prompt(&st, seed, ctx, 18.0, 5, prompt, sizeof(prompt)) == 0);
	assert(prompt[0] == '\0');

	/* Healthy SNR on window 0 uses dictation_seed + bounded sanitized context */
	size_t plen = jarvis_stream_build_prompt(&st, seed, ctx, 18.0, 25, prompt, sizeof(prompt));
	assert(plen > 0);
	assert(is_valid_utf8(prompt));
	assert(strncmp(prompt, seed, strlen(seed)) == 0);
	assert(strstr(prompt, "libjarvis-phase5") != NULL);
	assert(strstr(prompt, "12345") == NULL); /* purely numeric dropped */

	/* Test strict <= 16 token cap on active_context alone */
	char ctx_only[512];
	assert(jarvis_stream_build_prompt(&st, NULL, ctx, 18.0, 25, ctx_only, sizeof(ctx_only)) > 0);
	int n_toks = 0;
	for (const char *p = ctx_only; *p; ) {
		while (*p == ' ') p++;
		if (!*p) break;
		n_toks++;
		while (*p && *p != ' ') p++;
	}
	assert(n_toks <= JARVIS_STREAM_MAX_CTX_TOKENS);
	assert(n_toks >= 10);

	/* Mid-window commits BEFORE a slide must NOT populate prompt_tail (prevents
	 * Whisper from skipping words that are still inside the active audio window). */
	jarvis_stt_word_t early[3] = {
		mkword("olá", 100, 400),
		mkword("mundo", 450, 900),
		mkword("agora", 950, 1400)
	};
	assert(jarvis_stream_update_partial(&st, early, 3, 2500) == 0);
	assert(jarvis_stream_update_partial(&st, early, 3, 2500) == 3);
	assert(st.window_slides == 0);
	assert(st.prompt_tail[0] == '\0');

	/* Reset for clean UTF-8 prompt_tail replacement test on slide/finalize */
	jarvis_stream_init(&st, 0, 20);
	jarvis_stt_word_t utf8_words[40];
	for (int i = 0; i < 40; i++) {
		char wbuf[64];
		snprintf(wbuf, sizeof(wbuf), "ação_coração_%02d_日本語", i + 1);
		utf8_words[i] = mkword(wbuf, i * 100, i * 100 + 80);
	}
	jarvis_stream_finalize(&st, utf8_words, 40, 4500);
	assert(st.prompt_tail[0] != '\0');
	assert(is_valid_utf8(st.prompt_tail));
	assert(strstr(st.prompt_tail, "ação_coração_40_日本語") != NULL);

	assert(jarvis_stream_build_prompt(&st, seed, NULL, 18.0, 25, prompt, sizeof(prompt)) > 0);
	assert(is_valid_utf8(prompt));
	assert(strstr(prompt, "planear") == NULL); /* seed replaced by user's own prompt_tail */
	assert(strstr(prompt, "ação_coração_40_日本語") != NULL);

	printf("[PASS] test_prompt_tail_utf8_and_snr_gates\n");
}

static void
test_preview_viewport_and_fallback_words(void)
{
	jarvis_stt_word_t w[16];
	int nw = jarvis_stream_words_from_text("Um dois três quatro", 2000, w, 16);
	assert(nw == 4);
	assert(strcmp(w[0].text, "Um") == 0);
	assert(strcmp(w[3].text, "quatro") == 0);
	assert(w[0].start_ms == 0);
	assert(w[3].end_ms == 2000);
	for (int i = 0; i < nw; i++) {
		assert(w[i].end_ms >= w[i].start_ms);
		if (i > 0)
			assert(w[i].start_ms >= w[i - 1].end_ms);
	}

	/* Long committed text truncates cleanly with "... " trailing UTF-8 viewport */
	jarvis_stream_t st;
	jarvis_stream_init(&st, 0, 20);
	jarvis_stt_word_t long_w[30];
	for (int i = 0; i < 30; i++) {
		char b[32];
		snprintf(b, sizeof(b), "canção%02d", i + 1);
		long_w[i] = mkword(b, i * 100, i * 100 + 80);
	}
	jarvis_stream_finalize(&st, long_w, 30, 3500);

	char small_osd[64];
	size_t n = jarvis_stream_format_preview(&st, small_osd, sizeof(small_osd));
	assert(n > 0 && n < sizeof(small_osd));
	assert(strncmp(small_osd, "... ", 4) == 0);
	assert(is_valid_utf8(small_osd));
	assert(strstr(small_osd, "canção30") != NULL);

	printf("[PASS] test_preview_viewport_and_fallback_words\n");
}

int
main(void)
{
	test_word_agreement_utf8();
	test_local_agreement_and_margin();
	test_bpe_split_alignment();
	test_90s_multi_slide_zero_loss_zero_dup();
	test_forced_slide_on_hesitant_speech();
	test_prompt_tail_utf8_and_snr_gates();
	test_preview_viewport_and_fallback_words();
	printf("[PASS] All jarvis-stream unit tests passed!\n");
	return 0;
}
