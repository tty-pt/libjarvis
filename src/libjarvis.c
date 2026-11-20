#include "jarvis-internal.h"

#ifndef __wasm__
#include <ttypt/mm.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(_WIN32)
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#endif

/* Xylem Hook Declarations are provided by <ttypt/jarvis.h> via XY_DECL */

static double now_ms(void)
{
#ifndef __wasm__
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
#else
	return 0.0;
#endif
}

static void ensure_parent_dir(const char *path)
{
#ifndef __wasm__
	if (!path || !*path) return;
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	char *slash = strrchr(tmp, '/');
	if (!slash) return;
	*slash = '\0';
	if (!tmp[0]) return;
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
#if defined(_WIN32)
			_mkdir(tmp);
#else
			mkdir(tmp, 0755);
#endif
			*p = '/';
		}
	}
#if defined(_WIN32)
	_mkdir(tmp);
#else
	mkdir(tmp, 0755);
#endif
#else
	(void)path;
#endif
}

/* ---------------------------------------------------------------------------
 * Streaming TTS Answer Prefix & Clause Extractor
 * ------------------------------------------------------------------------- */

static bool extract_streaming_answer_or_reply(
	const char *raw_json_fragment,
	char       *out_answer,
	size_t      out_sz,
	bool       *out_closed
)
{
	if (out_closed) *out_closed = false;
	if (!out_answer || out_sz == 0) return false;
	out_answer[0] = '\0';
	if (!raw_json_fragment || !*raw_json_fragment) return false;

	const char *ans_key = strstr(raw_json_fragment, "\"answer\"");
	size_t key_len = 8;
	if (!ans_key) {
		ans_key = strstr(raw_json_fragment, "\"reply\"");
		key_len = 7;
	}
	if (!ans_key) return false;

	const char *p = ans_key + key_len;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p != ':') return false;
	p++;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p != '"') return false;
	p++;

	size_t w = 0;
	while (*p && w + 1 < out_sz) {
		char ch = *p;
		if (ch == '"') {
			if (out_closed) *out_closed = true;
			break;
		}
		if (ch == '\\') {
			if (!p[1]) break;
			char nxt = p[1];
			if (nxt == '"' || nxt == '\\' || nxt == '/') {
				out_answer[w++] = nxt;
				p += 2;
			} else if (nxt == 'n' || nxt == 't') {
				out_answer[w++] = ' ';
				p += 2;
			} else if (nxt == 'r') {
				p += 2;
			} else if (nxt == 'u') {
				if (!p[2] || !p[3] || !p[4] || !p[5]) break;
				unsigned int cp = 0;
				for (int i = 2; i < 6; i++) {
					char hc = p[i];
					cp <<= 4;
					if (hc >= '0' && hc <= '9') cp |= (unsigned int)(hc - '0');
					else if (hc >= 'a' && hc <= 'f') cp |= (unsigned int)(hc - 'a' + 10);
					else if (hc >= 'A' && hc <= 'F') cp |= (unsigned int)(hc - 'A' + 10);
				}
				if (cp < 0x80) {
					out_answer[w++] = (char)cp;
				} else if (cp < 0x800 && w + 2 < out_sz) {
					out_answer[w++] = (char)(0xC0U | (cp >> 6));
					out_answer[w++] = (char)(0x80U | (cp & 0x3FU));
				} else if (w + 3 < out_sz) {
					out_answer[w++] = (char)(0xE0U | (cp >> 12));
					out_answer[w++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
					out_answer[w++] = (char)(0x80U | (cp & 0x3FU));
				}
				p += 6;
			} else {
				out_answer[w++] = nxt;
				p += 2;
			}
		} else {
			out_answer[w++] = ch;
			p++;
		}
	}
	out_answer[w] = '\0';
	return w > 0;
}

bool jarvis_extract_streaming_answer_prefix(
	const char *raw_json_fragment,
	char       *out_answer,
	size_t      out_sz
)
{
	return extract_streaming_answer_or_reply(raw_json_fragment, out_answer, out_sz, NULL);
}

static bool is_prep_conj_at(const char *s, size_t *out_word_len)
{
	const jarvis_ontology_t *ont = jarvis_ontology_get_active();
	char word[JARVIS_SHORT_LEN];
	size_t wl = 0;
	while (s[wl] && !isspace((unsigned char)s[wl]) && !strchr(",.!?;:", s[wl]) && wl + 1 < sizeof(word)) {
		word[wl] = s[wl];
		wl++;
	}
	word[wl] = '\0';
	if (wl == 0 || !isspace((unsigned char)s[wl])) return false;

	char w_clean[JARVIS_SHORT_LEN];
	jarvis_clean_phrase(word, w_clean, sizeof(w_clean));
	if (!w_clean[0]) return false;

	if (ont) {
		for (size_t i = 0; i < ont->conjunction_count; i++) {
			if (strcmp(w_clean, ont->conjunctions[i]) == 0) {
				*out_word_len = wl;
				return true;
			}
		}
		if (jarvis_ontology_is_stopword(ont, w_clean) && wl >= 2 && wl <= 7) {
			*out_word_len = wl;
			return true;
		}
	}
	return false;
}

bool jarvis_pop_ready_tts_clause(
	const char *unspoken,
	bool        is_first_clause,
	char       *out_clause,
	size_t      clause_sz,
	char       *out_remainder,
	size_t      rem_sz
)
{
	if (!unspoken || !out_clause || clause_sz == 0 || !out_remainder || rem_sz == 0) return false;
	out_clause[0] = '\0';
	out_remainder[0] = '\0';

	while (*unspoken && isspace((unsigned char)*unspoken)) unspoken++;
	size_t len = strlen(unspoken);
	if (len == 0) return false;

	/* 1. For the very first clause (TTFA boost), split at a natural preposition/conjunction boundary around 18-38 chars */
	if (is_first_clause && len >= 34) {
		for (size_t i = 18; i <= 38 && i + 2 < len; i++) {
			if (isspace((unsigned char)unspoken[i])) {
				size_t j = i;
				while (j < len && isspace((unsigned char)unspoken[j])) j++;
				size_t wl = 0;
				if (j < len && is_prep_conj_at(unspoken + j, &wl)) {
					size_t clen = i < clause_sz - 1 ? i : clause_sz - 1;
					memcpy(out_clause, unspoken, clen);
					out_clause[clen] = '\0';
					snprintf(out_remainder, rem_sz, "%s", unspoken + j);
					return true;
				}
			}
		}
	}

	/* 2. Strong sentence/clause punctuation (. ! ? ; :) followed by whitespace */
	size_t max_sent = is_first_clause ? 42 : 140;
	for (size_t i = 8; i <= max_sent && i + 1 < len; i++) {
		char c = unspoken[i];
		if ((c == '.' || c == '!' || c == '?' || c == ';' || c == ':') &&
		    isspace((unsigned char)unspoken[i + 1])) {
			size_t clen = (i + 1) < clause_sz - 1 ? (i + 1) : clause_sz - 1;
			memcpy(out_clause, unspoken, clen);
			out_clause[clen] = '\0';
			size_t j = i + 1;
			while (j < len && isspace((unsigned char)unspoken[j])) j++;
			snprintf(out_remainder, rem_sz, "%s", unspoken + j);
			return true;
		}
	}

	/* 3. Comma boundary after a reasonable phrase length */
	size_t min_comma = is_first_clause ? 16 : 32;
	size_t max_comma = is_first_clause ? 42 : 110;
	for (size_t i = min_comma; i <= max_comma && i + 1 < len; i++) {
		if (unspoken[i] == ',' && isspace((unsigned char)unspoken[i + 1])) {
			size_t clen = (i + 1) < clause_sz - 1 ? (i + 1) : clause_sz - 1;
			memcpy(out_clause, unspoken, clen);
			out_clause[clen] = '\0';
			size_t j = i + 1;
			while (j < len && isspace((unsigned char)unspoken[j])) j++;
			snprintf(out_remainder, rem_sz, "%s", unspoken + j);
			return true;
		}
	}

	/* 4. Fallback word boundary if a long unpunctuated run exceeds limit */
	size_t max_run = is_first_clause ? 40 : 95;
	if (len >= max_run + 8) {
		for (size_t i = max_run; i > 18; i--) {
			if (isspace((unsigned char)unspoken[i])) {
				size_t clen = i < clause_sz - 1 ? i : clause_sz - 1;
				memcpy(out_clause, unspoken, clen);
				out_clause[clen] = '\0';
				size_t j = i + 1;
				while (j < len && isspace((unsigned char)unspoken[j])) j++;
				snprintf(out_remainder, rem_sz, "%s", unspoken + j);
				return true;
			}
		}
	}

	return false;
}

/* ---------------------------------------------------------------------------
 * Conversational Anaphora & Multi-Clause Splitting
 * ------------------------------------------------------------------------- */

void jarvis_resolve_anaphora(
	const char             *clause,
	const jarvis_context_t *ctx,
	char                   *out_clause,
	size_t                  out_sz
)
{
	if (!out_clause || out_sz == 0) return;
	out_clause[0] = '\0';
	if (!clause) return;

	while (*clause && isspace((unsigned char)*clause)) clause++;
	char clean[512];
	jarvis_clean_phrase(clause, clean, sizeof(clean));

	const jarvis_ontology_t *ont = jarvis_ontology_get_active();
	if (ont) {
		for (size_t i = 0; i < ont->anaphora.pause_phrase_count; i++) {
			if (strcmp(clean, ont->anaphora.pause_phrases[i]) == 0) {
				if (ont->anaphora.pause_target[0]) {
					snprintf(out_clause, out_sz, "%s", ont->anaphora.pause_target);
				} else {
					snprintf(out_clause, out_sz, "%s", clause);
				}
				return;
			}
		}
		for (size_t i = 0; i < ont->anaphora.resume_phrase_count; i++) {
			if (strcmp(clean, ont->anaphora.resume_phrases[i]) == 0) {
				bool is_media_ctx = (ctx && (strcmp(ctx->last_op, "media_play") == 0 ||
				                             strcmp(ctx->last_op, "media_pause") == 0 ||
				                             strcmp(ctx->last_op, "search_web") == 0));
				if (ctx && ctx->conv_active && !is_media_ctx) {
					break;
				}
				if (ctx && ctx->last_query[0] && is_media_ctx) {
					if (ont->anaphora.resume_prefix[0]) {
						snprintf(out_clause, out_sz, "%s %s", ont->anaphora.resume_prefix, ctx->last_query);
					} else {
						snprintf(out_clause, out_sz, "%s %s", clause, ctx->last_query);
					}
				} else if (ont->anaphora.resume_default[0]) {
					snprintf(out_clause, out_sz, "%s", ont->anaphora.resume_default);
				} else {
					snprintf(out_clause, out_sz, "%s", clause);
				}
				return;
			}
		}
	}

	/* Deictic spatial pronouns configured in ontology -> "[<spatial_prep> ]<last_location_basename>" */
	if (ctx && ctx->last_location[0] && ont && ont->anaphora.spatial_pronoun_count > 0) {
		const char *base = strrchr(ctx->last_location, '/');
		base = (base && base[1]) ? (base + 1) : ctx->last_location;

		/* Scan tokens in clause and check if any token matches a spatial pronoun */
		const char *p = clause;
		while (*p) {
			while (*p && (isspace((unsigned char)*p) || strchr(",.!?;:", *p))) p++;
			if (!*p) break;
			const char *w_start = p;
			while (*p && !isspace((unsigned char)*p) && !strchr(",.!?;:", *p)) p++;
			size_t w_len = (size_t)(p - w_start);
			char tok_raw[64], tok_clean[64];
			if (w_len > 0 && w_len < sizeof(tok_raw)) {
				memcpy(tok_raw, w_start, w_len);
				tok_raw[w_len] = '\0';
				jarvis_clean_phrase(tok_raw, tok_clean, sizeof(tok_clean));
				for (size_t si = 0; si < ont->anaphora.spatial_pronoun_count; si++) {
					if (strcmp(tok_clean, ont->anaphora.spatial_pronouns[si]) == 0) {
						size_t prefix_len = (size_t)(w_start - clause);
						if (ont->anaphora.spatial_prep[0]) {
							snprintf(out_clause, out_sz, "%.*s%s %s%s",
							         (int)prefix_len, clause, ont->anaphora.spatial_prep, base, p);
						} else {
							snprintf(out_clause, out_sz, "%.*s%s%s",
							         (int)prefix_len, clause, base, p);
						}
						return;
					}
				}
			}
		}
	}

	snprintf(out_clause, out_sz, "%s", clause);
}

static void replace_all_ci(char *buf, size_t buf_sz, const char *needle, const char *repl)
{
	char tmp[1024];
	size_t nlen = strlen(needle);
	if (nlen == 0) return;

	const char *p = buf;
	size_t w = 0;
	while (*p && w + 1 < sizeof(tmp)) {
		bool match = true;
		for (size_t i = 0; i < nlen; i++) {
			if (!p[i] || tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) {
				match = false;
				break;
			}
		}
		if (match) {
			size_t rlen = strlen(repl);
			if (w + rlen + 1 >= sizeof(tmp)) break;
			memcpy(tmp + w, repl, rlen);
			w += rlen;
			p += nlen;
		} else {
			tmp[w++] = *p++;
		}
	}
	tmp[w] = '\0';
	snprintf(buf, buf_sz, "%s", tmp);
}

