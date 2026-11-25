#include "jarvis-stream.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool
is_edge_punct(unsigned char c)
{
	switch (c) {
	case '.': case ',': case '!': case '?': case ';': case ':':
	case '"': case '\'': case '(': case ')': case '[': case ']':
	case '{': case '}': case '-': case '`':
		return true;
	default:
		return false;
	}
}

static size_t
normalize_word_core(const char *s, char *out, size_t out_sz)
{
	if (!out || out_sz == 0)
		return 0;
	out[0] = '\0';
	if (!s || !*s)
		return 0;

	const unsigned char *p = (const unsigned char *)s;
	const unsigned char *end = p + strlen(s);

	while (p < end && (isspace(*p) || is_edge_punct(*p)))
		p++;
	while (end > p && (isspace(end[-1]) || is_edge_punct(end[-1])))
		end--;

	if (p == end) {
		/* Pure punctuation token: compare trimmed raw bytes */
		p = (const unsigned char *)s;
		end = p + strlen(s);
		while (p < end && isspace(*p))
			p++;
		while (end > p && isspace(end[-1]))
			end--;
	}

	size_t w = 0;
	while (p < end && w + 1 < out_sz) {
		unsigned char c = *p;
		if (c == 0xC3 && p + 1 < end && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {
			if (w + 2 >= out_sz)
				break;
			out[w++] = (char)0xC3;
			out[w++] = (char)(p[1] + 0x20);
			p += 2;
		} else if (c >= 'A' && c <= 'Z') {
			out[w++] = (char)(c - 'A' + 'a');
			p++;
		} else {
			out[w++] = (char)c;
			p++;
		}
	}
	out[w] = '\0';
	return w;
}

bool
jarvis_stream_words_agree(const char *a, const char *b)
{
	if (!a || !b || !*a || !*b)
		return false;
	char na[64], nb[64];
	size_t la = normalize_word_core(a, na, sizeof(na));
	size_t lb = normalize_word_core(b, nb, sizeof(nb));
	if (la == 0 || la != lb)
		return false;
	return memcmp(na, nb, la) == 0;
}

int
jarvis_stream_words_from_text(const char *text, int duration_ms,
                              jarvis_stt_word_t *out, int max_words)
{
	if (!text || !*text || !out || max_words <= 0)
		return 0;
	if (duration_ms < 0)
		duration_ms = 0;

	int n = 0;
	size_t total_chars = 0;
	const char *p = text;
	while (*p && n < max_words) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		const char *ws = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		size_t wlen = (size_t)(p - ws);
		size_t copy = (wlen < sizeof(out[n].text) - 1) ? wlen : (sizeof(out[n].text) - 1);
		memcpy(out[n].text, ws, copy);
		out[n].text[copy] = '\0';
		out[n].start_ms = 0;
		out[n].end_ms = 0;
		total_chars += (wlen > 0 ? wlen : 1);
		n++;
	}
	if (n == 0)
		return 0;

	size_t acc = 0;
	for (int i = 0; i < n; i++) {
		size_t wl = strlen(out[i].text);
		if (wl == 0)
			wl = 1;
		int t0 = (total_chars > 0) ? (int)(((int64_t)duration_ms * (int64_t)acc) / (int64_t)total_chars) : 0;
		acc += wl;
		int t1 = (total_chars > 0) ? (int)(((int64_t)duration_ms * (int64_t)acc) / (int64_t)total_chars) : duration_ms;
		if (t1 < t0)
			t1 = t0;
		out[i].start_ms = t0;
		out[i].end_ms = t1;
	}
	return n;
}

