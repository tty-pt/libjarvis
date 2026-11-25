#ifndef JARVIS_STREAM_H
#define JARVIS_STREAM_H

#include "jarvis-stt-backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JARVIS_STREAM_COMMITTED_CAP  65536
#define JARVIS_STREAM_PROMPT_CAP     256
#define JARVIS_STREAM_MAX_WORDS      JARVIS_STT_MAX_WORDS
#define JARVIS_STREAM_MARGIN_MS      600   /* stability margin behind live audio edge */
#define JARVIS_STREAM_SLIDE_MIN_MS   4500  /* slide window once >= 4.5s committed */
#define JARVIS_STREAM_WINDOW_MAX_MS  12000 /* force-slide cap so decode stays bounded O(1) */
#define JARVIS_STREAM_PROMPT_WORDS   30    /* last ~30 committed words for next window */
#define JARVIS_STREAM_MAX_CTX_TOKENS 16    /* hard cap on active-context bias tokens */

typedef struct {
	int64_t           window_start_f;     /* absolute frame where active decode window starts */
	int64_t           committed_end_f;    /* absolute frame of last committed word end */
	int               frame_ms;           /* ms per audio frame (default 20) */
	int               margin_ms;          /* stability margin behind live audio edge (default 600) */
	int               slide_min_ms;       /* committed span inside window before sliding */
	int               window_max_ms;      /* max window span before force-sliding */

	jarvis_stt_word_t win_committed[JARVIS_STREAM_MAX_WORDS];
	int               win_committed_n;
	int               win_committed_end_ms;

	jarvis_stt_word_t prev_words[JARVIS_STREAM_MAX_WORDS];
	int               prev_count;

	jarvis_stt_word_t cur_tail[JARVIS_STREAM_MAX_WORDS];
	int               cur_tail_count;

	char              committed[JARVIS_STREAM_COMMITTED_CAP];
	size_t            committed_len;
	int               committed_word_count;

	char              prompt_tail[JARVIS_STREAM_PROMPT_CAP];
	int               window_slides;
} jarvis_stream_t;

void   jarvis_stream_init(jarvis_stream_t *st, int64_t start_frame, int frame_ms);

/* Case/punctuation-tolerant UTF-8 word comparison for LocalAgreement-2 */
bool   jarvis_stream_words_agree(const char *a, const char *b);

/* Fallback helper: synthesize monotonically spaced words from plain text */
int    jarvis_stream_words_from_text(const char *text, int duration_ms,
                                     jarvis_stt_word_t *out, int max_words);

/* Ingest a partial hypothesis covering [window_start_f .. window_start_f + duration_ms].
 * Returns number of newly committed words (0 if none).
 */
int    jarvis_stream_update_partial(jarvis_stream_t *st,
                                    const jarvis_stt_word_t *words, int n_words,
                                    int window_duration_ms);

/* Ingest the authoritative final decode for the remaining uncommitted window tail.
 * Commits all remaining words (no trailing margin) and clears tentative tail.
 * Returns number of newly committed words.
 */
int    jarvis_stream_finalize(jarvis_stream_t *st,
                              const jarvis_stt_word_t *words, int n_words,
                              int window_duration_ms);

/* Format live preview ("committed + tentative") into out (UTF-8 safe trailing window if long). */
size_t jarvis_stream_format_preview(const jarvis_stream_t *st, char *out, size_t out_sz);

/* Build safe Whisper initial_prompt for the next streaming window:
 *  - Uses st->prompt_tail once committed words exist (replaces dictation_seed).
 *  - Uses dictation_seed ONLY on window 0 when snr_db >= 10.0 and voiced_frames >= 10.
 *  - Optionally appends <= 16 sanitized tokens from active_context when SNR is healthy.
 */
size_t jarvis_stream_build_prompt(const jarvis_stream_t *st,
                                  const char *dictation_seed,
                                  const char *active_context,
                                  double snr_db,
                                  int voiced_frames,
                                  char *out, size_t out_sz);

#endif /* JARVIS_STREAM_H */