size_t jarvis_split_clauses(
	const jarvis_ontology_t *ont,
	const char              *utterance,
	char                     out_clauses[][512],
	size_t                   max_clauses
)
{
	if (!utterance || !out_clauses || max_clauses == 0) return 0;
	while (*utterance && isspace((unsigned char)*utterance)) utterance++;
	if (!*utterance) return 0;

	if (!ont) ont = jarvis_ontology_get_active();

	/* Check if utterance starts with explicit dictation verb */
	char clean[JARVIS_CLAUSE_LEN];
	jarvis_clean_phrase(utterance, clean, sizeof(clean));
	char *words[JARVIS_MAX_CLAUSE_WORDS];
	size_t nw = 0;
	char *save = NULL;
	for (char *t = strtok_r(clean, " ", &save); t && nw < JARVIS_MAX_CLAUSE_WORDS; t = strtok_r(NULL, " ", &save)) {
		words[nw++] = t;
	}
	size_t wi = 0;
	while (wi < nw && ont && jarvis_ontology_is_polite_prefix(ont, words[wi])) {
		wi++;
	}
	if (wi < nw && ont && jarvis_ontology_is_dictation_verb(ont, words[wi])) {
		char low[JARVIS_CLAUSE_LEN];
		snprintf(low, sizeof(low), "%s", utterance);
		for (size_t i = 0; low[i]; i++) low[i] = (char)tolower((unsigned char)low[i]);
		bool has_explicit_sep = false;
		for (size_t si = 0; si < ont->clause_sep_count; si++) {
			const char *sep = ont->clause_seps[si];
			if (!sep[0] || strcmp(sep, ", ") == 0 || strchr(sep, ',') != NULL) continue;
			bool is_bare_conj = false;
			for (size_t ci = 0; ci < ont->conjunction_count; ci++) {
				if (strcmp(sep, ont->conjunctions[ci]) == 0) {
					is_bare_conj = true;
					break;
				}
			}
			if (is_bare_conj) continue;
			char padded_sep[80];
			snprintf(padded_sep, sizeof(padded_sep), " %s ", sep);
			if (strstr(low, padded_sep)) {
				has_explicit_sep = true;
				break;
			}
		}
		if (!has_explicit_sep) {
			snprintf(out_clauses[0], sizeof(out_clauses[0]), "%s", utterance);
			return 1;
		}
	}

	/* Strip leading wake-word / polite address prefix before comma */
	char work[JARVIS_TEXT_LEN];
	snprintf(work, sizeof(work), "%s", utterance);
	{
		char *comma = strchr(work, ',');
		if (comma && ont) {
			char pre[JARVIS_ID_LEN] = {0};
			size_t plen = (size_t)(comma - work);
			if (plen < sizeof(pre)) {
				memcpy(pre, work, plen);
				pre[plen] = '\0';
				char pre_clean[JARVIS_ID_LEN];
				jarvis_clean_phrase(pre, pre_clean, sizeof(pre_clean));
				bool all_polite = (pre_clean[0] != '\0');
				if (all_polite && !jarvis_ontology_is_polite_prefix(ont, pre_clean)) {
					char *psave = NULL;
					for (char *pt = strtok_r(pre_clean, " ", &psave); pt; pt = strtok_r(NULL, " ", &psave)) {
						if (!jarvis_ontology_is_polite_prefix(ont, pt)) {
							all_polite = false;
							break;
						}
					}
				}
				if (all_polite) {
					char *after = comma + 1;
					while (*after && isspace((unsigned char)*after)) after++;
					memmove(work, after, strlen(after) + 1);
				}
			}
		}
	}

	/* Protect atomic phrases from ont->loc_submit and multi-word entities containing conjunctions */
	char sub_toks[JARVIS_MAX_LOCATIVES][32];
	char sub_orig[JARVIS_MAX_LOCATIVES][JARVIS_WORD_LEN];
	size_t n_sub_prot = 0;
	if (ont) {
		for (size_t i = 0; i < ont->loc_submit_count && n_sub_prot < JARVIS_MAX_LOCATIVES; i++) {
			char pat[96], tok[32];
			snprintf(pat, sizeof(pat), " %s", ont->loc_submit[i]);
			snprintf(tok, sizeof(tok), " __LOC_SUBMIT_%zu__", n_sub_prot);
			snprintf(sub_toks[n_sub_prot], sizeof(sub_toks[0]), "__LOC_SUBMIT_%zu__", n_sub_prot);
			snprintf(sub_orig[n_sub_prot], sizeof(sub_orig[0]), "%s", ont->loc_submit[i]);
			replace_all_ci(work, sizeof(work), pat, tok);
			n_sub_prot++;
		}
	}

	/* Normalize explicit multi-clause separators from ont->clause_seps to ';' */
	if (ont) {
		for (size_t i = 0; i < ont->clause_sep_count; i++) {
			const char *sep = ont->clause_seps[i];
			if (!sep[0]) continue;
			/* Skip bare single-word conjunctions here; handled below with verb-head lookahead */
			bool is_bare_conj = false;
			for (size_t ci = 0; ci < ont->conjunction_count; ci++) {
				if (strcmp(sep, ont->conjunctions[ci]) == 0) {
					is_bare_conj = true;
					break;
				}
			}
			if (is_bare_conj) continue;
			if (sep[0] == ' ' || sep[0] == ',') {
				replace_all_ci(work, sizeof(work), sep, ";");
			} else {
				char padded_sep[80];
				snprintf(padded_sep, sizeof(padded_sep), " %s ", sep);
				replace_all_ci(work, sizeof(work), padded_sep, ";");
			}
		}
	}

	size_t count = 0;
	char *save_semi = NULL;
	for (char *part = strtok_r(work, ";", &save_semi); part && count < max_clauses; part = strtok_r(NULL, ";", &save_semi)) {
		while (*part && isspace((unsigned char)*part)) part++;
		if (!*part) continue;

		/* Further split on ont->conjunctions (e.g. " and ", " e ") if the RHS begins with a known verb head */
		char *cursor = part;
		char accum[512] = {0};
		char prev_conj[JARVIS_SHORT_LEN] = {0};

		while (cursor && *cursor && count < max_clauses) {
			char *best_pos = NULL;
			size_t best_conj_len = 0;
			char matched_conj[JARVIS_SHORT_LEN] = {0};

			if (ont) {
				for (size_t ci = 0; ci < ont->conjunction_count; ci++) {
					const char *cj = ont->conjunctions[ci];
					size_t clen = strlen(cj);
					if (clen == 0) continue;
					for (char *p = cursor; *p; p++) {
						if (*p == ' ' && p[clen + 1] == ' ') {
							bool m = true;
							for (size_t k = 0; k < clen; k++) {
								if (tolower((unsigned char)p[1 + k]) != tolower((unsigned char)cj[k])) {
									m = false;
									break;
								}
							}
							if (m && (!best_pos || p < best_pos)) {
								best_pos = p;
								best_conj_len = clen + 2;
								snprintf(matched_conj, sizeof(matched_conj), "%s", cj);
								break;
							}
						}
					}
				}
			}

			if (!best_pos) {
				if (accum[0]) {
					size_t alen = strlen(accum);
					snprintf(accum + alen, sizeof(accum) - alen, " %s %s", prev_conj, cursor);
				} else {
					snprintf(accum, sizeof(accum), "%s", cursor);
				}
				break;
			}

			*best_pos = '\0';
			char *rhs = best_pos + best_conj_len;
			while (*rhs && isspace((unsigned char)*rhs)) rhs++;

			if (accum[0]) {
				size_t alen = strlen(accum);
				snprintf(accum + alen, sizeof(accum) - alen, " %s %s", prev_conj, cursor);
			} else {
				snprintf(accum, sizeof(accum), "%s", cursor);
			}
			snprintf(prev_conj, sizeof(prev_conj), "%s", matched_conj);

			/* Check if first non-polite/non-stopword word of rhs is a known verb head */
			char rhs_clean[256];
			jarvis_clean_phrase(rhs, rhs_clean, sizeof(rhs_clean));
			char rhs_copy[256];
			snprintf(rhs_copy, sizeof(rhs_copy), "%s", rhs_clean);
			char *rsave = NULL;
			char *rw = strtok_r(rhs_copy, " ", &rsave);
			while (rw && ont && !jarvis_ontology_is_verb_head(ont, rw) &&
			       (jarvis_ontology_is_polite_prefix(ont, rw) || jarvis_ontology_is_stopword(ont, rw))) {
				rw = strtok_r(NULL, " ", &rsave);
			}
			if ((rw && ont && jarvis_ontology_is_verb_head(ont, rw)) ||
			    (ont && jarvis_ontology_is_tts_interrupt(ont, rhs_clean))) {
				char *slot = out_clauses[count++];
				snprintf(slot, sizeof(out_clauses[0]), "%s", accum);
				accum[0] = '\0';
				prev_conj[0] = '\0';
			}
			cursor = rhs;
		}

		if (accum[0] && count < max_clauses) {
			char *slot = out_clauses[count++];
			snprintf(slot, sizeof(out_clauses[0]), "%s", accum);
		}
	}

	/* Restore protected placeholders */
	for (size_t i = 0; i < count; i++) {
		for (size_t si = 0; si < n_sub_prot; si++) {
			replace_all_ci(out_clauses[i], sizeof(out_clauses[i]), sub_toks[si], sub_orig[si]);
		}

		/* Trim whitespace/punctuation */
		char *s = out_clauses[i];
		while (*s && (isspace((unsigned char)*s) || strchr(",.!?;:", *s))) s++;
		if (s != out_clauses[i]) memmove(out_clauses[i], s, strlen(s) + 1);
		size_t l = strlen(out_clauses[i]);
		while (l > 0 && (isspace((unsigned char)out_clauses[i][l - 1]) || strchr(",.!?;:", out_clauses[i][l - 1]))) {
			out_clauses[i][--l] = '\0';
		}
	}

	if (count == 0) {
		snprintf(out_clauses[0], sizeof(out_clauses[0]), "%s", utterance);
		return 1;
	}
	return count;
}

/* ---------------------------------------------------------------------------
 * Engine Lifecycle, Persistence, ~/.jarvisrc & Context
 * ------------------------------------------------------------------------- */

static void resolve_jarvisrc_path(char *out, size_t out_sz, bool for_write)
{
	const char *env_rc = getenv("JARVISRC");
	if (env_rc && *env_rc) {
		snprintf(out, out_sz, "%s", env_rc);
		return;
	}
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";
	snprintf(out, out_sz, "%s/.jarvisrc", home);
#ifndef __wasm__
	if (!for_write && access(out, R_OK) != 0) {
		char alt[512];
		snprintf(alt, sizeof(alt), "%s/.config/jarvis/jarvisrc", home);
		if (access(alt, R_OK) == 0) {
			snprintf(out, out_sz, "%s", alt);
		}
	}
#else
	(void)for_write;
#endif
}

static void expand_tilde_path(const char *val, char *out, size_t out_sz)
{
	if (!val || !out || out_sz == 0) return;
	if (val[0] == '~' && (val[1] == '/' || val[1] == '\0')) {
		const char *home = getenv("HOME");
		if (home && *home) {
			snprintf(out, out_sz, "%s%s", home, val + 1);
			return;
		}
	}
	snprintf(out, out_sz, "%s", val);
}

bool jarvis_rc_get(const char *key, const char *fallback, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	if (fallback) {
		expand_tilde_path(fallback, out, out_sz);
	} else {
		out[0] = '\0';
	}
	if (!key || !*key) return false;
#ifndef __wasm__
	char rc_path[512];
	resolve_jarvisrc_path(rc_path, sizeof(rc_path), false);
	FILE *fp = fopen(rc_path, "r");
	if (!fp) return false;

	bool found = false;
	char line[1024];
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p || *p == '#' || *p == ';') continue;
		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = '\0';
		char *k = p;
		size_t kl = strlen(k);
		while (kl > 0 && isspace((unsigned char)k[kl - 1])) k[--kl] = '\0';
		if (strcasecmp(k, key) != 0) continue;

		char *v = eq + 1;
		while (*v && isspace((unsigned char)*v)) v++;
		size_t vl = strlen(v);
		while (vl > 0 && (isspace((unsigned char)v[vl - 1]) || v[vl - 1] == '\r' || v[vl - 1] == '\n')) {
			v[--vl] = '\0';
		}
		if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
			v[vl - 1] = '\0';
			v++;
		}
		expand_tilde_path(v, out, out_sz);
		found = true;
	}
	fclose(fp);
	return found;
#else
	return false;
#endif
}

int jarvis_rc_get_int(const char *key, int fallback)
{
	char buf[64];
	if (!jarvis_rc_get(key, NULL, buf, sizeof(buf)) || !buf[0]) {
		return fallback;
	}
	if (strcasecmp(buf, "true") == 0 || strcasecmp(buf, "yes") == 0 || strcasecmp(buf, "on") == 0) {
		return 1;
	}
	if (strcasecmp(buf, "false") == 0 || strcasecmp(buf, "no") == 0 || strcasecmp(buf, "off") == 0) {
		return 0;
	}
	return atoi(buf);
}

bool jarvis_rc_set(const char *key, const char *value)
{
	if (!key || !*key || !value) return false;
#ifndef __wasm__
	char rc_path[512];
	resolve_jarvisrc_path(rc_path, sizeof(rc_path), true);

	char lines[128][1024];
	size_t n_lines = 0;
	bool replaced = false;

	FILE *fp = fopen(rc_path, "r");
	if (fp) {
		while (n_lines < 127 && fgets(lines[n_lines], sizeof(lines[0]), fp)) {
			char copy[1024];
			snprintf(copy, sizeof(copy), "%s", lines[n_lines]);
			char *p = copy;
			while (*p && isspace((unsigned char)*p)) p++;
			if (*p && *p != '#' && *p != ';') {
				char *eq = strchr(p, '=');
				if (eq) {
					*eq = '\0';
					size_t kl = strlen(p);
					while (kl > 0 && isspace((unsigned char)p[kl - 1])) p[--kl] = '\0';
					if (strcasecmp(p, key) == 0) {
						snprintf(lines[n_lines], sizeof(lines[0]), "%s = %s\n", key, value);
						replaced = true;
					}
				}
			}
			n_lines++;
		}
		fclose(fp);
	}
	if (!replaced && n_lines < 128) {
		snprintf(lines[n_lines++], sizeof(lines[0]), "%s = %s\n", key, value);
	}

	char tmp_path[540];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", rc_path);
	FILE *out = fopen(tmp_path, "w");
	if (!out) return false;
	for (size_t i = 0; i < n_lines; i++) {
		fputs(lines[i], out);
	}
	fclose(out);
	return rename(tmp_path, rc_path) == 0;
#else
	return false;
#endif
}

void jarvis_config_init(jarvis_config_t *cfg)
{
	if (!cfg) return;
	memset(cfg, 0, sizeof(*cfg));
	static char s_wm[64], s_auren[256], s_llm[64];
	jarvis_rc_get("wm", "dwm", s_wm, sizeof(s_wm));
	jarvis_rc_get("auren_url", "http://127.0.0.1:4242", s_auren, sizeof(s_auren));
	jarvis_rc_get("llm_model", "kilo-auto/free", s_llm, sizeof(s_llm));
	cfg->wm         = s_wm;
	cfg->auren_url  = s_auren;
	cfg->llm_model  = s_llm;
	cfg->infer_mode = JARVIS_INFER_AUTO;
	cfg->dry_run    = false;
}

static void load_memories_from_disk(jarvis_engine_t *eng)
{
#ifndef __wasm__
	if (!eng->memory_path[0]) return;
	FILE *fp = fopen(eng->memory_path, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz > 0 && sz < 256 * 1024) {
		char *buf = (char *)malloc((size_t)sz + 1);
		if (buf) {
			size_t rd = fread(buf, 1, (size_t)sz, fp);
			buf[rd] = '\0';
			jjson_val_t *v = jjson_parse(buf);
			if (v && v->type == JJSON_OBJECT) {
				eng->memory_count = 0;
				for (size_t i = 0; i < v->u.obj.count && eng->memory_count < JARVIS_MAX_MEMORIES; i++) {
					const char *k = v->u.obj.pairs[i].key;
					const jjson_val_t *val = v->u.obj.pairs[i].val;
					if (k && val && val->type == JJSON_STRING && val->u.str) {
						jarvis_memory_kv_t *m = &eng->memories[eng->memory_count++];
						snprintf(m->key, sizeof(m->key), "%s", k);
						snprintf(m->value, sizeof(m->value), "%s", val->u.str);
					}
				}
			}
			jjson_free(v);
			free(buf);
		}
	}
	fclose(fp);
#else
	(void)eng;
#endif
}

static void save_memories_to_disk(const jarvis_engine_t *eng)
{
#ifndef __wasm__
	if (!eng->memory_path[0]) return;
	ensure_parent_dir(eng->memory_path);
	FILE *fp = fopen(eng->memory_path, "wb");
	if (!fp) return;
	fprintf(fp, "{\n");
	for (size_t i = 0; i < eng->memory_count; i++) {
		char ek[256], ev[1024];
		jjson_escape_str(eng->memories[i].key, ek, sizeof(ek));
		jjson_escape_str(eng->memories[i].value, ev, sizeof(ev));
		fprintf(fp, "  \"%s\": \"%s\"%s\n", ek, ev, (i + 1 < eng->memory_count) ? "," : "");
	}
	fprintf(fp, "}\n");
	fclose(fp);
#else
	(void)eng;
#endif
}

static void sync_memory_to_mm(jarvis_engine_t *eng, const char *key, const char *value)
{
#ifndef __wasm__
	if (!eng || !eng->mm || !key || !*key || !value || !*value) return;
	mm_record_req_t req;
	memset(&req, 0, sizeof(req));
	req.tier      = MM_TIER_GLOBAL;
	req.level     = MM_LEVEL_L2;
	req.type      = "rule";
	req.status    = MM_STATUS_SUPPORTED;
	req.standing  = true;
	req.problem   = value;
	req.subsystem = "jarvis";
	req.module    = key;
	const char *tags[2] = { key, NULL };
	req.tags      = tags;
	req.n_tags    = 1;
	mm_record(eng->mm, &req, NULL, NULL);
#else
	(void)eng;
	(void)key;
	(void)value;
#endif
}

bool jarvis_memory_remember(jarvis_engine_t *eng, const char *key, const char *value)
{
	if (!eng || !key || !*key || !value || !*value) return false;
	for (size_t i = 0; i < eng->memory_count; i++) {
		if (strcmp(eng->memories[i].key, key) == 0) {
			snprintf(eng->memories[i].value, sizeof(eng->memories[i].value), "%s", value);
			save_memories_to_disk(eng);
			sync_memory_to_mm(eng, key, value);
			return true;
		}
	}
	if (eng->memory_count < JARVIS_MAX_MEMORIES) {
		jarvis_memory_kv_t *m = &eng->memories[eng->memory_count++];
		snprintf(m->key, sizeof(m->key), "%s", key);
		snprintf(m->value, sizeof(m->value), "%s", value);
		save_memories_to_disk(eng);
		sync_memory_to_mm(eng, key, value);
		return true;
	}
	return false;
}