static void
utf8_safe_copy_tail(const char *src, size_t src_len, char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0)
		return;
	dst[0] = '\0';
	if (!src || src_len == 0)
		return;

	size_t max_copy = dst_sz - 1;
	if (src_len <= max_copy) {
		memcpy(dst, src, src_len);
		dst[src_len] = '\0';
		return;
	}

	const char *start = src + (src_len - max_copy);
	const char *end = src + src_len;
	/* Advance past any UTF-8 continuation bytes (10xxxxxx) */
	while (start < end && (((unsigned char)*start & 0xC0) == 0x80))
		start++;
	/* Prefer advancing to the next word boundary so we don't slice mid-word */
	const char *sp = start;
	while (sp < end && !isspace((unsigned char)*sp))
		sp++;
	if (sp < end) {
		while (sp < end && isspace((unsigned char)*sp))
			sp++;
		if (sp < end)
			start = sp;
	}
	size_t len = (size_t)(end - start);
	if (len > max_copy)
		len = max_copy;
	memcpy(dst, start, len);
	dst[len] = '\0';
}

static void
refresh_prompt_tail(jarvis_stream_t *st)
{
	if (!st || st->committed_len == 0) {
		if (st)
			st->prompt_tail[0] = '\0';
		return;
	}

	/* Walk backward up to JARVIS_STREAM_PROMPT_WORDS words in st->committed */
	const char *base = st->committed;
	const char *p = base + st->committed_len;
	int words = 0;
	while (p > base) {
		while (p > base && isspace((unsigned char)p[-1]))
			p--;
		if (p == base)
			break;
		while (p > base && !isspace((unsigned char)p[-1]))
			p--;
		words++;
		if (words >= JARVIS_STREAM_PROMPT_WORDS)
			break;
	}
	while (p < base + st->committed_len && isspace((unsigned char)*p))
		p++;

	size_t span = (size_t)((base + st->committed_len) - p);
	utf8_safe_copy_tail(p, span, st->prompt_tail, sizeof(st->prompt_tail));
}

static void
append_committed_word(jarvis_stream_t *st, const jarvis_stt_word_t *w)
{
	if (!st || !w || !w->text[0])
		return;

	size_t wlen = strlen(w->text);
	size_t need = wlen + (st->committed_len > 0 ? 1 : 0);
	if (st->committed_len + need + 1 < sizeof(st->committed)) {
		if (st->committed_len > 0)
			st->committed[st->committed_len++] = ' ';
		memcpy(st->committed + st->committed_len, w->text, wlen);
		st->committed_len += wlen;
		st->committed[st->committed_len] = '\0';
		st->committed_word_count++;
	}

	if (st->win_committed_n < JARVIS_STREAM_MAX_WORDS)
		st->win_committed[st->win_committed_n++] = *w;
	if (w->end_ms > st->win_committed_end_ms)
		st->win_committed_end_ms = w->end_ms;

	int64_t end_f = st->window_start_f + (int64_t)(w->end_ms / st->frame_ms);
	if (end_f > st->committed_end_f)
		st->committed_end_f = end_f;
}

static int
extract_committed_tail_words(const jarvis_stream_t *st, char out[][64], int max_n)
{
	if (!st || st->committed_len == 0 || max_n <= 0)
		return 0;

	const char *base = st->committed;
	const char *p = base + st->committed_len;
	const char *starts[16];
	size_t lens[16];
	int cap = (max_n < 16) ? max_n : 16;
	int found = 0;

	while (p > base && found < cap) {
		while (p > base && isspace((unsigned char)p[-1]))
			p--;
		if (p == base)
			break;
		const char *we = p;
		while (p > base && !isspace((unsigned char)p[-1]))
			p--;
		starts[found] = p;
		lens[found] = (size_t)(we - p);
		found++;
	}

	/* Reverse into chronological order */
	for (int i = 0; i < found; i++) {
		int src = found - 1 - i;
		size_t copy = (lens[src] < 63) ? lens[src] : 63;
		memcpy(out[i], starts[src], copy);
		out[i][copy] = '\0';
	}
	return found;
}