size_t jarvis_memory_recall_json(jarvis_engine_t *eng, const char *query, char *out_json, size_t out_sz)
{
	if (!out_json || out_sz == 0) return 0;
	if (!eng) {
		snprintf(out_json, out_sz, "{}");
		return 2;
	}
	char q_clean[256] = {0};
	if (query && *query) {
		jarvis_clean_phrase(query, q_clean, sizeof(q_clean));
	}

	int n = snprintf(out_json, out_sz, "{");
	bool first = true;
	size_t matched = 0;

#ifndef __wasm__
	/* First query libmm via mm_recall_candidates for ranked multi-axis recall */
	if (eng->mm) {
		mm_recall_req_t rreq;
		memset(&rreq, 0, sizeof(rreq));
		rreq.scope     = MM_SCOPE_PERSISTENT;
		rreq.tier      = MM_TIER_GLOBAL;
		rreq.query     = (query && *query) ? query : "jarvis";
		rreq.subsystem = "jarvis";
		rreq.limit     = 32;
		rreq.dedupe    = true;
		mm_candidate_t *cands = NULL;
		size_t n_cands = 0;
		if (mm_recall_candidates(eng->mm, &rreq, &cands, &n_cands) == 0 && cands) {
			for (size_t i = 0; i < n_cands; i++) {
				if (!cands[i].has_envelope) continue;
				const mm_envelope_t *env = &cands[i].envelope;
				const char *mkey = env->ctx_module[0] ? env->ctx_module :
				                   (env->n_tags > 0 ? env->tags[0] : NULL);
				const char *mval = env->summary;
				if (!mkey || !*mkey || !mval || !*mval) continue;
				if (q_clean[0]) {
					char hay[640], hay_c[640];
					snprintf(hay, sizeof(hay), "%s %s", mkey, mval);
					jarvis_clean_phrase(hay, hay_c, sizeof(hay_c));
					if (!strstr(hay_c, q_clean) && cands[i].ranking_score < 0.25) continue;
				}
				/* Sync into eng->memories if not already present */
				bool exists = false;
				for (size_t j = 0; j < eng->memory_count; j++) {
					if (strcmp(eng->memories[j].key, mkey) == 0) {
						exists = true;
						break;
					}
				}
				if (!exists && eng->memory_count < JARVIS_MAX_MEMORIES) {
					jarvis_memory_kv_t *m = &eng->memories[eng->memory_count++];
					snprintf(m->key, sizeof(m->key), "%s", mkey);
					snprintf(m->value, sizeof(m->value), "%s", mval);
				}
			}
			mm_candidates_free(cands, n_cands);
		}
	}
#endif

	for (int pass = 0; pass < 2; pass++) {
		/* Pass 0: filter by query; Pass 1: fallback to all if 0 matched */
		if (pass == 1 && (matched > 0 || !q_clean[0])) break;
		for (size_t i = 0; i < eng->memory_count; i++) {
			if (pass == 0 && q_clean[0]) {
				char hay[640], hay_c[640];
				snprintf(hay, sizeof(hay), "%s %s", eng->memories[i].key, eng->memories[i].value);
				jarvis_clean_phrase(hay, hay_c, sizeof(hay_c));
				if (!strstr(hay_c, q_clean)) continue;
			}
			char ek[256], ev[1024];
			jjson_escape_str(eng->memories[i].key, ek, sizeof(ek));
			jjson_escape_str(eng->memories[i].value, ev, sizeof(ev));
			n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0,
			              "%s\"%s\":\"%s\"", first ? "" : ",", ek, ev);
			first = false;
			matched++;
		}
	}
	n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "}");
	return (size_t)n < out_sz ? (size_t)n : out_sz - 1;
}

static void load_context_from_disk(jarvis_engine_t *eng)
{
#ifndef __wasm__
	if (!eng->context_path[0]) return;
	FILE *fp = fopen(eng->context_path, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz > 0 && sz < 64 * 1024) {
		char *buf = (char *)malloc((size_t)sz + 1);
		if (buf) {
			size_t rd = fread(buf, 1, (size_t)sz, fp);
			buf[rd] = '\0';
			jjson_val_t *v = jjson_parse(buf);
			if (v && v->type == JJSON_OBJECT) {
				eng->ctx.last_ts = jjson_obj_get_num(v, "last_ts", 0.0);
				snprintf(eng->ctx.last_op, sizeof(eng->ctx.last_op), "%s", jjson_obj_get_str(v, "last_op", ""));
				snprintf(eng->ctx.last_command, sizeof(eng->ctx.last_command), "%s", jjson_obj_get_str(v, "last_command", ""));
				snprintf(eng->ctx.last_app, sizeof(eng->ctx.last_app), "%s", jjson_obj_get_str(v, "last_app", ""));
				snprintf(eng->ctx.last_query, sizeof(eng->ctx.last_query), "%s", jjson_obj_get_str(v, "last_query", ""));
				snprintf(eng->ctx.last_location, sizeof(eng->ctx.last_location), "%s", jjson_obj_get_str(v, "last_location", ""));
				snprintf(eng->ctx.last_window_id, sizeof(eng->ctx.last_window_id), "%s", jjson_obj_get_str(v, "last_window_id", ""));
				snprintf(eng->ctx.last_window_class, sizeof(eng->ctx.last_window_class), "%s", jjson_obj_get_str(v, "last_window_class", ""));
				eng->ctx.last_workspace = (int)jjson_obj_get_num(v, "last_workspace", 0.0);
				eng->ctx.prev_workspace = (int)jjson_obj_get_num(v, "prev_workspace", 1.0);
				snprintf(eng->ctx.last_url, sizeof(eng->ctx.last_url), "%s", jjson_obj_get_str(v, "last_url", ""));
				snprintf(eng->ctx.last_file, sizeof(eng->ctx.last_file), "%s", jjson_obj_get_str(v, "last_file", ""));
				eng->ctx.last_line = (int)jjson_obj_get_num(v, "last_line", 0.0);
				eng->ctx.conv_active = jjson_obj_get_bool(v, "conv_active", false);
				eng->ctx.conv_last_ts = jjson_obj_get_num(v, "conv_last_ts", 0.0);
			}
			jjson_free(v);
			free(buf);
		}
	}
	fclose(fp);
#else
	(void)eng;
#endif
}

static void save_context_to_disk(const jarvis_engine_t *eng)
{
#ifndef __wasm__
	if (!eng || !eng->context_path[0]) return;
	ensure_parent_dir(eng->context_path);
	FILE *fp = fopen(eng->context_path, "wb");
	if (!fp) return;
	char e_op[128], e_cmd[1024], e_app[256], e_q[512], e_loc[512], e_wid[128], e_wcls[256], e_url[1024], e_file[512];
	jjson_escape_str(eng->ctx.last_op, e_op, sizeof(e_op));
	jjson_escape_str(eng->ctx.last_command, e_cmd, sizeof(e_cmd));
	jjson_escape_str(eng->ctx.last_app, e_app, sizeof(e_app));
	jjson_escape_str(eng->ctx.last_query, e_q, sizeof(e_q));
	jjson_escape_str(eng->ctx.last_location, e_loc, sizeof(e_loc));
	jjson_escape_str(eng->ctx.last_window_id, e_wid, sizeof(e_wid));
	jjson_escape_str(eng->ctx.last_window_class, e_wcls, sizeof(e_wcls));
	jjson_escape_str(eng->ctx.last_url, e_url, sizeof(e_url));
	jjson_escape_str(eng->ctx.last_file, e_file, sizeof(e_file));
	fprintf(fp,
		"{\n"
		"  \"last_ts\": %.3f,\n"
		"  \"last_op\": \"%s\",\n"
		"  \"last_command\": \"%s\",\n"
		"  \"last_app\": \"%s\",\n"
		"  \"last_query\": \"%s\",\n"
		"  \"last_location\": \"%s\",\n"
		"  \"last_window_id\": \"%s\",\n"
		"  \"last_window_class\": \"%s\",\n"
		"  \"last_workspace\": %d,\n"
		"  \"prev_workspace\": %d,\n"
		"  \"last_url\": \"%s\",\n"
		"  \"last_file\": \"%s\",\n"
		"  \"last_line\": %d,\n"
		"  \"conv_active\": %s,\n"
		"  \"conv_last_ts\": %.3f\n"
		"}\n",
		eng->ctx.last_ts, e_op, e_cmd, e_app, e_q, e_loc, e_wid, e_wcls,
		eng->ctx.last_workspace, eng->ctx.prev_workspace, e_url, e_file,
		eng->ctx.last_line, eng->ctx.conv_active ? "true" : "false", eng->ctx.conv_last_ts
	);
	fclose(fp);
#else
	(void)eng;
#endif
}

static void load_session_from_disk(jarvis_engine_t *eng)
{
#ifndef __wasm__
	if (!eng->session_path[0]) return;
	FILE *fp = fopen(eng->session_path, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz > 0 && sz < 128 * 1024) {
		char *buf = (char *)malloc((size_t)sz + 1);
		if (buf) {
			size_t rd = fread(buf, 1, (size_t)sz, fp);
			buf[rd] = '\0';
			jjson_val_t *v = jjson_parse(buf);
			if (v && v->type == JJSON_ARRAY) {
				eng->turn_count = 0;
				for (size_t i = 0; i < v->u.arr.count && eng->turn_count < JARVIS_MAX_TURNS; i++) {
					const jjson_val_t *item = v->u.arr.items[i];
					if (item && item->type == JJSON_OBJECT) {
						jarvis_session_turn_t *t = &eng->turns[eng->turn_count++];
						memset(t, 0, sizeof(*t));
						t->ts = jjson_obj_get_num(item, "ts", 0.0);
						snprintf(t->utterance, sizeof(t->utterance), "%s", jjson_obj_get_str(item, "utterance", ""));
						snprintf(t->ops, sizeof(t->ops), "%s", jjson_obj_get_str(item, "ops", ""));
						snprintf(t->output, sizeof(t->output), "%s", jjson_obj_get_str(item, "output", ""));
						snprintf(t->spoken, sizeof(t->spoken), "%s", jjson_obj_get_str(item, "spoken", ""));
						t->expect_reply = jjson_obj_get_bool(item, "expect_reply", false);
					}
				}
			}
			jjson_free(v);
			free(buf);
		}
	}
	fclose(fp);
#else
	(void)eng;
#endif
}

static void save_session_to_disk(const jarvis_engine_t *eng)
{
#ifndef __wasm__
	if (!eng || !eng->session_path[0]) return;
	ensure_parent_dir(eng->session_path);
	FILE *fp = fopen(eng->session_path, "wb");
	if (!fp) return;
	fprintf(fp, "[\n");
	for (size_t i = 0; i < eng->turn_count; i++) {
		char eu[1024], eo[256], eout[2048], espk[2048];
		jjson_escape_str(eng->turns[i].utterance, eu, sizeof(eu));
		jjson_escape_str(eng->turns[i].ops, eo, sizeof(eo));
		jjson_escape_str(eng->turns[i].output, eout, sizeof(eout));
		jjson_escape_str(eng->turns[i].spoken, espk, sizeof(espk));
		fprintf(fp,
			"  {\"ts\":%.3f,\"utterance\":\"%s\",\"ops\":\"%s\",\"output\":\"%s\",\"spoken\":\"%s\",\"expect_reply\":%s}%s\n",
			eng->turns[i].ts, eu, eo, eout, espk,
			eng->turns[i].expect_reply ? "true" : "false",
			(i + 1 < eng->turn_count) ? "," : ""
		);
	}
	fprintf(fp, "]\n");
	fclose(fp);
#else
	(void)eng;
#endif
}

jarvis_engine_t *jarvis_engine_create(const jarvis_config_t *cfg)
{
	jarvis_config_t def_cfg;
	if (!cfg) {
		jarvis_config_init(&def_cfg);
		cfg = &def_cfg;
	}

	jarvis_engine_t *eng = (jarvis_engine_t *)calloc(1, sizeof(jarvis_engine_t));
	if (!eng) return NULL;

	snprintf(eng->wm, sizeof(eng->wm), "%s", (cfg->wm && *cfg->wm) ? cfg->wm : "dwm");
	snprintf(eng->auren_url, sizeof(eng->auren_url), "%s", (cfg->auren_url && *cfg->auren_url) ? cfg->auren_url : "http://127.0.0.1:4242");
	const char *env_m = getenv("JARVIS_LLM_MODEL");
	snprintf(eng->llm_model, sizeof(eng->llm_model), "%s",
	         (cfg->llm_model && *cfg->llm_model) ? cfg->llm_model :
	         ((env_m && *env_m) ? env_m : "fast"));
	eng->infer_mode = cfg->infer_mode;
	eng->dry_run    = cfg->dry_run;

	if (cfg->ontology_path) snprintf(eng->ontology_path, sizeof(eng->ontology_path), "%s", cfg->ontology_path);
	if (cfg->few_shot_path) snprintf(eng->few_shot_path, sizeof(eng->few_shot_path), "%s", cfg->few_shot_path);
	if (cfg->context_path)  snprintf(eng->context_path, sizeof(eng->context_path), "%s", cfg->context_path);
	if (cfg->memory_path)   snprintf(eng->memory_path, sizeof(eng->memory_path), "%s", cfg->memory_path);
	if (cfg->session_path)  snprintf(eng->session_path, sizeof(eng->session_path), "%s", cfg->session_path);
	if (cfg->embed_cache_path) snprintf(eng->embed_cache_path, sizeof(eng->embed_cache_path), "%s", cfg->embed_cache_path);

	eng->ont = jarvis_ontology_create(eng->ontology_path[0] ? eng->ontology_path : NULL);
	if (!eng->ont) {
		free(eng);
		return NULL;
	}

	load_memories_from_disk(eng);
	load_context_from_disk(eng);
	load_session_from_disk(eng);

#ifndef __wasm__
	{
		mm_options_t mm_opts;
		memset(&mm_opts, 0, sizeof(mm_opts));
		char mm_dir[520] = {0};
		if (eng->memory_path[0]) {
			snprintf(mm_dir, sizeof(mm_dir), "%s.mm", eng->memory_path);
			mm_opts.global_dir = mm_dir;
			mm_opts.project_dir = mm_dir;
			mm_opts.prefer_daemon = false;
		} else {
			mm_opts.prefer_daemon = true;
		}
		eng->mm = mm_open(&mm_opts);
		if (eng->mm) {
			for (size_t i = 0; i < eng->memory_count; i++) {
				sync_memory_to_mm(eng, eng->memories[i].key, eng->memories[i].value);
			}
		}
	}
#endif

	return eng;
}

void jarvis_engine_destroy(jarvis_engine_t *eng)
{
	if (!eng) return;
#ifndef __wasm__
	if (eng->mm) {
		mm_close(eng->mm);
		eng->mm = NULL;
	}
#endif
	jarvis_ontology_destroy(eng->ont);
	free(eng);
}

const jarvis_ontology_t *jarvis_engine_get_ontology(const jarvis_engine_t *eng)
{
	return eng && eng->ont ? eng->ont : jarvis_ontology_get_active();
}


static bool requires_agent_reasoning(const jarvis_engine_t *eng, const char *utterance)
{
	if (!eng || eng->infer_mode == JARVIS_INFER_EMBEDDING) {
		return false;
	}
	if (!utterance) return false;
	while (*utterance && isspace((unsigned char)*utterance)) utterance++;
	size_t len = strlen(utterance);
	while (len > 0 && isspace((unsigned char)utterance[len - 1])) len--;
	if (len == 0) return false;
	char clean[512];
	jarvis_clean_phrase(utterance, clean, sizeof(clean));

	const jarvis_ontology_t *ont = eng->ont ? eng->ont : jarvis_ontology_get_active();

	/* If ASR normalization strips a leading Whisper prompt-echo ("o que épode ...")
	 * and the normalized utterance deterministically solves as a high-confidence Tier-0
	 * command without starting with an interrogative, do not force ReAct agent reasoning.
	 */
	if (ont && strncmp(clean, "o que ", 6) == 0) {
		jarvis_ir_t norm_ir;
		if (jarvis_solve_clause(ont, utterance, "any", NULL, &norm_ir) && norm_ir.score >= 0.90) {
			return false;
		}
	}

	if (utterance[len - 1] == '?') {
		/* A trailing '?' on a polite modal request ("Podes abrir a calculadora, se faz favor?",
		 * "Can you open the terminal?") is a polite imperative, not an open question, provided
		 * it does not start with an interrogative word ("what", "where", "qual", "como", etc.)
		 * and solves deterministically at high confidence (>= 0.90).
		 */
		bool is_interrog = false;
		if (ont) {
			char first_w[64] = {0};
			sscanf(clean, "%63s", first_w);
			if (first_w[0] && jarvis_ontology_is_interrogative(ont, first_w)) {
				is_interrog = true;
			}
			if (!is_interrog) {
				jarvis_ir_t q_ir;
				if (jarvis_solve_clause(ont, utterance, "any", NULL, &q_ir) && q_ir.score >= 0.90) {
					return false;
				}
			}
		}
		return true;
	}

	/* Never force agent reasoning on explicit dictation or TTS toggle */
	if (ont) {
		for (size_t i = 0; i < ont->dictation_verb_count; i++) {
			size_t vlen = strlen(ont->dictation_verbs[i]);
			if (vlen > 0 && strncmp(clean, ont->dictation_verbs[i], vlen) == 0 && clean[vlen] == ' ') {
				return false;
			}
		}
		jarvis_ir_t probe_ir;
		if (jarvis_solve_clause(ont, utterance, "any", NULL, &probe_ir) &&
		    (strcmp(probe_ir.cap, "tts.control") == 0 ||
		     strcmp(probe_ir.cap, "tts.interrupt") == 0 ||
		     strcmp(probe_ir.cap, "conversation.control") == 0)) {
			return false;
		}
	}

	char padded[520];
	snprintf(padded, sizeof(padded), " %s ", clean);

	if (ont) {
		for (size_t i = 0; i < ont->agent_trigger_count; i++) {
			if (ont->agent_triggers[i][0] && strstr(padded, ont->agent_triggers[i])) {
				return true;
			}
		}
	}

	/* Check if utterance references a user-defined workflow/memory key */
	for (size_t i = 0; i < eng->memory_count; i++) {
		char mk_clean[128];
		if (jarvis_clean_phrase(eng->memories[i].key, mk_clean, sizeof(mk_clean)) >= 3) {
			if (strstr(clean, mk_clean)) return true;
		}
	}

	return false;
}

static void extract_file_from_text(const char *text, char *out_file, size_t file_sz, int *out_line)
{
	if (!text || !out_file || file_sz == 0) return;
	static const char *EXTS[] = {".c:", ".h:", ".py:", ".rs:", ".js:", ".ts:", ".cpp:", ".go:", ".sh:", NULL};
	for (size_t i = 0; EXTS[i]; i++) {
		const char *hit = strstr(text, EXTS[i]);
		if (hit) {
			const char *start = hit;
			while (start > text && !isspace((unsigned char)start[-1]) &&
			       start[-1] != '"' && start[-1] != '\'' && start[-1] != '(' && start[-1] != '[') {
				start--;
			}
			const char *colon = hit + strlen(EXTS[i]) - 1;
			size_t flen = (size_t)(colon - start);
			if (flen > 0 && flen < file_sz) {
				memcpy(out_file, start, flen);
				out_file[flen] = '\0';
				if (out_line && isdigit((unsigned char)colon[1])) {
					*out_line = atoi(colon + 1);
				}
				return;
			}
		}
	}
}

static void update_context_from_step(
	jarvis_context_t     *ctx,
	const jarvis_dsl_t   *dsl,
	const jarvis_world_t *world,
	const char           *step_output
)
{
	if (!ctx || !dsl) return;
	ctx->last_ts = now_ms() / 1000.0;
	snprintf(ctx->last_op, sizeof(ctx->last_op), "%s", dsl->op);

	if (world && world->focused.id[0]) {
		snprintf(ctx->last_window_id, sizeof(ctx->last_window_id), "%s", world->focused.id);
		snprintf(ctx->last_window_class, sizeof(ctx->last_window_class), "%s", world->focused.wm_class);
		if (ctx->last_workspace <= 0 && world->focused.tag > 0) {
			ctx->last_workspace = world->focused.tag;
		}
	}
	if (world && world->pane_count > 0 && world->panes[0].tail[0]) {
		extract_file_from_text(world->panes[0].tail, ctx->last_file, sizeof(ctx->last_file), &ctx->last_line);
	}
	if (step_output && step_output[0]) {
		extract_file_from_text(step_output, ctx->last_file, sizeof(ctx->last_file), &ctx->last_line);
		if (strcmp(dsl->op, "inspect_browser") == 0) {
			const char *href = strstr(step_output, "href=http");
			const char *u = href ? (href + 5) : strstr(step_output, "https://");
			if (!u) u = strstr(step_output, "http://");
			if (u) {
				size_t ul = 0;
				while (u[ul] && !isspace((unsigned char)u[ul]) &&
				       u[ul] != ')' && u[ul] != ']' && u[ul] != '"' && u[ul] != '\'' && u[ul] != '\\') {
					ul++;
				}
				if (ul > 8 && ul < sizeof(ctx->last_url)) {
					memcpy(ctx->last_url, u, ul);
					ctx->last_url[ul] = '\0';
				}
			}
		}
	}
	if (dsl->target.identifier[0]) {
		snprintf(ctx->last_app, sizeof(ctx->last_app), "%.*s", (int)(sizeof(ctx->last_app) - 1), dsl->target.identifier);
	}
	if (strcmp(dsl->op, "open_terminal") == 0) {
		snprintf(ctx->last_app, sizeof(ctx->last_app), "terminal");
	}
	if (strcmp(dsl->op, "open_url") == 0 && dsl->target.url[0]) {
		snprintf(ctx->last_url, sizeof(ctx->last_url), "%.*s", (int)(sizeof(ctx->last_url) - 1), dsl->target.url);
	}
	if (strcmp(dsl->op, "spawn_shell_command") == 0) {
		snprintf(ctx->last_command, sizeof(ctx->last_command), "%s", dsl->target.command);
		if (strncmp(dsl->target.command, "cd ", 3) == 0) {
			const char *loc = dsl->target.command + 3;
			while (*loc == ' ') loc++;
			snprintf(ctx->last_location, sizeof(ctx->last_location), "%s", loc);
		} else if (strncmp(dsl->target.command, "ls -la ", 7) == 0) {
			const char *loc = dsl->target.command + 7;
			while (*loc == ' ') loc++;
			if (*loc) snprintf(ctx->last_location, sizeof(ctx->last_location), "%s", loc);
		}
	}
	if ((strcmp(dsl->op, "media_play") == 0 || strcmp(dsl->op, "search_web") == 0) && dsl->target.query[0]) {
		snprintf(ctx->last_query, sizeof(ctx->last_query), "%.*s", (int)(sizeof(ctx->last_query) - 1), dsl->target.query);
	}
	if ((strcmp(dsl->op, "workspace_switch") == 0 || strcmp(dsl->op, "workspace_move_window") == 0) && dsl->args.workspace > 0) {
		if (strcmp(dsl->op, "workspace_move_window") == 0 && world && world->focused.tag > 0 &&
		    world->focused.tag != dsl->args.workspace) {
			ctx->prev_workspace = world->focused.tag;
		} else if (ctx->last_workspace > 0 && ctx->last_workspace != dsl->args.workspace) {
			ctx->prev_workspace = ctx->last_workspace;
		} else if (ctx->prev_workspace <= 0) {
			ctx->prev_workspace = (world && world->focused.tag > 0) ? world->focused.tag : 1;
		}
		ctx->last_workspace = dsl->args.workspace;
	}
}

static void record_session_turn(
	jarvis_engine_t *eng,
	const char      *utterance,
	const char      *ops,
	const char      *output,
	const char      *spoken,
	bool             expect_reply
)
{
	if (!eng) return;
	if (!output || strcmp(output, "conversation reset") != 0) {
		if (eng->turn_count >= JARVIS_MAX_TURNS) {
			memmove(&eng->turns[0], &eng->turns[1], (JARVIS_MAX_TURNS - 1) * sizeof(jarvis_session_turn_t));
			eng->turn_count = JARVIS_MAX_TURNS - 1;
		}
		jarvis_session_turn_t *t = &eng->turns[eng->turn_count++];
		memset(t, 0, sizeof(*t));
		t->ts = now_ms() / 1000.0;
		snprintf(t->utterance, sizeof(t->utterance), "%s", utterance ? utterance : "");
		snprintf(t->ops, sizeof(t->ops), "%s", ops ? ops : "");
		snprintf(t->output, sizeof(t->output), "%s", output ? output : "");
		snprintf(t->spoken, sizeof(t->spoken), "%s", spoken ? spoken : "");
		t->expect_reply = expect_reply;
		if (eng->ctx.conv_active) {
			eng->ctx.conv_last_ts = t->ts;
		}
	}
	if (!eng->dry_run) {
		save_context_to_disk(eng);
		save_session_to_disk(eng);
	}
}

static void speak_text_segmented(const char *text)
{
	if (!text || !*text) return;
	if (strlen(text) > 240) {
		char segs[16][JARVIS_STR_LEN];
		size_t n_segs = jarvis_tts_split_segments(text, segs, 16);
		if (n_segs > 0) {
			for (size_t i = 0; i < n_segs; i++) {
				jarvis_xy_speak_clause("oneshot", segs[i], (i + 1 == n_segs) ? 1 : 0);
			}
			return;
		}
	}
	jarvis_xy_speak_clause("oneshot", text, 1);
}

static bool is_observation_op(const char *op)
{
	return strcmp(op, "inspect_terminal") == 0 ||
	       strcmp(op, "inspect_browser") == 0 ||
	       strcmp(op, "inspect_clipboard") == 0 ||
	       strcmp(op, "inspect_system") == 0 ||
	       strcmp(op, "run_background_command") == 0 ||
	       strcmp(op, "recall_facts") == 0;
}

static void execute_dsl_via_xylem(
	jarvis_engine_t         *eng,
	const jarvis_dsl_t      *dsl,
	jarvis_adapter_result_t *out_res
)
{
	memset(out_res, 0, sizeof(*out_res));

	if (strcmp(dsl->op, "conversation_control") == 0) {
		const char *act = dsl->args.action[0] ? dsl->args.action : "enter";
		out_res->handled = true;
		out_res->success = true;
		if (strcasecmp(act, "exit") == 0 || strcasecmp(act, "off") == 0 || strcasecmp(act, "stop") == 0) {
			if (eng) {
				eng->ctx.conv_active = false;
				eng->ctx.conv_last_ts = 0.0;
			}
			snprintf(out_res->output, sizeof(out_res->output), "conversation ended");
		} else if (strcasecmp(act, "reset") == 0 || strcasecmp(act, "clear") == 0) {
			if (eng) {
				eng->turn_count = 0;
				eng->ctx.conv_active = false;
				eng->ctx.conv_last_ts = 0.0;
				eng->ctx.last_op[0] = '\0';
				eng->ctx.last_command[0] = '\0';
				eng->ctx.last_query[0] = '\0';
			}
			snprintf(out_res->output, sizeof(out_res->output), "conversation reset");
		} else {
			if (eng) {
				eng->ctx.conv_active = true;
				eng->ctx.conv_last_ts = now_ms() / 1000.0;
			}
			snprintf(out_res->output, sizeof(out_res->output), "conversation started");
		}
		return;
	}
	if (strcmp(dsl->op, "remember_fact") == 0) {
		const char *k = dsl->args.key[0] ? dsl->args.key : "note";
		const char *v = dsl->args.value;
		jarvis_memory_remember(eng, k, v);
		out_res->handled = true;
		out_res->success = true;
		snprintf(out_res->output, sizeof(out_res->output), "Remembered '%s': %s", k, v);
		return;
	}
	if (strcmp(dsl->op, "recall_facts") == 0) {
		jarvis_memory_recall_json(eng, dsl->target.query, out_res->output, sizeof(out_res->output));
		out_res->handled = true;
		out_res->success = true;
		return;
	}

	/* Destructive command safety guardrail */
	if (strcmp(dsl->op, "spawn_shell_command") == 0 || strcmp(dsl->op, "run_background_command") == 0) {
		if (jarvis_is_destructive_command(dsl->target.command)) {
			int confirmed = jarvis_xy_confirm(dsl->target.command);
			if (xy_errno() == XY_ERR_NOTFOUND) {
				/* If no confirm hook is registered, default to confirmed unless overridden */
				confirmed = 1;
			}
			if (confirmed <= 0) {
				out_res->handled = true;
				out_res->success = false;
				snprintf(out_res->error, sizeof(out_res->error), "Cancelled destructive command: %.440s", dsl->target.command);
				return;
			}
		}
	}

	if (eng && eng->dry_run && !is_observation_op(dsl->op)) {
		out_res->handled = true;
		out_res->success = true;
		snprintf(out_res->output, sizeof(out_res->output), "dry-run:%s", dsl->op);
		return;
	}

#if !defined(__wasm__) && !defined(_WIN32)
	extern int jarvis_desktop_execute_fallback(const jarvis_dsl_t *d, jarvis_adapter_result_t *r);
#endif

	/* Dispatch to loaded Xylem adapter modules */
	jarvis_xy_execute(dsl, out_res);
	if (!eng || !eng->dry_run) {
		if (strcmp(dsl->op, "tts_interrupt") == 0) {
			jarvis_xy_tts_interrupt(0);
			out_res->handled = true;
			out_res->success = true;
			if (!out_res->output[0]) {
				snprintf(out_res->output, sizeof(out_res->output), "stopped speaking");
			}
		} else if (strcmp(dsl->op, "tts_control") == 0) {
			const char *act = dsl->args.action[0] ? dsl->args.action : "on";
			bool turn_on = (strcasecmp(act, "off") != 0 && strcasecmp(act, "0") != 0 && strcasecmp(act, "disable") != 0);
			if (!turn_on) {
				jarvis_xy_tts_interrupt(0);
			}
		} else if (strcmp(dsl->op, "answer_question") == 0 && !dsl->args.streamed_tts) {
			const char *ans = dsl->args.answer[0] ? dsl->args.answer :
			                  (dsl->args.text[0] ? dsl->args.text : dsl->target.query);
			if (ans && ans[0]) {
				speak_text_segmented(ans);
			}
		}
	}
#if !defined(__wasm__) && !defined(_WIN32)
	if (!out_res->handled) {
		jarvis_desktop_execute_fallback(dsl, out_res);
	}
#endif
	if (!out_res->handled) {
		/* Built-in fallback if no XY module claimed the operation */
		out_res->handled = true;
		out_res->success = true;
		if (strcmp(dsl->op, "answer_question") == 0) {
			snprintf(out_res->output, sizeof(out_res->output), "%s",
			         dsl->args.answer[0] ? dsl->args.answer : "answered");
		} else {
			snprintf(out_res->output, sizeof(out_res->output), "executed:%s", dsl->op);
		}
	}

	/* If the LLM attached an inline conversational reply alongside an action, notify/speak it */
	if (dsl->args.reply[0] && strcmp(dsl->op, "answer_question") != 0 && (!eng || !eng->dry_run)) {
		jarvis_dsl_t reply_dsl;
		memset(&reply_dsl, 0, sizeof(reply_dsl));
		snprintf(reply_dsl.op, sizeof(reply_dsl.op), "answer_question");
		snprintf(reply_dsl.args.answer, sizeof(reply_dsl.args.answer), "%s", dsl->args.reply);
		reply_dsl.args.speak = dsl->args.speak;
		reply_dsl.args.streamed_tts = !dsl->args.speak;
		jarvis_adapter_result_t reply_res;
		memset(&reply_res, 0, sizeof(reply_res));
		jarvis_xy_execute(&reply_dsl, &reply_res);
		if (!reply_dsl.args.streamed_tts) {
			speak_text_segmented(reply_dsl.args.answer);
		}
	}
}

/* ---------------------------------------------------------------------------
 * Remote Inference via axil-auren (:4242)
 * ------------------------------------------------------------------------- */

#ifndef __wasm__
static bool http_post_json(
	const char *base_url,
	const char *path,
	const char *json_body,
	char       *out_resp,
	size_t      resp_sz
)
{
	if (!out_resp || resp_sz == 0) return false;
	out_resp[0] = '\0';

	/* Parse host and port from base_url (e.g. "http://127.0.0.1:4242") */
	char host[128] = "127.0.0.1";
	int port = 4242;
	if (base_url && strncmp(base_url, "http://", 7) == 0) {
		const char *hp = base_url + 7;
		const char *colon = strchr(hp, ':');
		const char *slash = strchr(hp, '/');
		if (colon && (!slash || colon < slash)) {
			size_t hlen = (size_t)(colon - hp);
			if (hlen < sizeof(host)) {
				memcpy(host, hp, hlen);
				host[hlen] = '\0';
			}
			port = atoi(colon + 1);
		}
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return false;

	struct timeval tv;
	tv.tv_sec = 14;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
		struct hostent *he = gethostbyname(host);
		if (!he || !he->h_addr_list[0]) {
			close(fd);
			return false;
		}
		memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return false;
	}

	size_t body_len = strlen(json_body);
	char hdr[512];
	int hlen = snprintf(
		hdr, sizeof(hdr),
		"POST %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n\r\n",
		path, host, port, body_len
	);

	if (send(fd, hdr, (size_t)hlen, 0) < 0 || send(fd, json_body, body_len, 0) < 0) {
		close(fd);
		return false;
	}

	size_t cap = 16384;
	size_t total = 0;
	char *raw = (char *)malloc(cap);
	if (!raw) {
		close(fd);
		return false;
	}

	for (;;) {
		if (total + 2048 >= cap) {
			cap *= 2;
			char *nr = (char *)realloc(raw, cap);
			if (!nr) break;
			raw = nr;
		}
		ssize_t r = recv(fd, raw + total, cap - total - 1, 0);
		if (r <= 0) break;
		total += (size_t)r;
	}
	raw[total] = '\0';
	close(fd);

	const char *body = strstr(raw, "\r\n\r\n");
	if (body) body += 4;
	else body = raw;

	/* Handle chunked transfer encoding if present */
	if (strstr(raw, "Transfer-Encoding: chunked") || strstr(raw, "transfer-encoding: chunked")) {
		size_t w = 0;
		const char *p = body;
		while (*p && w + 1 < resp_sz) {
			char *endp = NULL;
			unsigned long csz = strtoul(p, &endp, 16);
			if (endp == p || csz == 0) break;
			const char *crlf = strstr(endp, "\r\n");
			if (!crlf) break;
			p = crlf + 2;
			size_t to_copy = csz;
			if (w + to_copy >= resp_sz) to_copy = resp_sz - w - 1;
			memcpy(out_resp + w, p, to_copy);
			w += to_copy;
			p += csz;
			if (p[0] == '\r' && p[1] == '\n') p += 2;
		}
		out_resp[w] = '\0';
	} else {
		snprintf(out_resp, resp_sz, "%s", body);
	}

	free(raw);
	return out_resp[0] != '\0';
}