static int
align_uncommitted_start(const jarvis_stream_t *st,
                        const jarvis_stt_word_t *words, int n_words)
{
	if (!st || !words || n_words <= 0)
		return 0;

	/* Case 1: words already committed inside the current window_start_f */
	if (st->win_committed_n > 0) {
		int wc = st->win_committed_n;
		int anchor = (wc < 3) ? wc : 3;

		/* Fast exact position check at index wc - anchor */
		if (wc <= n_words) {
			bool match = true;
			for (int k = 0; k < anchor; k++) {
				if (!jarvis_stream_words_agree(st->win_committed[wc - anchor + k].text,
				                               words[wc - anchor + k].text)) {
					match = false;
					break;
				}
			}
			if (match)
				return wc;
		}

		/* Tolerant search within +-3 words for BPE split/merge shifts */
		int lo = wc - 3;
		if (lo < anchor)
			lo = anchor;
		int hi = wc + 3;
		if (hi > n_words)
			hi = n_words;
		for (int end_pos = lo; end_pos <= hi; end_pos++) {
			bool match = true;
			for (int k = 0; k < anchor; k++) {
				if (!jarvis_stream_words_agree(st->win_committed[wc - anchor + k].text,
				                               words[end_pos - anchor + k].text)) {
					match = false;
					break;
				}
			}
			if (match)
				return end_pos;
		}

		/* Timestamp fallback if wording mutated earlier in window */
		int idx = 0;
		int limit = (wc + 1 < n_words) ? (wc + 1) : n_words;
		while (idx < limit && words[idx].end_ms <= st->win_committed_end_ms)
			idx++;
		return idx;
	}

	/* Case 2: freshly slid window — strip boundary echo against st->committed tail */
	if (st->window_slides > 0 && st->committed_word_count > 0) {
		char tail[8][64];
		int tn = extract_committed_tail_words(st, tail, 8);
		int max_k = tn;
		if (n_words < max_k)
			max_k = n_words;
		if (max_k > 6)
			max_k = 6;
		for (int k = max_k; k >= 1; k--) {
			bool match = true;
			for (int j = 0; j < k; j++) {
				if (!jarvis_stream_words_agree(tail[tn - k + j], words[j].text)) {
					match = false;
					break;
				}
			}
			if (match) {
				if (k >= 2 || words[0].start_ms <= 450)
					return k;
			}
		}
	}

	return 0;
}

static void
slide_window_by_ms(jarvis_stream_t *st, int slide_ms)
{
	if (!st || slide_ms <= 0)
		return;
	int64_t slide_f = (int64_t)(slide_ms / st->frame_ms);
	if (slide_f <= 0)
		return;
	int actual_ms = (int)(slide_f * st->frame_ms);

	st->window_start_f += slide_f;
	if (st->window_start_f > st->committed_end_f)
		st->committed_end_f = st->window_start_f;
	st->win_committed_n = 0;
	st->win_committed_end_ms = 0;
	st->window_slides++;
	/* prompt_tail must strictly contain words committed BEFORE window_start_f;
	 * refreshing it mid-window causes Whisper to treat words still inside the
	 * current audio slice as already-spoken context and skip them. */
	refresh_prompt_tail(st);

	for (int i = 0; i < st->prev_count; i++) {
		st->prev_words[i].start_ms -= actual_ms;
		st->prev_words[i].end_ms   -= actual_ms;
		if (st->prev_words[i].start_ms < 0)
			st->prev_words[i].start_ms = 0;
		if (st->prev_words[i].end_ms < st->prev_words[i].start_ms)
			st->prev_words[i].end_ms = st->prev_words[i].start_ms;
	}
	for (int i = 0; i < st->cur_tail_count; i++) {
		st->cur_tail[i].start_ms -= actual_ms;
		st->cur_tail[i].end_ms   -= actual_ms;
		if (st->cur_tail[i].start_ms < 0)
			st->cur_tail[i].start_ms = 0;
		if (st->cur_tail[i].end_ms < st->cur_tail[i].start_ms)
			st->cur_tail[i].end_ms = st->cur_tail[i].start_ms;
	}
}