/*
 * SSE Streaming LLM client with real-time TTS clause synthesis:
 * Sends a `"stream":true` request to axil-auren (:4242/v1/chat/completions),
 * incrementally unescapes SSE `delta.content` chunks as they arrive over the socket,
 * extracts `"answer"` or `"reply"` prefixes on the fly, and dispatches complete
 * clauses to `jarvis_xy_speak_clause()` BEFORE the full LLM response finishes.
 */
static bool http_post_json_sse_stream(
	const char *base_url,
	const char *path,
	const char *json_body,
	bool        enable_live_tts,
	char       *out_content,
	size_t      content_sz,
	bool       *out_streamed_tts
)
{
	if (out_streamed_tts) *out_streamed_tts = false;
	if (!out_content || content_sz == 0) return false;
	out_content[0] = '\0';

	char host[128] = "127.0.0.1";
	int port = 4242;
	if (base_url && strncmp(base_url, "http://", 7) == 0) {
		const char *hp = base_url + 7;
		const char *colon = strchr(hp, ':');
		const char *slash = strchr(hp, '/');
		if (colon && (!slash || colon < slash)) {
			size_t hlen = (size_t)(colon - hp);
			if (hlen < sizeof(host)) {
				memcpy(host, hp, hlen);
				host[hlen] = '\0';
			}
			port = atoi(colon + 1);
		}
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return false;

	struct timeval tv;
	tv.tv_sec = 14;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
		struct hostent *he = gethostbyname(host);
		if (!he || !he->h_addr_list[0]) {
			close(fd);
			return false;
		}
		memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return false;
	}

	size_t body_len = strlen(json_body);
	char hdr[512];
	int hlen = snprintf(
		hdr, sizeof(hdr),
		"POST %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Content-Type: application/json\r\n"
		"Accept: text/event-stream\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n\r\n",
		path, host, port, body_len
	);

	if (send(fd, hdr, (size_t)hlen, 0) < 0 || send(fd, json_body, body_len, 0) < 0) {
		close(fd);
		return false;
	}

	size_t cap = 32768;
	size_t total = 0;
	char *raw = (char *)malloc(cap);
	if (!raw) {
		close(fd);
		return false;
	}

	bool headers_done = false;
	bool is_sse = false;
	size_t line_cursor = 0;
	size_t content_len = 0;
	size_t spoken_offset = 0;
	bool is_first_clause = true;
	bool did_stream_tts = false;

	for (;;) {
		if (total + 4096 >= cap) {
			cap *= 2;
			char *nr = (char *)realloc(raw, cap);
			if (!nr) break;
			raw = nr;
		}
		ssize_t r = recv(fd, raw + total, cap - total - 1, 0);
		if (r <= 0) break;
		total += (size_t)r;
		raw[total] = '\0';

		if (!headers_done) {
			const char *hdr_end = strstr(raw, "\r\n\r\n");
			if (!hdr_end) continue;
			headers_done = true;
			line_cursor = (size_t)(hdr_end - raw) + 4;
			if (strstr(raw, "text/event-stream") || strstr(raw + line_cursor, "data: ")) {
				is_sse = true;
			}
		}

		if (!is_sse && strstr(raw + line_cursor, "data: ")) {
			is_sse = true;
		}

		while (is_sse && line_cursor < total) {
			char *nl = strchr(raw + line_cursor, '\n');
			if (!nl) break;
			size_t line_len = (size_t)(nl - (raw + line_cursor));
			char line[8192];
			if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
			memcpy(line, raw + line_cursor, line_len);
			line[line_len] = '\0';
			line_cursor = (size_t)(nl - raw) + 1;

			while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) {
				line[--line_len] = '\0';
			}
			const char *dp = strstr(line, "data:");
			if (!dp) continue;
			dp += 5;
			while (*dp == ' ') dp++;
			if (strcmp(dp, "[DONE]") == 0) break;

			jjson_val_t *cv = jjson_parse(dp);
			if (cv) {
				const jjson_val_t *choices = jjson_obj_get(cv, "choices");
				if (choices && choices->type == JJSON_ARRAY && choices->u.arr.count > 0) {
					const jjson_val_t *delta = jjson_obj_get(choices->u.arr.items[0], "delta");
					if (!delta) delta = jjson_obj_get(choices->u.arr.items[0], "message");
					const char *piece = jjson_obj_get_str(delta, "content", NULL);
					if (piece && *piece) {
						size_t plen = strlen(piece);
						if (content_len + plen + 1 < content_sz) {
							memcpy(out_content + content_len, piece, plen);
							content_len += plen;
							out_content[content_len] = '\0';
						}
					}
				}
				jjson_free(cv);
			}

			if (enable_live_tts && content_len > 16) {
				char full_ans[2048];
				bool ans_closed = false;
				if (extract_streaming_answer_or_reply(out_content, full_ans, sizeof(full_ans), &ans_closed)) {
					size_t ans_len = strlen(full_ans);
					while (spoken_offset < ans_len) {
						const char *unspoken = full_ans + spoken_offset;
						while (*unspoken && isspace((unsigned char)*unspoken)) {
							unspoken++;
							spoken_offset++;
						}
						if (!*unspoken) break;

						char clause[512], rem[2048];
						if (jarvis_pop_ready_tts_clause(unspoken, is_first_clause, clause, sizeof(clause), rem, sizeof(rem))) {
							jarvis_xy_speak_clause("stream", clause, 0);
							did_stream_tts = true;
							is_first_clause = false;
							size_t rem_len = strlen(rem);
							spoken_offset = ans_len >= rem_len ? (ans_len - rem_len) : ans_len;
						} else if (ans_closed) {
							jarvis_xy_speak_clause("stream", unspoken, 1);
							did_stream_tts = true;
							is_first_clause = false;
							spoken_offset = ans_len;
							break;
						} else {
							break;
						}
					}
				}
			}
		}
	}
	close(fd);

	/* Fallback if server returned non-SSE single JSON response */
	if (!is_sse && content_len == 0 && total > 0) {
		const char *body = strstr(raw, "\r\n\r\n");
		body = body ? (body + 4) : raw;
		jjson_val_t *rv = jjson_parse(body);
		if (rv) {
			const jjson_val_t *choices = jjson_obj_get(rv, "choices");
			if (choices && choices->type == JJSON_ARRAY && choices->u.arr.count > 0) {
				const jjson_val_t *msg = jjson_obj_get(choices->u.arr.items[0], "message");
				const char *c = jjson_obj_get_str(msg, "content", NULL);
				if (!c || !*c) c = jjson_obj_get_str(msg, "reasoning_content", NULL);
				if (c && *c) {
					snprintf(out_content, content_sz, "%s", c);
					content_len = strlen(out_content);
				}
			}
			jjson_free(rv);
		}
	}

	/* Flush any remaining unspoken tail of "answer"/"reply" once the stream ends */
	if (enable_live_tts && content_len > 0) {
		char full_ans[2048];
		if (extract_streaming_answer_or_reply(out_content, full_ans, sizeof(full_ans), NULL)) {
			size_t ans_len = strlen(full_ans);
			if (spoken_offset < ans_len) {
				const char *tail = full_ans + spoken_offset;
				while (*tail && isspace((unsigned char)*tail)) tail++;
				if (*tail) {
					jarvis_xy_speak_clause("stream", tail, 1);
					did_stream_tts = true;
				}
			}
		}
	}

	if (out_streamed_tts) *out_streamed_tts = did_stream_tts;
	free(raw);
	return out_content[0] != '\0';
}
#endif

static void summarize_world_for_prompt(
	const jarvis_engine_t *eng,
	const jarvis_world_t  *world,
	char                  *out_sum,
	size_t                 sum_sz
)
{
	int n = 0;
	out_sum[0] = '\0';
	if (world) {
		if (world->focused.id[0] || world->focused.wm_class[0]) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Focused window: class=%s, title=%s, id=%s\n",
			              world->focused.wm_class, world->focused.title, world->focused.id);
		}
		if (world->focused_cwd[0]) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Focused terminal working directory (cwd): %s\n", world->focused_cwd);
		}
		if (world->focused_process[0]) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Focused terminal active foreground command: %s\n", world->focused_process);
		}
		if (world->system_health[0]) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Live System Health (RAM / Disk / CPU): %s\n", world->system_health);
		}
		if (world->pane_count > 0) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Live Terminal / Tmux Panes (recent visible output & active process):\n");
			for (size_t i = 0; i < world->pane_count && i < 3; i++) {
				const jarvis_pane_t *p = &world->panes[i];
				n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
				              "  - Pane %s [active=%s, cmd=%s, cwd=%s]: %.360s\n",
				              p->pane, p->active ? "true" : "false", p->command, p->cwd, p->tail);
			}
		}
		if (world->other.id[0]) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Other window: class=%s, title=%s, id=%s\n",
			              world->other.wm_class, world->other.title, world->other.id);
		}
		n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
		              "Open windows (%zu):\n", world->client_count);
		for (size_t i = 0; i < world->client_count && i < 8; i++) {
			const jarvis_window_t *c = &world->clients[i];
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "  - [%s] %s: %.50s (tag=%d, visible=%s)\n",
			              c->id, c->wm_class, c->title, c->tag, c->visible ? "true" : "false");
		}
		if (world->tab_count > 0) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Browser tabs (%zu):\n", world->tab_count);
			for (size_t i = 0; i < world->tab_count && i < 5; i++) {
				n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
				              "  - %.50s (%.60s)\n", world->tabs[i].title, world->tabs[i].url);
			}
		}
		if (world->browser_page[0]) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "Live Browser Accessibility Page Content:\n  %.750s\n", world->browser_page);
		}
	}
#ifndef __wasm__
	{
		static const char *const cfg_cands[] = {
			"~/.jarvisrc", "~/.vimrc", "~/.config/nvim/init.lua", "~/.config/nvim/init.vim",
			"~/.bashrc", "~/.zshrc", "~/.tmux.conf", "~/.Xresources",
			"~/.config/jarvis/ontology.json", NULL
		};
		const char *home = getenv("HOME");
		if (home && *home) {
			bool any_cfg = false;
			for (size_t i = 0; cfg_cands[i]; i++) {
				char full[512];
				snprintf(full, sizeof(full), "%s%s", home, cfg_cands[i] + 1);
				if (access(full, F_OK) == 0) {
					if (!any_cfg) {
						n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
						              "Existing user config paths: ");
						any_cfg = true;
					} else {
						n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0, ", ");
					}
					n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0, "%s", cfg_cands[i]);
				}
			}
			if (any_cfg) {
				n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0, "\n");
			}
		}
		time_t now_t = time(NULL);
		struct tm tm_now;
		if (localtime_r(&now_t, &tm_now)) {
			char tbuf[128];
			if (strftime(tbuf, sizeof(tbuf), "%A, %B %d, %Y %H:%M:%S %Z", &tm_now) > 0) {
				n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
				              "Current local date & time: %s\n", tbuf);
			}
		}
	}
#endif

	if (eng && eng->ctx.last_op[0]) {
		n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
		              "Recent action context: last_op=%s, last_command=%s, last_app=%s, last_query=%s, last_location=%s, last_window=%s (%s), last_workspace=%d, prev_workspace=%d, last_file=%s, last_url=%s\n",
		              eng->ctx.last_op, eng->ctx.last_command, eng->ctx.last_app,
		              eng->ctx.last_query, eng->ctx.last_location,
		              eng->ctx.last_window_id, eng->ctx.last_window_class,
		              eng->ctx.last_workspace, eng->ctx.prev_workspace,
		              eng->ctx.last_file, eng->ctx.last_url);
	}
	if (eng && eng->memory_count > 0) {
		n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
		              "Learned User Preferences & Memories: ");
		for (size_t i = 0; i < eng->memory_count && i < 15; i++) {
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "%s%s: %s", i ? " | " : "", eng->memories[i].key, eng->memories[i].value);
		}
		n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0, "\n");
	}
}

typedef struct {
	char  *buf;
	size_t len;
	size_t cap;
} jarvis_sb_t;

static bool sb_ensure(jarvis_sb_t *sb, size_t extra)
{
	if (!sb) return false;
	if (sb->len + extra + 1 <= sb->cap) return true;
	size_t ncap = sb->cap ? sb->cap : 4096;
	while (ncap < sb->len + extra + 1) ncap *= 2;
	char *nb = (char *)realloc(sb->buf, ncap);
	if (!nb) return false;
	sb->buf = nb;
	sb->cap = ncap;
	return true;
}

static bool sb_append_n(jarvis_sb_t *sb, const char *s, size_t n)
{
	if (!s || n == 0) return true;
	if (!sb_ensure(sb, n)) return false;
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
	return true;
}

static bool sb_append(jarvis_sb_t *sb, const char *s)
{
	return s ? sb_append_n(sb, s, strlen(s)) : true;
}

static bool sb_append_json_str(jarvis_sb_t *sb, const char *s)
{
	if (!s) return true;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		unsigned char c = *p;
		if (c == '"' || c == '\\') {
			char esc[2] = {'\\', (char)c};
			if (!sb_append_n(sb, esc, 2)) return false;
		} else if (c == '\n') {
			if (!sb_append_n(sb, "\\n", 2)) return false;
		} else if (c == '\r') {
			if (!sb_append_n(sb, "\\r", 2)) return false;
		} else if (c == '\t') {
			if (!sb_append_n(sb, "\\t", 2)) return false;
		} else if (c < 0x20) {
			char ubuf[8];
			snprintf(ubuf, sizeof(ubuf), "\\u%04x", (unsigned int)c);
			if (!sb_append(sb, ubuf)) return false;
		} else {
			if (!sb_append_n(sb, (const char *)&c, 1)) return false;
		}
	}
	return true;
}

char *jarvis_build_chat_messages_json(
	const jarvis_engine_t *eng,
	const char            *model,
	const char            *sys_prompt,
	const char            *user_msg0,
	const char             assistant_turns[][4096],
	const char             user_obs_turns[][4096],
	int                    current_turn,
	double                 now_s,
	double                 history_ttl_s,
	size_t                 max_history_turns
)
{
	jarvis_sb_t sb = {0};
	if (!sb_ensure(&sb, 8192)) return NULL;
	sb.buf[0] = '\0';

	if (!sb_append(&sb, "{\"model\":\"") ||
	    !sb_append_json_str(&sb, (model && *model) ? model : "fast") ||
	    !sb_append(&sb, "\",\"stream\":true,\"temperature\":0.0,\"max_tokens\":512,\"messages\":[")) {
		free(sb.buf);
		return NULL;
	}

	if (!sb_append(&sb, "{\"role\":\"system\",\"content\":\"") ||
	    !sb_append_json_str(&sb, sys_prompt ? sys_prompt : "") ||
	    !sb_append(&sb, "\"}")) {
		free(sb.buf);
		return NULL;
	}

	if (eng && eng->turn_count > 0) {
		size_t eligible[JARVIS_MAX_TURNS];
		size_t n_elig = 0;
		for (size_t i = 0; i < eng->turn_count && i < JARVIS_MAX_TURNS; i++) {
			const jarvis_session_turn_t *t = &eng->turns[i];
			if (!t->utterance[0]) continue;
			if (history_ttl_s > 0.0 && t->ts > 0.0 && now_s > t->ts && (now_s - t->ts) > history_ttl_s) {
				continue;
			}
			eligible[n_elig++] = i;
		}
		size_t start_pos = 0;
		if (max_history_turns > 0 && n_elig > max_history_turns) {
			start_pos = n_elig - max_history_turns;
		}
		for (size_t k = start_pos; k < n_elig; k++) {
			const jarvis_session_turn_t *t = &eng->turns[eligible[k]];
			jarvis_sb_t asb = {0};
			bool is_ans = (strcmp(t->ops, "answer_question") == 0);
			if (is_ans) {
				const char *ans = t->spoken[0] ? t->spoken : t->output;
				if (!sb_append(&asb, "{\"op\":\"answer_question\",\"args\":{\"answer\":\"") ||
				    !sb_append_json_str(&asb, ans) ||
				    !sb_append(&asb, "\"") ||
				    (t->expect_reply && !sb_append(&asb, ",\"expect_reply\":true")) ||
				    !sb_append(&asb, "}}")) {
					free(asb.buf);
					free(sb.buf);
					return NULL;
				}
			} else {
				const char *op_str = t->ops[0] ? t->ops : "action";
				if (!sb_append(&asb, "{\"op\":\"") ||
				    !sb_append_json_str(&asb, op_str) ||
				    !sb_append(&asb, "\"") ||
				    (t->output[0] && (!sb_append(&asb, ",\"result\":\"") || !sb_append_json_str(&asb, t->output) || !sb_append(&asb, "\""))) ||
				    (t->spoken[0] && (!sb_append(&asb, ",\"spoken\":\"") || !sb_append_json_str(&asb, t->spoken) || !sb_append(&asb, "\""))) ||
				    (t->expect_reply && !sb_append(&asb, ",\"expect_reply\":true")) ||
				    !sb_append(&asb, "}")) {
					free(asb.buf);
					free(sb.buf);
					return NULL;
				}
			}

			if (!sb_append(&sb, ",{\"role\":\"user\",\"content\":\"") ||
			    !sb_append_json_str(&sb, t->utterance) ||
			    !sb_append(&sb, "\"},{\"role\":\"assistant\",\"content\":\"") ||
			    !sb_append_json_str(&sb, asb.buf ? asb.buf : "{}") ||
			    !sb_append(&sb, "\"}")) {
				free(asb.buf);
				free(sb.buf);
				return NULL;
			}
			free(asb.buf);
		}
	}

	if (!sb_append(&sb, ",{\"role\":\"user\",\"content\":\"") ||
	    !sb_append_json_str(&sb, user_msg0 ? user_msg0 : "") ||
	    !sb_append(&sb, "\"}")) {
		free(sb.buf);
		return NULL;
	}

	for (int pt = 0; pt < current_turn; pt++) {
		if (!sb_append(&sb, ",{\"role\":\"assistant\",\"content\":\"") ||
		    !sb_append_json_str(&sb, assistant_turns[pt]) ||
		    !sb_append(&sb, "\"},{\"role\":\"user\",\"content\":\"") ||
		    !sb_append_json_str(&sb, user_obs_turns[pt]) ||
		    !sb_append(&sb, "\"}")) {
			free(sb.buf);
			return NULL;
		}
	}

	if (!sb_append(&sb, "]}")) {
		free(sb.buf);
		return NULL;
	}
	return sb.buf;
}

static bool extract_first_url_from_text(const char *text, char *out_url, size_t url_sz)
{
	if (!text || !out_url || url_sz == 0) return false;
	const char *http = strstr(text, "http://");
	const char *https = strstr(text, "https://");
	const char *file = strstr(text, "file://");
	const char *best = NULL;
	if (https) best = https;
	if (http && (!best || http < best)) best = http;
	if (file && (!best || file < best)) best = file;
	if (!best) return false;

	size_t u = 0;
	while (best[u] && !isspace((unsigned char)best[u]) &&
	       best[u] != '"' && best[u] != '\'' && best[u] != '\\' &&
	       best[u] != ')' && best[u] != ']' && best[u] != '>' &&
	       u + 1 < url_sz) {
		out_url[u] = best[u];
		u++;
	}
	while (u > 0 && (out_url[u - 1] == '.' || out_url[u - 1] == ',' || out_url[u - 1] == ';')) {
		u--;
	}
	out_url[u] = '\0';
	return u > 7;
}

static bool padded_contains_rule_phrase(const char *padded_hay, const char *phrase)
{
	if (!phrase || !*phrase) return false;
	char probe[JARVIS_ID_LEN + 4];
	snprintf(probe, sizeof(probe), " %s ", phrase);
	return strstr(padded_hay, probe) != NULL;
}

static bool match_react_rule(
	const jarvis_react_rule_t *rule,
	const char                *padded_utt,
	int                        turn,
	const char                *user_obs_turn1
)
{
	if (!rule || !rule->kind[0]) return false;
	if (rule->turn_filter == 0 && turn != 0) return false;
	if (rule->turn_filter == 1 && turn <= 0) return false;
	if (rule->obs_op[0]) {
		char obs_tag[128];
		snprintf(obs_tag, sizeof(obs_tag), "OBSERVATION (%s)", rule->obs_op);
		if (!user_obs_turn1 || !strstr(user_obs_turn1, obs_tag)) return false;
	}

	if (rule->phrase_count > 0) {
		bool phrase_ok = false;
		for (size_t i = 0; i < rule->phrase_count; i++) {
			if (padded_contains_rule_phrase(padded_utt, rule->phrases[i])) {
				phrase_ok = true;
				break;
			}
		}
		if (!phrase_ok) return false;
	}

	if (rule->require_any_count > 0) {
		bool req_ok = false;
		for (size_t i = 0; i < rule->require_any_count; i++) {
			if (padded_contains_rule_phrase(padded_utt, rule->require_any[i])) {
				req_ok = true;
				break;
			}
		}
		if (!req_ok) return false;
	}

	if (rule->forbid_any_count > 0) {
		for (size_t i = 0; i < rule->forbid_any_count; i++) {
			if (padded_contains_rule_phrase(padded_utt, rule->forbid_any[i])) {
				return false;
			}
		}
	}
	return true;
}

static bool rule_has_line_trigger(const jarvis_react_rule_t *rule, const char *padded_utt)
{
	if (!rule) return false;
	for (size_t i = 0; i < rule->line_trigger_count; i++) {
		if (padded_contains_rule_phrase(padded_utt, rule->line_triggers[i])) {
			return true;
		}
	}
	return false;
}

static void extract_obs_body(const char *obs_str, char *out_esc, size_t out_sz)
{
	if (!out_esc || out_sz == 0) return;
	out_esc[0] = '\0';
	if (!obs_str) return;
	const char *colon = strchr(obs_str, ':');
	const char *body = colon ? (colon + 1) : obs_str;
	while (*body && isspace((unsigned char)*body)) body++;
	jjson_escape_str(body, out_esc, out_sz);
}

static bool synthesize_offline_react_json(
	const jarvis_engine_t *eng,
	const char            *utterance,
	const jarvis_world_t  *world,
	int                    turn,
	const char            *user_obs_turn1,
	char                  *out_json,
	size_t                 out_sz
)
{
	if (!utterance || !out_json || out_sz == 0) return false;
	out_json[0] = '\0';

	const jarvis_ontology_t *ont = (eng && eng->ont) ? eng->ont : jarvis_ontology_get_active();
	char clean[512];
	jarvis_clean_phrase(utterance, clean, sizeof(clean));
	char padded[520];
	snprintf(padded, sizeof(padded), " %s ", clean);

	/* 1. Evaluate declarative ontology react_rules (turn-0 & turn-1 observations, follow-ups, memory, shell templates) */
	if (ont) {
		for (size_t ri = 0; ri < ont->react_rule_count; ri++) {
			const jarvis_react_rule_t *rule = &ont->react_rules[ri];
			if (!match_react_rule(rule, padded, turn, user_obs_turn1)) continue;

			const char *kind = rule->kind;
			bool in_other = jarvis_ontology_clause_targets_other(ont, utterance);

			if (strcmp(kind, "open_obs_file") == 0) {
				char fpath[128] = {0};
				if (rule->arg[0]) snprintf(fpath, sizeof(fpath), "%s", rule->arg);
				int fline = 0;
				extract_file_from_text(user_obs_turn1, fpath, sizeof(fpath), &fline);
				if (!fpath[0]) return false;
				if (fline > 0 && rule_has_line_trigger(rule, padded)) {
					snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim +%d %s\"}}", fline, fpath);
				} else {
					snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim %s\"}}", fpath);
				}
				return true;
			}
			if (strcmp(kind, "answer_obs") == 0) {
				char esc_obs[384];
				extract_obs_body(user_obs_turn1, esc_obs, sizeof(esc_obs));
				char fpath[128] = {0};
				int fline = 0;
				extract_file_from_text(user_obs_turn1, fpath, sizeof(fpath), &fline);
				if (fpath[0] && fline > 0) {
					snprintf(out_json, out_sz,
					         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"%s:%d — %.220s\"}}",
					         fpath, fline, esc_obs);
				} else {
					snprintf(out_json, out_sz,
					         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"%.260s\"}}",
					         esc_obs);
				}
				return true;
			}
			if (strcmp(kind, "open_obs_url") == 0) {
				char url[256] = {0};
				if (!extract_first_url_from_text(user_obs_turn1, url, sizeof(url)) && rule->arg[0]) {
					snprintf(url, sizeof(url), "%s", rule->arg);
				}
				if (!url[0]) return false;
				snprintf(out_json, out_sz, "{\"op\":\"open_url\",\"target\":{\"url\":\"%s\"}}", url);
				return true;
			}
			if (strcmp(kind, "search_obs_topic") == 0) {
				char esc_obs[128];
				extract_obs_body(user_obs_turn1, esc_obs, sizeof(esc_obs));
				const char *q = rule->arg[0] ? rule->arg : esc_obs;
				snprintf(out_json, out_sz, "{\"op\":\"search_web\",\"target\":{\"query\":\"%.120s\"}}", q);
				return true;
			}
			if (strcmp(kind, "inspect_terminal") == 0) {
				if (!in_other && world && world->pane_count > 0 && world->panes[0].tail[0]) {
					const char *tail = world->panes[0].tail;
					char fpath[128] = {0};
					int fline = 0;
					extract_file_from_text(tail, fpath, sizeof(fpath), &fline);
					if (fpath[0] && fline > 0) {
						char esc_t[256];
						jjson_escape_str(tail, esc_t, sizeof(esc_t));
						snprintf(out_json, out_sz,
						         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"%s:%d — %.180s\"}}",
						         fpath, fline, esc_t);
						return true;
					}
				}
				snprintf(out_json, out_sz, "{\"op\":\"inspect_terminal\",\"target\":{\"relation\":\"%s\"}}",
				         in_other ? "other" : "focused");
				return true;
			}
			if (strcmp(kind, "inspect_clipboard") == 0) {
				snprintf(out_json, out_sz, "{\"op\":\"inspect_clipboard\",\"target\":{}}");
				return true;
			}
			if (strcmp(kind, "inspect_system") == 0) {
				const char *aspect = rule->arg[0] ? rule->arg : "all";
				snprintf(out_json, out_sz, "{\"op\":\"inspect_system\",\"args\":{\"aspect\":\"%s\"}}", aspect);
				return true;
			}
			if (strcmp(kind, "open_browser_url") == 0) {
				if (world && world->browser_page[0]) {
					char url[256] = {0};
					if (extract_first_url_from_text(world->browser_page, url, sizeof(url))) {
						snprintf(out_json, out_sz, "{\"op\":\"open_url\",\"target\":{\"url\":\"%s\"}}", url);
						return true;
					}
				}
				snprintf(out_json, out_sz, "{\"op\":\"inspect_browser\",\"target\":{}}");
				return true;
			}
			if (strcmp(kind, "inspect_browser") == 0) {
				snprintf(out_json, out_sz, "{\"op\":\"inspect_browser\",\"target\":{}}");
				return true;
			}
			if (strcmp(kind, "remember_workflow") == 0) {
				const char *q1 = strchr(utterance, '\'');
				const char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
				char key[128] = "workflow";
				if (q1 && q2 && q2 > q1 + 1) {
					size_t klen = (size_t)(q2 - (q1 + 1));
					if (klen >= sizeof(key)) klen = sizeof(key) - 1;
					memcpy(key, q1 + 1, klen);
					key[klen] = '\0';
				}
				const char *colon = strchr(utterance, ':');
				const char *val = colon ? (colon + 1) : utterance;
				while (*val && isspace((unsigned char)*val)) val++;
				char ek[256], ev[512];
				jjson_escape_str(key, ek, sizeof(ek));
				jjson_escape_str(val, ev, sizeof(ev));
				snprintf(out_json, out_sz, "{\"op\":\"remember_fact\",\"args\":{\"key\":\"%s\",\"value\":\"%s\"}}", ek, ev);
				return true;
			}
			if (strcmp(kind, "undo_workspace") == 0) {
				int prev_ws = (eng && eng->ctx.prev_workspace > 0) ? eng->ctx.prev_workspace : 1;
				const char *op = rule->op[0] ? rule->op :
				                 ((eng && strcmp(eng->ctx.last_op, "workspace_move_window") == 0) ? "workspace_move_window" : "workspace_switch");
				snprintf(out_json, out_sz, "{\"op\":\"%s\",\"args\":{\"workspace\":%d}}", op, prev_ws);
				return true;
			}
			if (strcmp(kind, "correct_workspace") == 0) {
				int ws = jarvis_ontology_extract_workspace_number(ont, utterance, 1);
				const char *op = rule->op[0] ? rule->op :
				                 ((eng && eng->ctx.last_op[0]) ? eng->ctx.last_op : "workspace_move_window");
				snprintf(out_json, out_sz, "{\"op\":\"%s\",\"args\":{\"workspace\":%d}}", op, ws);
				return true;
			}
			if (strcmp(kind, "replay_command") == 0) {
				const char *cmd = (eng && eng->ctx.last_command[0]) ? eng->ctx.last_command : rule->command;
				if (!cmd[0]) return false;
				char esc_cmd[512];
				jjson_escape_str(cmd, esc_cmd, sizeof(esc_cmd));
				snprintf(out_json, out_sz,
				         "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"%s\"%s}}",
				         esc_cmd, in_other ? ",\"relation\":\"other\"" : "");
				return true;
			}
			if (strcmp(kind, "open_context_file") == 0) {
				char file_path[128] = {0};
				int line_no = 0;
				if (world && world->pane_count > 0 && world->panes[0].tail[0]) {
					extract_file_from_text(world->panes[0].tail, file_path, sizeof(file_path), &line_no);
				}
				if (!file_path[0] && eng && eng->ctx.last_file[0]) {
					snprintf(file_path, sizeof(file_path), "%.*s", (int)(sizeof(file_path) - 1), eng->ctx.last_file);
				}
				if (!file_path[0] && eng && eng->turn_count > 0) {
					extract_file_from_text(eng->turns[eng->turn_count - 1].output, file_path, sizeof(file_path), &line_no);
				}
				if (line_no <= 0 && eng && eng->ctx.last_line > 0) {
					line_no = eng->ctx.last_line;
				}
				if (!file_path[0] && rule->arg[0]) {
					snprintf(file_path, sizeof(file_path), "%s", rule->arg);
				}
				if (!file_path[0]) return false;
				if (line_no > 0 && rule_has_line_trigger(rule, padded)) {
					snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim +%d %s\"}}", line_no, file_path);
				} else {
					snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim %s\"}}", file_path);
				}
				return true;
			}
			if (strcmp(kind, "shell_command") == 0) {
				char esc_cmd[512];
				jjson_escape_str(rule->command, esc_cmd, sizeof(esc_cmd));
				snprintf(out_json, out_sz,
				         "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"%s\"%s}}",
				         esc_cmd, in_other ? ",\"relation\":\"other\"" : "");
				return true;
			}
			if (strcmp(kind, "answer_time") == 0) {
				char tbuf[128] = "00:00";
#ifndef __wasm__
				time_t now_t = time(NULL);
				struct tm tm_now;
				if (localtime_r(&now_t, &tm_now)) {
					strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_now);
				}
#endif
				snprintf(out_json, out_sz,
				         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"%s\"}}", tbuf);
				return true;
			}
			if (strcmp(kind, "answer_cwd") == 0) {
				const char *cwd = (world && world->focused_cwd[0]) ? world->focused_cwd :
				                  (rule->arg[0] ? rule->arg : ".");
				snprintf(out_json, out_sz,
				         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"%s\"}}", cwd);
				return true;
			}
			if (strcmp(kind, "wm_help") == 0) {
				char esc_q[256];
				jjson_escape_str(rule->arg[0] ? rule->arg : clean, esc_q, sizeof(esc_q));
				snprintf(out_json, out_sz,
				         "{\"op\":\"wm_help\",\"target\":{\"query\":\"%s\"}}", esc_q);
				return true;
			}
		}
	}

	/* 2. Invoke a remembered user workflow if the utterance references a stored memory key */
	if (eng && eng->memory_count > 0) {
		for (size_t i = 0; i < eng->memory_count; i++) {
			char mk_clean[128];
			if (jarvis_clean_phrase(eng->memories[i].key, mk_clean, sizeof(mk_clean)) >= 3 &&
			    strstr(clean, mk_clean)) {
				char wf_clauses[JARVIS_MAX_STEPS][512];
				size_t n_wf = jarvis_split_clauses(ont, eng->memories[i].value, wf_clauses, JARVIS_MAX_STEPS);
				int pos = snprintf(out_json, out_sz, "[");
				size_t emitted = 0;
				for (size_t ci = 0; ci < n_wf; ci++) {
					jarvis_ir_t ir;
					jarvis_dsl_t dsl;
					if (jarvis_solve_clause(ont, wf_clauses[ci], "any", world, &ir) &&
					    jarvis_compile_ir_to_dsl(&ir, "any", &eng->ctx, world, &dsl)) {
						char dj[1024];
						jarvis_dsl_to_json(&dsl, dj, sizeof(dj));
						pos += snprintf(out_json + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0,
						                "%s%s", emitted ? "," : "", dj);
						emitted++;
					}
				}
				if (emitted > 0) {
					snprintf(out_json + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0, "]");
					return true;
				}
			}
		}
	}

	/* 3. Delegate to the language-agnostic Tier-0 ontology solver & idiom table */
	if (ont) {
		jarvis_ir_t ir;
		jarvis_dsl_t dsl;
		if (jarvis_solve_clause(ont, utterance, "any", world, &ir) &&
		    jarvis_compile_ir_to_dsl(&ir, "any", eng ? &eng->ctx : NULL, world, &dsl)) {
			jarvis_dsl_to_json(&dsl, out_json, out_sz);
			return out_json[0] != '\0';
		}
	}

	return false;
}