void
jarvis_stream_init(jarvis_stream_t *st, int64_t start_frame, int frame_ms)
{
	if (!st)
		return;
	memset(st, 0, sizeof(*st));
	st->window_start_f  = start_frame;
	st->committed_end_f = start_frame;
	st->frame_ms        = (frame_ms > 0) ? frame_ms : 20;
	st->margin_ms       = JARVIS_STREAM_MARGIN_MS;
	st->slide_min_ms    = JARVIS_STREAM_SLIDE_MIN_MS;
	st->window_max_ms   = JARVIS_STREAM_WINDOW_MAX_MS;
}

int
jarvis_stream_update_partial(jarvis_stream_t *st,
                             const jarvis_stt_word_t *words, int n_words,
                             int window_duration_ms)
{
	if (!st || !words || n_words <= 0)
		return 0;
	if (n_words > JARVIS_STREAM_MAX_WORDS)
		n_words = JARVIS_STREAM_MAX_WORDS;
	if (window_duration_ms < 0)
		window_duration_ms = 0;

	int base = align_uncommitted_start(st, words, n_words);
	const jarvis_stt_word_t *cur = words + base;
	int cur_n = n_words - base;
	if (cur_n < 0)
		cur_n = 0;

	/* LocalAgreement-2: longest agreeing prefix against previous uncommitted suffix */
	int agree = 0;
	int lim = (cur_n < st->prev_count) ? cur_n : st->prev_count;
	while (agree < lim &&
	       jarvis_stream_words_agree(cur[agree].text, st->prev_words[agree].text)) {
		agree++;
	}

	/* Enforce trailing stability margin behind live audio edge */
	int safe_cut_ms = window_duration_ms - st->margin_ms;
	while (agree > 0 && cur[agree - 1].end_ms > safe_cut_ms)
		agree--;

	int newly_committed = 0;
	for (int i = 0; i < agree; i++) {
		append_committed_word(st, &cur[i]);
		newly_committed++;
	}

	/* Remaining uncommitted words become the next prev_words & live tentative tail */
	int rem = cur_n - agree;
	if (rem > JARVIS_STREAM_MAX_WORDS)
		rem = JARVIS_STREAM_MAX_WORDS;
	if (rem > 0) {
		memcpy(st->prev_words, cur + agree, (size_t)rem * sizeof(jarvis_stt_word_t));
		memcpy(st->cur_tail, cur + agree, (size_t)rem * sizeof(jarvis_stt_word_t));
	}
	st->prev_count = rem;
	st->cur_tail_count = rem;

	/* Slide window forward once enough committed audio accumulated, or force-slide at cap */
	if (st->win_committed_n > 0 && st->win_committed_end_ms >= st->slide_min_ms) {
		slide_window_by_ms(st, st->win_committed_end_ms);
	} else if (window_duration_ms >= st->window_max_ms) {
		if (st->win_committed_n > 0 && st->win_committed_end_ms >= 1000) {
			slide_window_by_ms(st, st->win_committed_end_ms);
		} else if (st->prev_count > 0) {
			/* Force-commit safe prefix so decode window never stalls past window_max_ms */
			int force_n = 0;
			while (force_n < st->prev_count && st->prev_words[force_n].end_ms <= safe_cut_ms)
				force_n++;
			if (force_n == 0 && st->prev_count >= 2)
				force_n = st->prev_count / 2;
			if (force_n > 0) {
				for (int i = 0; i < force_n; i++) {
					append_committed_word(st, &st->prev_words[i]);
					newly_committed++;
				}
				int left = st->prev_count - force_n;
				if (left > 0) {
					memmove(st->prev_words, st->prev_words + force_n,
					        (size_t)left * sizeof(jarvis_stt_word_t));
					memmove(st->cur_tail, st->cur_tail + force_n,
					        (size_t)left * sizeof(jarvis_stt_word_t));
				}
				st->prev_count = left;
				st->cur_tail_count = left;
				if (st->win_committed_end_ms > 0)
					slide_window_by_ms(st, st->win_committed_end_ms);
			}
		}
	}

	return newly_committed;
}