bool jarvis_auren_run_react_agent(
	jarvis_engine_t         *eng,
	const char              *raw_utterance,
	const jarvis_world_t    *world,
	jarvis_process_result_t *out_res
)
{
	if (!eng || !raw_utterance || !out_res) return false;

	char utterance[512];
	snprintf(utterance, sizeof(utterance), "%s", raw_utterance);

	const char *lang_name = jarvis_ontology_get_lang_name(eng->ont);
	char sys_prompt[2048];
	snprintf(
		sys_prompt, sizeof(sys_prompt),
		"You are Jarvis, Tony Stark's intelligent Linux desktop assistant and systems engineer. "
		"You control a live Linux desktop (window manager, terminals, tmux, browser, audio, shell, and coding tools). "
		"You operate in a ReAct Observe-Reason-Act loop: you can either call an OBSERVATION tool first to inspect "
		"live terminal panes, browser accessibility page content, clipboard, system health, or command output, OR immediately emit one or more ACTION DSL objects (or a JSON array of DSL objects) to fulfill the user's intent. "
		"Previous conversation turns appear as alternating user/assistant messages before the current request; use them naturally for follow-ups, pronouns, clarifications, and ongoing discussion. "
		"When using `answer_question`, keep your spoken reply crisp, natural, and concise (1 to 3 short sentences, under 450 characters) in %s (or the language of the user's request). "
		"If you ask the user a clarifying or follow-up question and want them to reply, include \"expect_reply\":true in \"args\". "
		"Always output ONLY valid JSON (either a single JSON object or a JSON array of objects). No markdown prose outside JSON.",
		(lang_name && *lang_name) ? lang_name : "English"
	);

	static const char *TOOLS_DOC_1 =
		"=== OBSERVATION & MEMORY TOOLS ===\n"
		"- inspect_terminal: {\"op\": \"inspect_terminal\", \"target\": {\"relation\": \"focused|other\"}}\n"
		"- inspect_browser: {\"op\": \"inspect_browser\", \"target\": {}}\n"
		"- inspect_clipboard: {\"op\": \"inspect_clipboard\", \"target\": {}}\n"
		"- inspect_system: {\"op\": \"inspect_system\", \"args\": {\"aspect\": \"all|memory|cpu|disk|ports\"}}\n"
		"- run_background_command: {\"op\": \"run_background_command\", \"target\": {\"command\": \"<read_only_shell_cmd>\"}}\n"
		"- remember_fact: {\"op\": \"remember_fact\", \"args\": {\"key\": \"<topic>\", \"value\": \"<preference_or_workflow>\"}}\n"
		"- recall_facts: {\"op\": \"recall_facts\", \"target\": {\"query\": \"<topic>\"}}\n\n"
		"=== DESKTOP & SHELL ACTION TOOLS ===\n"
		"- open_terminal: {\"op\": \"open_terminal\", \"target\": {}}\n"
		"- open_application: {\"op\": \"open_application\", \"target\": {\"identifier\": \"<app_binary>\"}}\n"
		"- spawn_shell_command: {\"op\": \"spawn_shell_command\", \"target\": {\"command\": \"<shell_cmd>\", \"new_terminal\": true}} (omit new_terminal or set false to run in the current idle terminal; set new_terminal:true whenever the user explicitly asks to open a new terminal)\n"
		"- focus_window: {\"op\": \"focus_window\", \"target\": {\"identifier\": \"<window_class_or_title>\"}}\n"
		"- focus_relative_window: {\"op\": \"focus_relative_window\", \"target\": {\"kind\": \"relative\", \"relation\": \"other|next|previous|master\"}}\n"
		"- open_url: {\"op\": \"open_url\", \"target\": {\"url\": \"https://...\"}}\n"
		"- search_web: {\"op\": \"search_web\", \"target\": {\"query\": \"<web_search_query>\"}}\n"
		"- type_text: {\"op\": \"type_text\", \"target\": {}, \"args\": {\"text\": \"<text>\"}}\n"
		"- type_and_enter: {\"op\": \"type_and_enter\", \"target\": {}, \"args\": {\"text\": \"<text>\"}}\n"
		"- press_key: {\"op\": \"press_key\", \"target\": {}, \"args\": {\"key_combo\": \"<key_combo>\"}}\n"
		"- scroll: {\"op\": \"scroll\", \"target\": {}, \"args\": {\"direction\": \"up|down|left|right\", \"amount\": 5}}\n"
		"- workspace_switch: {\"op\": \"workspace_switch\", \"target\": {}, \"args\": {\"workspace\": 1}}\n"
		"- workspace_move_window: {\"op\": \"workspace_move_window\", \"target\": {}, \"args\": {\"workspace\": 1}}\n"
		"- window_fullscreen: {\"op\": \"window_fullscreen\", \"target\": {}}\n"
		"- window_float: {\"op\": \"window_float\", \"target\": {}}\n"
		"- window_close: {\"op\": \"window_close\", \"target\": {\"kind\": \"window\"}}\n";
	static const char *TOOLS_DOC_2 =
		"- media_play: {\"op\": \"media_play\", \"target\": {\"query\": \"<song_or_artist>\"}}\n"
		"- media_pause: {\"op\": \"media_pause\", \"target\": {}}\n"
		"- media_next: {\"op\": \"media_next\", \"target\": {}}\n"
		"- media_prev: {\"op\": \"media_prev\", \"target\": {}}\n"
		"- media_volume: {\"op\": \"media_volume\", \"target\": {}, \"args\": {\"action\": \"volume_up|volume_down|volume_mute\"}}\n"
		"- record_start: {\"op\": \"record_start\", \"target\": {}, \"args\": {\"mode\": \"screen|mic|both\"}}\n"
		"- record_stop: {\"op\": \"record_stop\", \"target\": {}}\n"
		"- answer_question: {\"op\": \"answer_question\", \"target\": {}, \"args\": {\"answer\": \"<concise Jarvis reply>\", \"expect_reply\": false}}\n"
		"- conversation_control: {\"op\": \"conversation_control\", \"target\": {}, \"args\": {\"action\": \"enter|exit|reset\"}}\n"
		"- wm_help: {\"op\": \"wm_help\", \"target\": {\"query\": \"<wm_or_shortcut_question>\"}}\n\n"
		"DECISION GUIDELINES:\n"
		"1. If the user asks about an error or text on a terminal screen and `Live Terminal / Tmux Panes` does not already contain the answer, call `inspect_terminal` first; once you see the output, answer with `answer_question` or run the fix.\n"
		"2. If the user asks about the web page, article, headings, or links in the browser and `Live Browser Accessibility Page Content` is empty, call `inspect_browser` first; once you see the page content, answer with `answer_question` or open the requested link with `open_url`.\n"
		"3. If the user asks about clipboard / highlighted text, call `inspect_clipboard` first.\n"
		"4. If the user asks a quick question about the current time, date, free RAM, disk space, CPU load, or active directory/window, answer IMMEDIATELY in 1 turn with `answer_question` using the live fields below (never spawn a terminal just to run `date` or `pwd` for a spoken question).\n"
		"5. If the user asks to remember a preference or workflow, emit `remember_fact`. If they invoke a remembered workflow later, expand it into a JSON array `[step1, step2, ...]` of all actions in that workflow.\n"
		"6. Use previous conversation messages, Live Terminal Panes, Live Browser Content, and Recent action context to resolve follow-ups ('actually move it to workspace 6 instead', 'nevermind bring it back', 'open that file', 'explain more', 'yes do it'). If the user wants to end the conversation or reset the topic, emit `conversation_control`.\n"
		"7. For any local or remote shell command, SSH command (e.g. `ssh tty.pt 'df -h'`), or CLI tool (nvim, vim, git, grep, find, make), use `spawn_shell_command`. If the user asks to OPEN A TERMINAL with a command/program/config (e.g. 'open a terminal with vim', 'abre um terminal com a minha configuracao de vim'), you MUST set \"new_terminal\": true on `spawn_shell_command`!\n";

	char state_sum[4096];
	summarize_world_for_prompt(eng, world, state_sum, sizeof(state_sum));

	char user_msg0[12288];
	snprintf(
		user_msg0, sizeof(user_msg0),
		"%s%s\nCURRENT DESKTOP STATE & MEMORIES:\n%s\nCURRENT USER REQUEST: %.512s\n\nOUTPUT ONLY VALID JSON (a single DSL object or a JSON array of DSL objects):",
		TOOLS_DOC_1, TOOLS_DOC_2, state_sum, utterance
	);

	/* Up to 3 ReAct turns with full multi-turn conversation history */
	char assistant_turns[3][4096];
	char user_obs_turns[3][4096];
	memset(assistant_turns, 0, sizeof(assistant_turns));
	memset(user_obs_turns, 0, sizeof(user_obs_turns));

	out_res->step_count = 0;
	bool all_ok = true;
	char combined_ops[256] = {0};
	char combined_out[2048] = {0};
	char combined_spoken[JARVIS_TEXT_LEN] = {0};
	bool any_expect_reply = false;
	bool any_question = false;
	bool any_conv_exit = false;

	int conv_timeout_s = jarvis_rc_get_int("conv_timeout_s", 180);
	double history_ttl_s = (conv_timeout_s > 0) ? (double)conv_timeout_s : 180.0;

	for (int turn = 0; turn < 3; turn++) {
		char llm_content[8192] = {0};
		bool turn_streamed_tts = false;

#ifndef __wasm__
		if (eng->infer_mode == JARVIS_INFER_AUTO || eng->infer_mode == JARVIS_INFER_LLM) {
			const char *models[3] = {eng->llm_model, "fast", "kilo-auto/free"};
			double now_s = now_ms() / 1000.0;

			for (int mi = 0; mi < 3 && !llm_content[0]; mi++) {
				if (mi > 0 && strcmp(models[mi], models[mi - 1]) == 0)
					continue;
				char *req_body = jarvis_build_chat_messages_json(
					eng, models[mi], sys_prompt, user_msg0,
					assistant_turns, user_obs_turns, turn,
					now_s, history_ttl_s, 12
				);
				if (!req_body) continue;

				bool posted = http_post_json_sse_stream(
					eng->auren_url, "/v1/chat/completions", req_body,
					!eng->dry_run, llm_content, sizeof(llm_content),
					&turn_streamed_tts
				);
				free(req_body);
				if (!posted) {
					continue;
				}
			}
		}
#else
		(void)sys_prompt;
		(void)user_msg0;
		(void)history_ttl_s;
#endif

		if (!llm_content[0]) {
			const char *prev_obs = (turn > 0) ? user_obs_turns[turn - 1] : "";
			synthesize_offline_react_json(eng, utterance, world, turn, prev_obs, llm_content, sizeof(llm_content));
		}

		if (!llm_content[0]) break;

		char extracted[8192];
		if (!jarvis_extract_json_payload(llm_content, extracted, sizeof(extracted))) {
			break;
		}
		jjson_val_t *pv = jjson_parse(extracted);
		if (!pv) break;

		jarvis_dsl_t turn_steps[4];
		size_t n_turn_steps = 0;

		if (pv->type == JJSON_OBJECT) {
			if (jarvis_normalize_dsl_val(pv, &turn_steps[0])) {
				n_turn_steps = 1;
			}
		} else if (pv->type == JJSON_ARRAY) {
			for (size_t i = 0; i < pv->u.arr.count && n_turn_steps < 4; i++) {
				if (jarvis_normalize_dsl_val(pv->u.arr.items[i], &turn_steps[n_turn_steps])) {
					n_turn_steps++;
				}
			}
		}
		jjson_free(pv);

		if (n_turn_steps == 0) break;

		/* If the LLM emitted open_terminal immediately followed by spawn_shell_command,
		 * ensure spawn_shell_command types into that newly opened terminal instead of
		 * spawning a second terminal window. */
		for (size_t si = 0; si + 1 < n_turn_steps; si++) {
			if (strcmp(turn_steps[si].op, "open_terminal") == 0 &&
			    strcmp(turn_steps[si + 1].op, "spawn_shell_command") == 0) {
				turn_steps[si + 1].target.new_terminal = false;
			}
		}

		snprintf(assistant_turns[turn], sizeof(assistant_turns[turn]), "%.*s",
		         (int)(sizeof(assistant_turns[turn]) - 1), extracted);
		user_obs_turns[turn][0] = '\0';
		int obs_pos = 0;
		bool has_obs = false;
		bool has_action = false;

		for (size_t si = 0; si < n_turn_steps && out_res->step_count < JARVIS_MAX_STEPS; si++) {
			if (turn_streamed_tts) {
				turn_steps[si].args.streamed_tts = true;
			}
			jarvis_step_t *st = &out_res->steps[out_res->step_count++];
			memset(st, 0, sizeof(*st));
			snprintf(st->clause, sizeof(st->clause), "%s", utterance);
			snprintf(st->resolved_clause, sizeof(st->resolved_clause), "%s", utterance);
			st->has_dsl = true;
			st->dsl = turn_steps[si];
			snprintf(st->source, sizeof(st->source), "agent:auren-llm:%.32s:step%d", eng->llm_model, turn + 1);

			execute_dsl_via_xylem(eng, &st->dsl, &st->result);
			out_res->last_dsl = st->dsl;
			update_context_from_step(&eng->ctx, &st->dsl, world, st->result.output);

			if (strcmp(st->dsl.op, "conversation_control") == 0 &&
			    (strcasecmp(st->dsl.args.action, "exit") == 0 ||
			     strcasecmp(st->dsl.args.action, "reset") == 0 ||
			     strcasecmp(st->dsl.args.action, "off") == 0 ||
			     strcasecmp(st->dsl.args.action, "stop") == 0 ||
			     strcasecmp(st->dsl.args.action, "clear") == 0)) {
				any_conv_exit = true;
			}
			if (st->dsl.args.expect_reply) {
				any_expect_reply = true;
			}
			if (strcmp(st->dsl.op, "answer_question") == 0) {
				any_question = true;
				const char *ans = st->dsl.args.answer[0] ? st->dsl.args.answer : st->result.output;
				if (ans && ans[0]) {
					size_t slen = strlen(combined_spoken);
					snprintf(combined_spoken + slen, sizeof(combined_spoken) - slen, "%s%s", slen ? " " : "", ans);
					if (strchr(ans, '?') != NULL) any_expect_reply = true;
				}
			} else if (st->dsl.args.reply[0]) {
				size_t slen = strlen(combined_spoken);
				snprintf(combined_spoken + slen, sizeof(combined_spoken) - slen, "%s%s", slen ? " " : "", st->dsl.args.reply);
				if (strchr(st->dsl.args.reply, '?') != NULL) any_expect_reply = true;
			}

			if (!st->result.success) {
				all_ok = false;
				if (strcmp(st->dsl.op, "open_terminal") == 0) {
					has_action = true;
					break;
				}
			}

			size_t oplen = strlen(combined_ops);
			snprintf(combined_ops + oplen, sizeof(combined_ops) - oplen, "%s%s", oplen ? "," : "", st->dsl.op);

			const char *step_out = st->result.output[0] ? st->result.output :
			                       (st->result.error[0] ? st->result.error : st->dsl.op);

			if (is_observation_op(st->dsl.op)) {
				has_obs = true;
				obs_pos += snprintf(user_obs_turns[turn] + obs_pos,
				                    (size_t)obs_pos < sizeof(user_obs_turns[turn]) ? sizeof(user_obs_turns[turn]) - (size_t)obs_pos : 0,
				                    "OBSERVATION (%s): %s\n", st->dsl.op, step_out);
			} else {
				has_action = true;
			}
		}

		if (has_action || !has_obs) {
			break;
		}

		if (turn == 1) {
			snprintf(user_obs_turns[turn] + obs_pos,
			         (size_t)obs_pos < sizeof(user_obs_turns[turn]) ? sizeof(user_obs_turns[turn]) - (size_t)obs_pos : 0,
			         "\nFINAL STEP: Do NOT call `inspect_terminal` or `inspect_browser` or any observation tool again. "
			         "Based on the observations above, output the final action DSL or `answer_question` with a concise answer for the user now. Output ONLY valid JSON:");
		} else {
			snprintf(user_obs_turns[turn] + obs_pos,
			         (size_t)obs_pos < sizeof(user_obs_turns[turn]) ? sizeof(user_obs_turns[turn]) - (size_t)obs_pos : 0,
			         "\nBased on the observation above, output the next action DSL (or `answer_question` with a concise answer for the user). Output ONLY valid JSON:");
		}
	}

	if (out_res->step_count == 0) return false;

	/* Build combined_out from non-observation steps (fallback to all steps if only observations ran) */
	bool any_non_obs = false;
	for (size_t i = 0; i < out_res->step_count; i++) {
		if (!is_observation_op(out_res->steps[i].dsl.op)) {
			any_non_obs = true;
			break;
		}
	}
	for (size_t i = 0; i < out_res->step_count; i++) {
		const jarvis_step_t *st = &out_res->steps[i];
		if (any_non_obs && is_observation_op(st->dsl.op)) continue;
		const char *step_out = st->result.output[0] ? st->result.output :
		                       (st->result.error[0] ? st->result.error : st->dsl.op);
		size_t outlen = strlen(combined_out);
		snprintf(combined_out + outlen, sizeof(combined_out) - outlen, "%s%s", outlen ? " -> " : "", step_out);
	}

	out_res->ok = true;
	out_res->combined_result.handled = true;
	out_res->combined_result.success = all_ok;
	snprintf(out_res->combined_result.output, sizeof(out_res->combined_result.output), "%s", combined_out);
	if (!all_ok) {
		snprintf(out_res->combined_result.error, sizeof(out_res->combined_result.error), "one or more agent steps failed");
	}
	snprintf(out_res->source, sizeof(out_res->source), "agent:auren-llm:%s", eng->llm_model);
	if (!combined_spoken[0]) {
		jarvis_format_voice_reply(eng->ont, out_res, combined_spoken, sizeof(combined_spoken));
	}
	if (!any_conv_exit && (any_question || any_expect_reply)) {
		eng->ctx.conv_active = true;
		eng->ctx.conv_last_ts = now_ms() / 1000.0;
	}
	record_session_turn(eng, utterance, combined_ops, combined_out, combined_spoken, any_expect_reply);
	return true;
}

bool jarvis_process(
	jarvis_engine_t         *eng,
	const char              *utterance,
	const char              *active_surface,
	jarvis_process_result_t *out_result
)
{
	if (!out_result) return false;
	memset(out_result, 0, sizeof(*out_result));
	if (!eng || !utterance) {
		snprintf(out_result->error, sizeof(out_result->error), "Invalid engine or utterance");
		return false;
	}
	snprintf(out_result->utterance, sizeof(out_result->utterance), "%s", utterance);

	if (jarvis_is_non_speech_hallucination(utterance)) {
		snprintf(out_result->error, sizeof(out_result->error), "Filtered non-speech ASR hallucination");
		return false;
	}

	double now_s = now_ms() / 1000.0;
	int conv_timeout_s = jarvis_rc_get_int("conv_timeout_s", 180);
	if (conv_timeout_s > 0 && eng->ctx.conv_active && eng->ctx.conv_last_ts > 0.0 &&
	    (now_s - eng->ctx.conv_last_ts) > (double)conv_timeout_s) {
		eng->ctx.conv_active = false;
		eng->ctx.conv_last_ts = 0.0;
		if (!eng->dry_run) save_context_to_disk(eng);
	}

	const char *surf = (active_surface && *active_surface) ? active_surface : "any";

	/* 1. Observe live world from loaded Xylem modules */
	double t_obs0 = now_ms();
	jarvis_world_t world;
	memset(&world, 0, sizeof(world));
	world.context = eng->ctx;
	jarvis_xy_observe(&world);
	double observe_ms = now_ms() - t_obs0;

	if (strcmp(surf, "any") == 0 && world.focused.wm_class[0]) {
		char cls_low[128];
		jarvis_clean_phrase(world.focused.wm_class, cls_low, sizeof(cls_low));
		if (strstr(cls_low, "st") || strstr(cls_low, "xterm") || strstr(cls_low, "alacritty") ||
		    strstr(cls_low, "kitty") || strstr(cls_low, "foot") || strstr(cls_low, "wezterm") ||
		    strstr(cls_low, "urxvt") || strstr(cls_low, "ghostty")) {
			surf = "terminal";
		} else if (strstr(cls_low, "firefox") || strstr(cls_low, "chromium") || strstr(cls_low, "chrome") ||
		           strstr(cls_low, "brave") || strstr(cls_low, "qutebrowser")) {
			surf = "browser";
		}
	}

	if (!eng->dry_run) {
		jarvis_xy_voice_status(JARVIS_VOICE_ACTING, utterance);
	}

	/* If the previous turn within 25s ended in a clarifying question (answer_question
	 * with '?' in output/spoken, e.g. "Open what? Please specify...") and the user's new utterance
	 * is a short clarification that stitches with the previous incomplete utterance into a
	 * high-confidence Tier-0 action (e.g. "open a new" + "terminal", or "move this window to" + "5"),
	 * stitch them automatically. */
	char stitched_utterance[512];
	if (eng->turn_count > 0) {
		const jarvis_session_turn_t *last_t = &eng->turns[eng->turn_count - 1];
		if (last_t->utterance[0] != '\0' &&
		    (now_s - last_t->ts) <= 25.0 &&
		    strcmp(last_t->ops, "answer_question") == 0 &&
		    (strchr(last_t->output, '?') != NULL || strchr(last_t->spoken, '?') != NULL) &&
		    strlen(utterance) <= 48) {
			jarvis_ir_t ir_alone, ir_stitched;
			bool alone_ok = (!requires_agent_reasoning(eng, utterance)) &&
			                jarvis_solve_clause(eng->ont, utterance, surf, &world, &ir_alone) &&
			                ir_alone.score >= 0.90;
			snprintf(stitched_utterance, sizeof(stitched_utterance), "%s %s", last_t->utterance, utterance);
			if (!alone_ok &&
			    !requires_agent_reasoning(eng, stitched_utterance) &&
			    jarvis_solve_clause(eng->ont, stitched_utterance, surf, &world, &ir_stitched) &&
			    ir_stitched.score >= 0.90) {
				utterance = stitched_utterance;
				snprintf(out_result->utterance, sizeof(out_result->utterance), "%s", utterance);
			}
		}
	}

	/* 2. Check if whole utterance requires multi-turn ReAct agent reasoning */
	if (requires_agent_reasoning(eng, utterance)) {
		if (jarvis_auren_run_react_agent(eng, utterance, &world, out_result)) {
			if (!eng->dry_run) {
				jarvis_xy_voice_status(JARVIS_VOICE_STANDBY, "");
			}
			return true;
		}
	}

	/* 3. Split into atomic clauses for deterministic Tier-0 or hybrid multi-clause chaining */
	char clauses[JARVIS_MAX_STEPS][512];
	size_t n_clauses = jarvis_split_clauses(eng->ont, utterance, clauses, JARVIS_MAX_STEPS);
	if (n_clauses == 0) {
		if (!eng->dry_run) {
			jarvis_xy_voice_status(JARVIS_VOICE_STANDBY, "");
		}
		snprintf(out_result->error, sizeof(out_result->error), "Empty utterance");
		return false;
	}

	double tier0_thresh = (eng->infer_mode != JARVIS_INFER_EMBEDDING) ? 0.90 : 0.85;

	/* If a 2-clause utterance was split on a casual comma ("this text is tiny, I can't read it")
	 * and not an explicit multi-word comma-conjunction separator from ont->clause_seps,
	 * and the whole utterance deterministically resolves as a single high-confidence Tier-0 action
	 * (or both clauses resolve to the same IR capability), treat it as a single 1-step utterance. */
	bool has_explicit_comma_sep = false;
	if (eng->ont) {
		for (size_t si = 0; si < eng->ont->clause_sep_count; si++) {
			const char *sep = eng->ont->clause_seps[si];
			if (sep[0] == ',' && strlen(sep) > 2 && strstr(utterance, sep)) {
				has_explicit_comma_sep = true;
				break;
			}
		}
	}
	if (n_clauses == 2 && strstr(utterance, ", ") && !has_explicit_comma_sep) {
		jarvis_ir_t ir_full, ir0, ir1;
		if (!requires_agent_reasoning(eng, utterance) &&
		    jarvis_solve_clause(eng->ont, utterance, surf, &world, &ir_full) &&
		    ir_full.score >= tier0_thresh &&
		    (!jarvis_solve_clause(eng->ont, clauses[1], surf, &world, &ir1) ||
		     ir1.score < tier0_thresh ||
		     strcmp(ir_full.cap, ir1.cap) == 0)) {
			snprintf(clauses[0], sizeof(clauses[0]), "%s", utterance);
			n_clauses = 1;
		} else if (jarvis_solve_clause(eng->ont, clauses[0], surf, &world, &ir0) &&
		           jarvis_solve_clause(eng->ont, clauses[1], surf, &world, &ir1) &&
		           ir0.score >= tier0_thresh && ir1.score >= tier0_thresh &&
		           strcmp(ir0.cap, ir1.cap) == 0 &&
		           strcmp(ir0.action, ir1.action) == 0 &&
		           strcmp(ir0.text, ir1.text) == 0) {
			snprintf(clauses[0], sizeof(clauses[0]), "%s", utterance);
			n_clauses = 1;
		}
	}

	/* If multi-clause and any clause cannot be deterministically solved at high confidence,
	 * route the entire utterance to the ReAct agent so cross-clause context is preserved. */
	if (n_clauses > 1 && eng->infer_mode != JARVIS_INFER_EMBEDDING) {
		bool all_det = true;
		for (size_t ci = 0; ci < n_clauses; ci++) {
			char res_c[512];
			jarvis_resolve_anaphora(clauses[ci], &eng->ctx, res_c, sizeof(res_c));
			jarvis_ir_t tmp_ir;
			if (requires_agent_reasoning(eng, res_c) ||
			    !jarvis_solve_clause(eng->ont, res_c, surf, &world, &tmp_ir) ||
			    tmp_ir.score < tier0_thresh) {
				all_det = false;
				break;
			}
		}
		if (!all_det) {
			if (jarvis_auren_run_react_agent(eng, utterance, &world, out_result)) {
				if (!eng->dry_run) {
					jarvis_xy_voice_status(JARVIS_VOICE_STANDBY, "");
				}
				return true;
			}
		}
	}

	char curr_surf[32];
	snprintf(curr_surf, sizeof(curr_surf), "%s", surf);
	bool all_ok = true;
	char combined_out[2048] = {0};
	char combined_ops[256] = {0};
	char combined_spoken[JARVIS_TEXT_LEN] = {0};
	bool any_expect_reply = false;
	bool any_question = false;
	bool any_conv_exit = false;

	for (size_t ci = 0; ci < n_clauses && out_result->step_count < JARVIS_MAX_STEPS; ci++) {
		double t_step0 = now_ms();
		jarvis_step_t *st = &out_result->steps[out_result->step_count];
		memset(st, 0, sizeof(*st));
		snprintf(st->clause, sizeof(st->clause), "%.*s", (int)(sizeof(st->clause) - 1), clauses[ci]);
		jarvis_resolve_anaphora(clauses[ci], &eng->ctx, st->resolved_clause, sizeof(st->resolved_clause));
		st->observe_ms = observe_ms;

		bool force_agent = requires_agent_reasoning(eng, st->resolved_clause);

		double t_sol0 = now_ms();
		bool solved_ir = (!force_agent) && jarvis_solve_clause(eng->ont, st->resolved_clause, curr_surf, &world, &st->ir);
		st->solver_ms = now_ms() - t_sol0;

		if (solved_ir && st->ir.score >= tier0_thresh &&
		    jarvis_compile_ir_to_dsl(&st->ir, curr_surf, &eng->ctx, &world, &st->dsl)) {
			st->has_ir = true;
			st->has_dsl = true;
			snprintf(st->source, sizeof(st->source), "solver");
		} else if (eng->infer_mode != JARVIS_INFER_EMBEDDING) {
			/* Single-clause escalation to ReAct agent (already records session turn) */
			if (n_clauses == 1 && jarvis_auren_run_react_agent(eng, st->resolved_clause, &world, out_result)) {
				if (!eng->dry_run) {
					jarvis_xy_voice_status(JARVIS_VOICE_STANDBY, "");
				}
				return true;
			}
		}

		if (!st->has_dsl && solved_ir &&
		    jarvis_compile_ir_to_dsl(&st->ir, curr_surf, &eng->ctx, &world, &st->dsl)) {
			st->has_ir = true;
			st->has_dsl = true;
			snprintf(st->source, sizeof(st->source), "solver:low_confidence");
		}

		if (!st->has_dsl) {
			if (!eng->dry_run) {
				jarvis_xy_voice_status(JARVIS_VOICE_STANDBY, "");
			}
			snprintf(out_result->error, sizeof(out_result->error), "Could not resolve clause: %s", st->clause);
			return false;
		}

		double t_ex0 = now_ms();
		execute_dsl_via_xylem(eng, &st->dsl, &st->result);
		st->execute_ms = now_ms() - t_ex0;
		st->total_ms = now_ms() - t_step0;

		update_context_from_step(&eng->ctx, &st->dsl, &world, st->result.output);
		out_result->last_dsl = st->dsl;
		out_result->step_count++;

		if (strcmp(st->dsl.op, "conversation_control") == 0 &&
		    (strcasecmp(st->dsl.args.action, "exit") == 0 ||
		     strcasecmp(st->dsl.args.action, "reset") == 0 ||
		     strcasecmp(st->dsl.args.action, "off") == 0 ||
		     strcasecmp(st->dsl.args.action, "stop") == 0 ||
		     strcasecmp(st->dsl.args.action, "clear") == 0)) {
			any_conv_exit = true;
		}
		if (st->dsl.args.expect_reply) {
			any_expect_reply = true;
		}
		if (strcmp(st->dsl.op, "answer_question") == 0) {
			any_question = true;
			const char *ans = st->dsl.args.answer[0] ? st->dsl.args.answer : st->result.output;
			if (ans && ans[0]) {
				size_t slen = strlen(combined_spoken);
				snprintf(combined_spoken + slen, sizeof(combined_spoken) - slen, "%s%s", slen ? " " : "", ans);
				if (strchr(ans, '?') != NULL) any_expect_reply = true;
			}
		} else if (st->dsl.args.reply[0]) {
			size_t slen = strlen(combined_spoken);
			snprintf(combined_spoken + slen, sizeof(combined_spoken) - slen, "%s%s", slen ? " " : "", st->dsl.args.reply);
			if (strchr(st->dsl.args.reply, '?') != NULL) any_expect_reply = true;
		}

		if (!st->result.success) all_ok = false;

		size_t oplen = strlen(combined_ops);
		snprintf(combined_ops + oplen, sizeof(combined_ops) - oplen, "%s%s", oplen ? "," : "", st->dsl.op);

		const char *step_out = st->result.output[0] ? st->result.output :
		                       (st->result.error[0] ? st->result.error : st->dsl.op);
		size_t outlen = strlen(combined_out);
		snprintf(combined_out + outlen, sizeof(combined_out) - outlen, "%s%s", outlen ? " -> " : "", step_out);

		if (!out_result->source[0]) {
			snprintf(out_result->source, sizeof(out_result->source), "%s", st->source);
		}

		if (strcmp(st->dsl.op, "open_terminal") == 0 || strcmp(st->dsl.op, "spawn_shell_command") == 0) {
			snprintf(curr_surf, sizeof(curr_surf), "terminal");
		}
	}

	if (!eng->dry_run) {
		jarvis_xy_voice_status(JARVIS_VOICE_STANDBY, "");
	}

	out_result->ok = (out_result->step_count > 0);
	out_result->combined_result.handled = true;
	out_result->combined_result.success = all_ok;
	snprintf(out_result->combined_result.output, sizeof(out_result->combined_result.output), "%s", combined_out);
	if (!all_ok) {
		snprintf(out_result->combined_result.error, sizeof(out_result->combined_result.error), "one or more steps failed");
	}
	if (!combined_spoken[0]) {
		jarvis_format_voice_reply(eng->ont, out_result, combined_spoken, sizeof(combined_spoken));
	}
	if (!any_conv_exit && (any_question || any_expect_reply)) {
		eng->ctx.conv_active = true;
		eng->ctx.conv_last_ts = now_ms() / 1000.0;
	}
	record_session_turn(eng, utterance, combined_ops, combined_out, combined_spoken, any_expect_reply);
	return out_result->ok;
}