int
jarvis_stream_finalize(jarvis_stream_t *st,
                       const jarvis_stt_word_t *words, int n_words,
                       int window_duration_ms)
{
	(void)window_duration_ms;
	if (!st)
		return 0;

	int newly_committed = 0;
	if (words && n_words > 0) {
		if (n_words > JARVIS_STREAM_MAX_WORDS)
			n_words = JARVIS_STREAM_MAX_WORDS;
		int base = align_uncommitted_start(st, words, n_words);
		for (int i = base; i < n_words; i++) {
			append_committed_word(st, &words[i]);
			newly_committed++;
		}
	} else if (st->cur_tail_count > 0) {
		/* If final pass was empty/aborted, preserve already-heard tentative tail */
		for (int i = 0; i < st->cur_tail_count; i++) {
			append_committed_word(st, &st->cur_tail[i]);
			newly_committed++;
		}
	}

	st->prev_count = 0;
	st->cur_tail_count = 0;
	if (newly_committed > 0)
		refresh_prompt_tail(st);
	return newly_committed;
}

size_t
jarvis_stream_format_preview(const jarvis_stream_t *st, char *out, size_t out_sz)
{
	if (!out || out_sz == 0)
		return 0;
	out[0] = '\0';
	if (!st)
		return 0;

	/* Build tail of committed + full cur_tail into a scratch buffer */
	char tail_buf[4096];
	size_t tlen = 0;
	tail_buf[0] = '\0';
	for (int i = 0; i < st->cur_tail_count; i++) {
		const char *w = st->cur_tail[i].text;
		size_t wl = strlen(w);
		if (wl == 0)
			continue;
		if (tlen + wl + 2 >= sizeof(tail_buf))
			break;
		if (tlen > 0)
			tail_buf[tlen++] = ' ';
		memcpy(tail_buf + tlen, w, wl);
		tlen += wl;
		tail_buf[tlen] = '\0';
	}

	size_t total = st->committed_len + (st->committed_len > 0 && tlen > 0 ? 1 : 0) + tlen;
	if (total == 0)
		return 0;

	if (total + 1 <= out_sz) {
		size_t pos = 0;
		if (st->committed_len > 0) {
			memcpy(out, st->committed, st->committed_len);
			pos += st->committed_len;
		}
		if (pos > 0 && tlen > 0)
			out[pos++] = ' ';
		if (tlen > 0) {
			memcpy(out + pos, tail_buf, tlen);
			pos += tlen;
		}
		out[pos] = '\0';
		return pos;
	}

	/* Long transcript: show UTF-8-safe trailing viewport prefixed with "..." */
	const char *prefix = "... ";
	size_t plen = (out_sz > 8) ? strlen(prefix) : 0;
	size_t avail = out_sz - 1 - plen;
	if (plen > 0)
		memcpy(out, prefix, plen);

	if (tlen >= avail) {
		utf8_safe_copy_tail(tail_buf, tlen, out + plen, avail + 1);
		return strlen(out);
	}

	size_t comm_budget = avail - tlen - (tlen > 0 ? 1 : 0);
	char comm_tail[4096];
	if (comm_budget >= sizeof(comm_tail))
		comm_budget = sizeof(comm_tail) - 1;
	utf8_safe_copy_tail(st->committed, st->committed_len, comm_tail, comm_budget + 1);
	size_t clen = strlen(comm_tail);

	size_t pos = plen;
	if (clen > 0) {
		memcpy(out + pos, comm_tail, clen);
		pos += clen;
	}
	if (clen > 0 && tlen > 0 && pos + 1 < out_sz)
		out[pos++] = ' ';
	if (tlen > 0 && pos < out_sz - 1) {
		size_t copy = tlen;
		if (copy > out_sz - 1 - pos)
			copy = out_sz - 1 - pos;
		memcpy(out + pos, tail_buf, copy);
		pos += copy;
	}
	out[pos] = '\0';
	return pos;
}

static bool
is_ctx_delim(unsigned char c)
{
	if (isspace(c) || c < 0x20)
		return true;
	switch (c) {
	case '/': case '\\': case '|': case ':': case ';':
	case ',': case '(': case ')': case '[': case ']':
	case '{': case '}': case '"': case '\'': case '<': case '>':
	case '=': case '*': case '?':
		return true;
	default:
		return false;
	}
}

static bool
token_is_numeric_only(const char *tok, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)tok[i];
		if (!(isdigit(c) || c == '.' || c == '-' || c == '_'))
			return false;
	}
	return true;
}

size_t
jarvis_stream_build_prompt(const jarvis_stream_t *st,
                           const char *dictation_seed,
                           const char *active_context,
                           double snr_db,
                           int voiced_frames,
                           char *out, size_t out_sz)
{
	if (!out || out_sz == 0)
		return 0;
	out[0] = '\0';

	bool snr_healthy = (snr_db >= 10.0 && voiced_frames >= 10);
	size_t pos = 0;

	/* Window 1+: user's own committed speech carries continuity across boundaries */
	if (st && st->prompt_tail[0] != '\0') {
		utf8_safe_copy_tail(st->prompt_tail, strlen(st->prompt_tail), out, out_sz);
		pos = strlen(out);
	} else if (snr_healthy && dictation_seed && *dictation_seed) {
		/* Window 0 only: natural 1-sentence regional dialect prose (never low-SNR) */
		size_t slen = strlen(dictation_seed);
		if (slen >= out_sz) {
			utf8_safe_copy_tail(dictation_seed, slen, out, out_sz);
		} else {
			memcpy(out, dictation_seed, slen + 1);
		}
		pos = strlen(out);
	}

	/* Bounded dynamic active-context biasing (<= 16 sanitized tokens, healthy SNR only) */
	if (snr_healthy && active_context && *active_context && pos + 4 < out_sz) {
		char seen[JARVIS_STREAM_MAX_CTX_TOKENS][48];
		int n_seen = 0;
		const unsigned char *p = (const unsigned char *)active_context;

		while (*p && n_seen < JARVIS_STREAM_MAX_CTX_TOKENS) {
			while (*p && is_ctx_delim(*p))
				p++;
			if (!*p)
				break;
			const unsigned char *ts = p;
			while (*p && !is_ctx_delim(*p))
				p++;
			size_t tlen = (size_t)(p - ts);
			/* Trim leading/trailing dashes or dots */
			while (tlen > 0 && (*ts == '-' || *ts == '.')) {
				ts++;
				tlen--;
			}
			while (tlen > 0 && (ts[tlen - 1] == '-' || ts[tlen - 1] == '.'))
				tlen--;
			if (tlen < 3 || tlen >= sizeof(seen[0]))
				continue;
			if (token_is_numeric_only((const char *)ts, tlen))
				continue;

			char tok[48];
			memcpy(tok, ts, tlen);
			tok[tlen] = '\0';

			bool dup = false;
			for (int k = 0; k < n_seen; k++) {
				if (jarvis_stream_words_agree(seen[k], tok)) {
					dup = true;
					break;
				}
			}
			if (dup)
				continue;

			size_t need = tlen + (pos > 0 ? 1 : 0);
			if (pos + need + 1 >= out_sz)
				break;
			if (pos > 0)
				out[pos++] = ' ';
			memcpy(out + pos, tok, tlen);
			pos += tlen;
			out[pos] = '\0';

			snprintf(seen[n_seen++], sizeof(seen[0]), "%s", tok);
		}
	}

	return pos;
}
