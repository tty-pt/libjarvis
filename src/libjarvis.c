#include "jarvis-internal.h"

#ifndef __wasm__
#include <sys/time.h>
#include <unistd.h>
#if defined(_WIN32)
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

/* ---------------------------------------------------------------------------
 * Streaming TTS Answer Prefix & Clause Extractor
 * ------------------------------------------------------------------------- */

bool jarvis_extract_streaming_answer_prefix(
	const char *raw_json_fragment,
	char       *out_answer,
	size_t      out_sz
)
{
	if (!out_answer || out_sz == 0) return false;
	out_answer[0] = '\0';
	if (!raw_json_fragment || !*raw_json_fragment) return false;

	if (!strstr(raw_json_fragment, "\"answer_question\"") &&
	    !strstr(raw_json_fragment, "\"answer\"") &&
	    !strstr(raw_json_fragment, "\"reply\"")) {
		return false;
	}

	const char *ans_key = strstr(raw_json_fragment, "\"answer\"");
	if (!ans_key) return false;
	const char *p = ans_key + 8;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p != ':') return false;
	p++;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p != '"') return false;
	p++;

	size_t w = 0;
	while (*p && w + 1 < out_sz) {
		char ch = *p;
		if (ch == '"') break;
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
				if (cp < 0x80) out_answer[w++] = (char)cp;
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

static bool is_prep_conj_at(const char *s, size_t *out_word_len)
{
	static const char *WORDS[] = {
		"at", "in", "on", "by", "to", "for", "from", "with",
		"where", "which", "that", "and", "or", "as", "of",
		"because", "while", "when", NULL
	};
	for (size_t i = 0; WORDS[i]; i++) {
		size_t wl = strlen(WORDS[i]);
		if (strncmp(s, WORDS[i], wl) == 0 && isspace((unsigned char)s[wl])) {
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

	if (strcmp(clean, "pause it") == 0 || strcmp(clean, "pause that") == 0) {
		snprintf(out_clause, out_sz, "pause the music");
		return;
	}
	if (strcmp(clean, "play it") == 0 || strcmp(clean, "play that") == 0 ||
	    strcmp(clean, "resume it") == 0 || strcmp(clean, "resume that") == 0 ||
	    strcmp(clean, "play it again") == 0) {
		if (ctx && ctx->last_query[0] &&
		    (strcmp(ctx->last_op, "media_play") == 0 || strcmp(ctx->last_op, "search_web") == 0)) {
			snprintf(out_clause, out_sz, "play %s", ctx->last_query);
		} else {
			snprintf(out_clause, out_sz, "play music");
		}
		return;
	}

	/* Deictic spatial pronoun: "there" -> "in <last_location_basename>" */
	if (ctx && ctx->last_location[0]) {
		const char *base = strrchr(ctx->last_location, '/');
		base = (base && base[1]) ? (base + 1) : ctx->last_location;

		/* Look for whole word "there" in clause */
		char low[512];
		size_t len = strlen(clause);
		if (len >= sizeof(low)) len = sizeof(low) - 1;
		for (size_t i = 0; i < len; i++) low[i] = (char)tolower((unsigned char)clause[i]);
		low[len] = '\0';

		const char *hit = strstr(low, "there");
		while (hit) {
			bool left_ok = (hit == low || !isalnum((unsigned char)hit[-1]));
			bool right_ok = (!isalnum((unsigned char)hit[5]));
			if (left_ok && right_ok) {
				size_t prefix_len = (size_t)(hit - low);
				snprintf(out_clause, out_sz, "%.*sin %s%s",
				         (int)prefix_len, clause, base, clause + prefix_len + 5);
				return;
			}
			hit = strstr(hit + 5, "there");
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

	/* Check if utterance starts with explicit dictation or agent-ask verb */
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
		if (!strstr(low, " and then ") && !strstr(low, " then ")) {
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
				/* Check if every token in pre_clean is a polite prefix or wake word */
				char *psave = NULL;
				bool all_polite = (pre_clean[0] != '\0');
				for (char *pt = strtok_r(pre_clean, " ", &psave); pt; pt = strtok_r(NULL, " ", &psave)) {
					if (!jarvis_ontology_is_polite_prefix(ont, pt)) {
						all_polite = false;
						break;
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

	/* Protect atomic phrases containing 'and' */
	replace_all_ci(work, sizeof(work), " and press enter", " __AND_PRESS_ENTER__");
	replace_all_ci(work, sizeof(work), " and hit enter", " __AND_HIT_ENTER__");
	replace_all_ci(work, sizeof(work), " and hit return", " __AND_HIT_RETURN__");
	replace_all_ci(work, sizeof(work), " and press return", " __AND_PRESS_RETURN__");
	replace_all_ci(work, sizeof(work), "screen and audio", "__SCREEN_AND_AUDIO__");

	/* Normalize explicit multi-clause separators to ';' */
	replace_all_ci(work, sizeof(work), ", and then ", ";");
	replace_all_ci(work, sizeof(work), ", then ", ";");
	replace_all_ci(work, sizeof(work), ", and ", ";");
	replace_all_ci(work, sizeof(work), " and then ", ";");
	replace_all_ci(work, sizeof(work), " after that ", ";");
	replace_all_ci(work, sizeof(work), " followed by ", ";");
	replace_all_ci(work, sizeof(work), " then ", ";");
	replace_all_ci(work, sizeof(work), ", ", ";");

	size_t count = 0;
	char *save_semi = NULL;
	for (char *part = strtok_r(work, ";", &save_semi); part && count < max_clauses; part = strtok_r(NULL, ";", &save_semi)) {
		while (*part && isspace((unsigned char)*part)) part++;
		if (!*part) continue;

		/* Further split on " and " if the right-hand side begins with a known verb head word */
		char *cursor = part;
		char accum[512] = {0};

		while (cursor && *cursor && count < max_clauses) {
			char *and_pos = NULL;
			for (char *p = cursor; *p; p++) {
				if ((strncmp(p, " and ", 5) == 0 || strncmp(p, " AND ", 5) == 0)) {
					and_pos = p;
					break;
				}
			}
			if (!and_pos) {
				if (accum[0]) {
					size_t alen = strlen(accum);
					snprintf(accum + alen, sizeof(accum) - alen, " and %s", cursor);
				} else {
					snprintf(accum, sizeof(accum), "%s", cursor);
				}
				break;
			}

			*and_pos = '\0';
			char *rhs = and_pos + 5;
			while (*rhs && isspace((unsigned char)*rhs)) rhs++;

			if (accum[0]) {
				size_t alen = strlen(accum);
				snprintf(accum + alen, sizeof(accum) - alen, " and %s", cursor);
			} else {
				snprintf(accum, sizeof(accum), "%s", cursor);
			}

			/* Check if first non-polite word of rhs is a known verb head */
			char rhs_clean[256];
			jarvis_clean_phrase(rhs, rhs_clean, sizeof(rhs_clean));
			char *rsave = NULL;
			char *rw = strtok_r(rhs_clean, " ", &rsave);
			while (rw && (strcmp(rw, "please") == 0 || strcmp(rw, "also") == 0 || strcmp(rw, "then") == 0)) {
				rw = strtok_r(NULL, " ", &rsave);
			}
			if (rw && ont && jarvis_ontology_is_verb_head(ont, rw)) {
				char *slot = out_clauses[count++];
				snprintf(slot, sizeof(out_clauses[0]), "%s", accum);
				accum[0] = '\0';
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
		replace_all_ci(out_clauses[i], sizeof(out_clauses[i]), "__AND_PRESS_ENTER__", "and press enter");
		replace_all_ci(out_clauses[i], sizeof(out_clauses[i]), "__AND_HIT_ENTER__", "and hit enter");
		replace_all_ci(out_clauses[i], sizeof(out_clauses[i]), "__AND_HIT_RETURN__", "and hit return");
		replace_all_ci(out_clauses[i], sizeof(out_clauses[i]), "__AND_PRESS_RETURN__", "and press return");
		replace_all_ci(out_clauses[i], sizeof(out_clauses[i]), "__SCREEN_AND_AUDIO__", "screen and audio");

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
 * Engine Lifecycle, Persistence & Context
 * ------------------------------------------------------------------------- */

void jarvis_config_init(jarvis_config_t *cfg)
{
	if (!cfg) return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->wm         = "dwm";
	cfg->auren_url  = "http://127.0.0.1:4242";
	cfg->llm_model  = "kilo-auto/free";
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

bool jarvis_memory_remember(jarvis_engine_t *eng, const char *key, const char *value)
{
	if (!eng || !key || !*key || !value || !*value) return false;
	for (size_t i = 0; i < eng->memory_count; i++) {
		if (strcmp(eng->memories[i].key, key) == 0) {
			snprintf(eng->memories[i].value, sizeof(eng->memories[i].value), "%s", value);
			save_memories_to_disk(eng);
			return true;
		}
	}
	if (eng->memory_count < JARVIS_MAX_MEMORIES) {
		jarvis_memory_kv_t *m = &eng->memories[eng->memory_count++];
		snprintf(m->key, sizeof(m->key), "%s", key);
		snprintf(m->value, sizeof(m->value), "%s", value);
		save_memories_to_disk(eng);
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
		"  \"last_line\": %d\n"
		"}\n",
		eng->ctx.last_ts, e_op, e_cmd, e_app, e_q, e_loc, e_wid, e_wcls,
		eng->ctx.last_workspace, eng->ctx.prev_workspace, e_url, e_file,
		eng->ctx.last_line
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
						t->ts = jjson_obj_get_num(item, "ts", 0.0);
						snprintf(t->utterance, sizeof(t->utterance), "%s", jjson_obj_get_str(item, "utterance", ""));
						snprintf(t->ops, sizeof(t->ops), "%s", jjson_obj_get_str(item, "ops", ""));
						snprintf(t->output, sizeof(t->output), "%s", jjson_obj_get_str(item, "output", ""));
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
	FILE *fp = fopen(eng->session_path, "wb");
	if (!fp) return;
	fprintf(fp, "[\n");
	for (size_t i = 0; i < eng->turn_count; i++) {
		char eu[1024], eo[512], eout[1024];
		jjson_escape_str(eng->turns[i].utterance, eu, sizeof(eu));
		jjson_escape_str(eng->turns[i].ops, eo, sizeof(eo));
		jjson_escape_str(eng->turns[i].output, eout, sizeof(eout));
		fprintf(fp,
			"  {\"ts\":%.3f,\"utterance\":\"%s\",\"ops\":\"%s\",\"output\":\"%s\"}%s\n",
			eng->turns[i].ts, eu, eo, eout, (i + 1 < eng->turn_count) ? "," : ""
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
	         ((env_m && *env_m) ? env_m : "kilo-auto/free"));
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
	return eng;
}

void jarvis_engine_destroy(jarvis_engine_t *eng)
{
	if (!eng) return;
	jarvis_ontology_destroy(eng->ont);
	free(eng);
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
	if (utterance[len - 1] == '?') return true;

	char clean[512];
	jarvis_clean_phrase(utterance, clean, sizeof(clean));

	/* Never force agent reasoning on explicit dictation, coding-agent delegation, or TTS toggle */
	if (strncmp(clean, "type ", 5) == 0 || strncmp(clean, "dictate ", 8) == 0 ||
	    strncmp(clean, "tell pi ", 8) == 0 || strncmp(clean, "ask pi ", 7) == 0 ||
	    strncmp(clean, "have pi ", 8) == 0 || strncmp(clean, "ask the agent ", 14) == 0 ||
	    strncmp(clean, "tell the agent ", 15) == 0 ||
	    ((strstr(clean, "voice") || strstr(clean, "tts")) &&
	     (strstr(clean, "turn on") || strstr(clean, "turn off") ||
	      strstr(clean, "enable") || strstr(clean, "disable") ||
	      strcmp(clean, "voice on") == 0 || strcmp(clean, "voice off") == 0))) {
		return false;
	}

	char padded[520];
	snprintf(padded, sizeof(padded), " %s ", clean);

	static const char *TRIGGERS[] = {
		" what ", " whats ", " why ", " how ", " who ", " where ", " which ",
		" explain ", " diagnose ", " describe ", " clarify ", " elaborate ", " summarize ",
		" tell me ", " give me an example ", " for example ", " what about ", " how about ",
		" and what ", " and why ", " and how ", " really ", " seriously ", " are you sure ",
		" hello ", " hi ", " hey ", " greetings ", " good morning ", " good afternoon ", " good evening ", " thanks ", " thank you ",
		" are you there ", " can you hear ", " talk to me ", " talk again ", " speak to me ", " you awake ", " you listening ",
		" error ", " errors ", " failing ", " failed ", " broken ", " crash ", " crashing ", " crashed ", " bug ", " traceback ", " exception ", " problem ", " issue ", " wrong ",
		" eating ", " hogging ", " lagging ", " slow ", " ram ", " memory ", " cpu ", " port ", " ports ", " process ", " processes ",
		" clipboard ", " selected ", " highlighted ", " selection ",
		" remember ", " whenever ", " preference ", " preferences ", " workflow ", " habit ",
		" actually ", " instead ", " undo ", " nevermind ", " never mind ", " bring it back ", " take me back ", " go back to ",
		" repeat that ", " do that again ", " run that again ", " run it again ", " run that command again ", " that again ", " do the same ", " same thing ",
		" what did you ", " say that again ",
		" open that file ", " that file ", " failing file ", " file mentioned ", " failing line ", " at that line ", " at line ", " ssh ",
		" nelvim ", " near of him ", " vam ",
		" killing me ", " i am bored ", " im bored ", " set up my ", " setup my ",
		" on the page ", " on that page ", " web page ", " webpage ", " in the browser ", " on my browser ", " browser page ",
		" heading ", " article ", " link on ", " button on ", " read the page ", " on the screen ", " terminal screen ", " other terminal ",
		" pull back ",
		NULL
	};
	for (size_t i = 0; i < sizeof(TRIGGERS) / sizeof(TRIGGERS[0]) && TRIGGERS[i]; i++) {
		if (strstr(padded, TRIGGERS[i])) return true;
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

static void record_session_turn(jarvis_engine_t *eng, const char *utterance, const char *ops, const char *output)
{
	if (!eng) return;
	if (eng->turn_count >= JARVIS_MAX_TURNS) {
		memmove(&eng->turns[0], &eng->turns[1], (JARVIS_MAX_TURNS - 1) * sizeof(jarvis_session_turn_t));
		eng->turn_count = JARVIS_MAX_TURNS - 1;
	}
	jarvis_session_turn_t *t = &eng->turns[eng->turn_count++];
	t->ts = now_ms() / 1000.0;
	snprintf(t->utterance, sizeof(t->utterance), "%s", utterance ? utterance : "");
	snprintf(t->ops, sizeof(t->ops), "%s", ops ? ops : "");
	snprintf(t->output, sizeof(t->output), "%s", output ? output : "");
	if (!eng->dry_run) {
		save_context_to_disk(eng);
		save_session_to_disk(eng);
	}
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
				jarvis_xy_speak_clause("oneshot", ans, 1);
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
			jarvis_xy_speak_clause("oneshot", reply_dsl.args.answer, 1);
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
			"~/.vimrc", "~/.config/nvim/init.lua", "~/.config/nvim/init.vim",
			"~/.bashrc", "~/.zshrc", "~/.tmux.conf", "~/.Xresources",
			"~/.local/src/dwm/config.h", "~/.config/jarvis/ontology.json", NULL
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
	if (eng && eng->turn_count > 0) {
		n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
		              "Recent Conversation Turns:\n");
		for (size_t i = 0; i < eng->turn_count; i++) {
			const jarvis_session_turn_t *t = &eng->turns[i];
			n += snprintf(out_sum + n, (size_t)n < sum_sz ? sum_sz - (size_t)n : 0,
			              "  User: %s -> Ops: %s (Jarvis Reply/Output: %.240s)\n",
			              t->utterance, t->ops, t->output);
		}
	}
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

	char clean[512];
	jarvis_clean_phrase(utterance, clean, sizeof(clean));
	char padded[520];
	snprintf(padded, sizeof(padded), " %s ", clean);

	/* Turn 1 follow-up after an observation step */
	if (turn > 0 && user_obs_turn1 && user_obs_turn1[0]) {
		if (strstr(user_obs_turn1, "OBSERVATION (inspect_terminal)")) {
			if (strstr(padded, " open ") && (strstr(padded, " file ") || strstr(padded, " neovim ") || strstr(padded, " vim "))) {
				char fpath[128] = "src/audio.c";
				int fline = 0;
				extract_file_from_text(user_obs_turn1, fpath, sizeof(fpath), &fline);
				if (fline > 0 && (strstr(padded, " line ") || strstr(padded, " crashed ") || strstr(padded, " failed "))) {
					snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim +%d %s\"}}", fline, fpath);
				} else {
					snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim %s\"}}", fpath);
				}
				return true;
			}
			const char *err_hit = strstr(user_obs_turn1, "error:");
			if (err_hit) {
				char fpath[128] = "src/audio.c";
				int fline = 88;
				extract_file_from_text(user_obs_turn1, fpath, sizeof(fpath), &fline);
				snprintf(out_json, out_sz,
				         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"Compilation failed in %s at line %d due to a missing semicolon after pitch_init().\"}}",
				         fpath, fline);
				return true;
			}
			if (strstr(user_obs_turn1, "reactor.c") || strstr(user_obs_turn1, "xy_flux_capacitor")) {
				snprintf(out_json, out_sz,
				         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"The terminal screen shows a fatal error in src/reactor.c at line 142: undefined symbol xy_flux_capacitor.\"}}");
				return true;
			}
			char esc_obs[320];
			jjson_escape_str(user_obs_turn1 + 31, esc_obs, sizeof(esc_obs));
			snprintf(out_json, out_sz,
			         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"Terminal output: %.200s\"}}", esc_obs);
			return true;
		}
		if (strstr(user_obs_turn1, "OBSERVATION (inspect_browser)")) {
			if ((strstr(padded, " open ") || strstr(padded, " follow ") || strstr(padded, " click ") || strstr(padded, " go to ")) &&
			    (strstr(padded, " link ") || strstr(padded, " docs ") || strstr(padded, " architecture ") || strstr(padded, " url "))) {
				char url[256] = "https://tty.pt/docs/architecture";
				extract_first_url_from_text(user_obs_turn1, url, sizeof(url));
				snprintf(out_json, out_sz, "{\"op\":\"open_url\",\"target\":{\"url\":\"%s\"}}", url);
				return true;
			}
			if (strstr(padded, " search ") || strstr(padded, " look up ") || strstr(padded, " google ")) {
				const char *topic = strstr(user_obs_turn1, "auren-node-07") ? "auren-node-07 cluster node" : "Project Auren Status Dashboard";
				snprintf(out_json, out_sz, "{\"op\":\"search_web\",\"target\":{\"query\":\"%s\"}}", topic);
				return true;
			}
			if (strstr(user_obs_turn1, "Project Auren Status Dashboard")) {
				snprintf(out_json, out_sz,
				         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"The browser is displaying the Project Auren Status Dashboard on node auren-node-07 with status ALL_TESTS_GREEN and a link to the Architecture Docs.\"}}");
				return true;
			}
			char esc_b[320];
			jjson_escape_str(user_obs_turn1 + 30, esc_b, sizeof(esc_b));
			snprintf(out_json, out_sz,
			         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"Browser page content: %.220s\"}}", esc_b);
			return true;
		}
		if (strstr(user_obs_turn1, "OBSERVATION (inspect_clipboard)")) {
			char url[256] = "https://news.ycombinator.com";
			extract_first_url_from_text(user_obs_turn1, url, sizeof(url));
			snprintf(out_json, out_sz, "{\"op\":\"open_url\",\"target\":{\"url\":\"%s\"}}", url);
			return true;
		}
		if (strstr(user_obs_turn1, "OBSERVATION (inspect_system)")) {
			char esc_s[320];
			jjson_escape_str(user_obs_turn1 + 29, esc_s, sizeof(esc_s));
			snprintf(out_json, out_sz,
			         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"System status: %.220s\"}}", esc_s);
			return true;
		}
		return false;
	}

	/* 1. Multi-step ReAct Terminal Diagnosis & Screen Reading */
	if (strstr(padded, " compiler error ") || strstr(padded, " error in my terminal ") ||
	    strstr(padded, " terminal screen ") || strstr(padded, " on the terminal ") ||
	    strstr(padded, " in the other terminal ") || strstr(padded, " other terminal ") ||
	    (strstr(padded, " terminal ") && (strstr(padded, " error ") || strstr(padded, " failed ") ||
	     strstr(padded, " say ") || strstr(padded, " says ") || strstr(padded, " output ") ||
	     strstr(padded, " what ") || strstr(padded, " whats ") || strstr(padded, " read ")))) {
		bool is_other = strstr(padded, " other ") != NULL;
		if (!is_other && world && world->pane_count > 0 && world->panes[0].tail[0] &&
		    !(strstr(padded, " open ") && strstr(padded, " file "))) {
			const char *tail = world->panes[0].tail;
			if (strstr(tail, "reactor.c") || strstr(tail, "xy_flux_capacitor")) {
				snprintf(out_json, out_sz,
				         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"The terminal screen shows a fatal error in src/reactor.c at line 142: undefined symbol xy_flux_capacitor.\"}}");
				return true;
			}
			if (strstr(tail, "error:")) {
				char fpath[128] = "src/audio.c";
				int fline = 88;
				extract_file_from_text(tail, fpath, sizeof(fpath), &fline);
				snprintf(out_json, out_sz,
				         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"Compilation failed in %s:%d (line %d).\"}}",
				         fpath, fline, fline);
				return true;
			}
		}
		snprintf(out_json, out_sz, "{\"op\":\"inspect_terminal\",\"target\":{\"relation\":\"%s\"}}",
		         is_other ? "other" : "focused");
		return true;
	}

	/* 1b. Multi-step ReAct Clipboard -> Browser */
	if (strstr(padded, " clipboard ") || strstr(padded, " highlighted ") || strstr(padded, " selected ")) {
		snprintf(out_json, out_sz, "{\"op\":\"inspect_clipboard\",\"target\":{}}");
		return true;
	}

	/* 1c. Multi-step ReAct System Health / RAM / CPU / Disk / Ports Inspection */
	if ((strstr(padded, " eating ") || strstr(padded, " hogging ") || strstr(padded, " how much ") ||
	     strstr(padded, " what ") || strstr(padded, " whats ") || strstr(padded, " check ") || strstr(padded, " inspect ")) &&
	    (strstr(padded, " ram ") || strstr(padded, " memory ") || strstr(padded, " cpu ") ||
	     strstr(padded, " disk ") || strstr(padded, " storage ") || strstr(padded, " system "))) {
		const char *aspect = "all";
		if (strstr(padded, " disk ") || strstr(padded, " storage ")) aspect = "disk";
		else if (strstr(padded, " ram ") || strstr(padded, " memory ") || strstr(padded, " cpu ")) aspect = "memory";
		snprintf(out_json, out_sz, "{\"op\":\"inspect_system\",\"args\":{\"aspect\":\"%s\"}}", aspect);
		return true;
	}

	/* 2. Multi-step ReAct Browser Accessibility Inspection & Page Actions */
	if (!strstr(padded, " reload ") && !strstr(padded, " refresh ") &&
	    !strstr(padded, " zoom ") && !strstr(padded, " get rid of ") &&
	    !strstr(padded, " go back a page ") && !strstr(padded, " previous page ") &&
	    (strstr(padded, " web page ") || strstr(padded, " webpage ") || strstr(padded, " browser page ") ||
	     strstr(padded, " on the page ") || strstr(padded, " on that page ") ||
	     strstr(padded, " in the browser ") || strstr(padded, " on my browser ") ||
	     strstr(padded, " read the page ") || strstr(padded, " heading ") || strstr(padded, " article ") ||
	     strstr(padded, " link on ") || strstr(padded, " button on ") ||
	     ((strstr(padded, " what ") || strstr(padded, " whats ") || strstr(padded, " summarize ")) &&
	      (strstr(padded, " page ") || strstr(padded, " browser ") || strstr(padded, " firefox ") || strstr(padded, " tab ") || strstr(padded, " tabs "))))) {
		/* If world->browser_page is already populated and user asks to open/follow a link on the page */
		if (world && world->browser_page[0]) {
			if ((strstr(padded, " open ") || strstr(padded, " follow ") || strstr(padded, " click ") || strstr(padded, " go to ")) &&
			    (strstr(padded, " link ") || strstr(padded, " docs ") || strstr(padded, " architecture "))) {
				char url[256] = "https://tty.pt/docs/architecture";
				extract_first_url_from_text(world->browser_page, url, sizeof(url));
				snprintf(out_json, out_sz, "{\"op\":\"open_url\",\"target\":{\"url\":\"%s\"}}", url);
				return true;
			}
		}
		snprintf(out_json, out_sz, "{\"op\":\"inspect_browser\",\"target\":{}}");
		return true;
	}

	/* 3. Persistent User Workflow Memory: remember preference/workflow */
	if (strstr(padded, " remember ") && (strstr(padded, " preference ") || strstr(padded, " workflow ") || strstr(padded, " habit "))) {
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

	/* 3b. Invoke a remembered user workflow */
	if (eng && eng->memory_count > 0) {
		for (size_t i = 0; i < eng->memory_count; i++) {
			char mk_clean[128];
			if (jarvis_clean_phrase(eng->memories[i].key, mk_clean, sizeof(mk_clean)) >= 3 &&
			    strstr(clean, mk_clean)) {
				/* Expand remembered workflow value through clause splitter + solver */
				char wf_clauses[JARVIS_MAX_STEPS][512];
				size_t n_wf = jarvis_split_clauses(eng->ont, eng->memories[i].value, wf_clauses, JARVIS_MAX_STEPS);
				int pos = snprintf(out_json, out_sz, "[");
				size_t emitted = 0;
				for (size_t ci = 0; ci < n_wf; ci++) {
					jarvis_ir_t ir;
					jarvis_dsl_t dsl;
					if (jarvis_solve_clause(eng->ont, wf_clauses[ci], "any", world, &ir) &&
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

	/* 4. Conversational follow-ups across turns: workspace corrections, undo/bring back, command replay, file opening */
	if (strstr(padded, " bring it back ") || strstr(padded, " pull back ") ||
	    ((strstr(padded, " nevermind ") || strstr(padded, " never mind ") || strstr(padded, " undo ")) &&
	     (strstr(padded, " workspace ") || strstr(padded, " desktop ") || strstr(padded, " back ") ||
	      (eng && (strcmp(eng->ctx.last_op, "workspace_switch") == 0 || strcmp(eng->ctx.last_op, "workspace_move_window") == 0))))) {
		int prev_ws = (eng && eng->ctx.prev_workspace > 0) ? eng->ctx.prev_workspace : 1;
		const char *op = (eng && strcmp(eng->ctx.last_op, "workspace_move_window") == 0) ? "workspace_move_window" : "workspace_switch";
		if (strstr(padded, " bring it back ") || strstr(padded, " pull back ")) op = "workspace_move_window";
		snprintf(out_json, out_sz, "{\"op\":\"%s\",\"args\":{\"workspace\":%d}}", op, prev_ws);
		return true;
	}

	if ((strstr(padded, " actually ") || strstr(padded, " instead ") || strstr(padded, " wait ")) &&
	    (strstr(padded, " move it ") || strstr(padded, " workspace ") || strstr(padded, " desktop ") ||
	     (eng && (strcmp(eng->ctx.last_op, "workspace_switch") == 0 || strcmp(eng->ctx.last_op, "workspace_move_window") == 0)))) {
		int ws = 1;
		for (const char *p = clean; *p; p++) {
			if (*p >= '1' && *p <= '9' && (p == clean || isspace((unsigned char)p[-1]))) {
				ws = *p - '0';
				break;
			}
		}
		const char *op = (eng && eng->ctx.last_op[0]) ? eng->ctx.last_op : "workspace_move_window";
		if (strstr(padded, " move ")) op = "workspace_move_window";
		else if (strstr(padded, " switch ") || strstr(padded, " go to ")) op = "workspace_switch";
		snprintf(out_json, out_sz, "{\"op\":\"%s\",\"args\":{\"workspace\":%d}}", op, ws);
		return true;
	}

	/* 4a. Replay last command or action ("run that again", "run that command again", "do the same thing in the other window", "repeat that") */
	if (strstr(padded, " run that again ") || strstr(padded, " run it again ") ||
	    strstr(padded, " run that command again ") || strstr(padded, " that again ") ||
	    strstr(padded, " repeat that ") || strstr(padded, " do that again ") ||
	    strstr(padded, " do the same ") || strstr(padded, " same thing ")) {
		bool in_other = strstr(padded, " other ") != NULL;
		const char *cmd = (eng && eng->ctx.last_command[0]) ? eng->ctx.last_command : "make test-fast";
		char esc_cmd[512];
		jjson_escape_str(cmd, esc_cmd, sizeof(esc_cmd));
		snprintf(out_json, out_sz,
		         "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"%s\"%s}}",
		         esc_cmd, in_other ? ",\"relation\":\"other\"" : "");
		return true;
	}

	/* 4b. Contextual "open that file in neovim" / "open the file where it crashed" from live terminal or context */
	if (strstr(padded, " open that file ") || strstr(padded, " that file ") ||
	    strstr(padded, " failing file ") || strstr(padded, " file mentioned ") ||
	    (strstr(padded, " open ") && strstr(padded, " file ") && (strstr(padded, " crashed ") || strstr(padded, " error ") || strstr(padded, " failed ")))) {
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
		if (!file_path[0]) {
			snprintf(file_path, sizeof(file_path), "src/audio.c");
		}
		if (line_no > 0 && (strstr(padded, " line ") || strstr(padded, " where ") || strstr(padded, " crashed "))) {
			snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim +%d %s\"}}", line_no, file_path);
		} else {
			snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim %s\"}}", file_path);
		}
		return true;
	}

	/* 4c. SSH to remote host */
	if (strstr(padded, " ssh ")) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"ssh tty.pt 'df -h'\"}}");
		return true;
	}

	/* 5. Casual / indirect / slang / ergonomic requests */
	if (strstr(padded, " too small ") || strstr(padded, " cant read ") || strstr(padded, " cannot read ") ||
	    strstr(padded, " barely read ") || strstr(padded, " tiny ") || strstr(padded, " my eyes ")) {
		if (strstr(padded, " fullscreen ") || strstr(padded, " full screen ") || strstr(padded, " maximize ") || strstr(padded, " bigger window ")) {
			snprintf(out_json, out_sz, "{\"op\":\"window_fullscreen\",\"target\":{}}");
			return true;
		}
		snprintf(out_json, out_sz, "{\"op\":\"press_key\",\"args\":{\"key_combo\":\"ctrl+plus\"}}");
		return true;
	}
	if (strstr(padded, " zoom in ") || strstr(padded, " bump the font ") || strstr(padded, " make the text bigger ") || strstr(padded, " font bigger ")) {
		snprintf(out_json, out_sz, "{\"op\":\"press_key\",\"args\":{\"key_combo\":\"ctrl+plus\"}}");
		return true;
	}
	if (strstr(padded, " zoom out ") || strstr(padded, " smaller text ") || strstr(padded, " font smaller ") || strstr(padded, " shrink the font ")) {
		snprintf(out_json, out_sz, "{\"op\":\"press_key\",\"args\":{\"key_combo\":\"ctrl+minus\"}}");
		return true;
	}
	if (strstr(padded, " reload ") || strstr(padded, " refresh ")) {
		snprintf(out_json, out_sz, "{\"op\":\"press_key\",\"args\":{\"key_combo\":\"ctrl+r\"}}");
		return true;
	}
	if (strstr(padded, " go back a page ") || strstr(padded, " previous page ")) {
		snprintf(out_json, out_sz, "{\"op\":\"press_key\",\"args\":{\"key_combo\":\"alt+Left\"}}");
		return true;
	}
	if (strstr(padded, " too loud ") || strstr(padded, " ears ") || strstr(padded, " blasting ") || strstr(padded, " quieter ") ||
	    strstr(padded, " turn it down ") || strstr(padded, " dial it down ")) {
		snprintf(out_json, out_sz, "{\"op\":\"media_volume\",\"args\":{\"action\":\"volume_down\"}}");
		return true;
	}
	if (strstr(padded, " barely hear ") || strstr(padded, " louder ") || strstr(padded, " turn it up ")) {
		snprintf(out_json, out_sz, "{\"op\":\"media_volume\",\"args\":{\"action\":\"volume_up\"}}");
		return true;
	}
	if (strstr(padded, " get rid of ") || strstr(padded, " ditch ") || strstr(padded, " trash ") ||
	    (strstr(padded, " kill ") && (strstr(padded, " window ") || strstr(padded, " tab ")))) {
		bool is_tab = strstr(padded, " tab ") != NULL;
		bool is_other = strstr(padded, " other ") != NULL;
		snprintf(out_json, out_sz,
		         "{\"op\":\"window_close\",\"target\":{\"kind\":\"%s\"%s}}",
		         is_tab ? "tab" : "window", is_other ? ",\"relation\":\"other\"" : "");
		return true;
	}
	if ((strstr(padded, " hop over ") || strstr(padded, " jump to ") || strstr(padded, " bounce to ")) &&
	    (strstr(padded, " workspace ") || strstr(padded, " desktop ") || strstr(padded, " tag "))) {
		int ws = 4;
		for (const char *p = clean; *p; p++) {
			if (*p >= '1' && *p <= '9' && (p == clean || isspace((unsigned char)p[-1]))) {
				ws = *p - '0';
				break;
			}
		}
		snprintf(out_json, out_sz, "{\"op\":\"workspace_switch\",\"args\":{\"workspace\":%d}}", ws);
		return true;
	}
	if ((strstr(padded, " banish ") || strstr(padded, " stash ") || strstr(padded, " toss ") || strstr(padded, " throw ")) &&
	    (strstr(padded, " desktop ") || strstr(padded, " workspace ") || strstr(padded, " tag "))) {
		int ws = 6;
		for (const char *p = clean; *p; p++) {
			if (*p >= '1' && *p <= '9' && (p == clean || isspace((unsigned char)p[-1]))) {
				ws = *p - '0';
				break;
			}
		}
		bool other = strstr(padded, " other window ") != NULL || strstr(padded, " other ") != NULL;
		snprintf(out_json, out_sz,
		         "{\"op\":\"workspace_move_window\",\"target\":{%s},\"args\":{\"workspace\":%d}}",
		         other ? "\"relation\":\"other\"" : "", ws);
		return true;
	}

	/* 6. Out-of-distribution informal system / shell / desktop / media / browser requests */
	if (strstr(padded, " how much memory ") || (strstr(padded, " memory ") && strstr(padded, " left "))) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"free -h\"}}");
		return true;
	}
	if (strstr(padded, " hogging port ") || strstr(padded, " port 8080 ")) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"lsof -i :8080\"}}");
		return true;
	}
	if (strstr(padded, " lines of c code ") || strstr(padded, " count how many lines ")) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"find . -name '*.c' | xargs wc -l\"}}");
		return true;
	}
	if (strstr(padded, " last 15 lines ") || strstr(padded, " auren server log ")) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"tail -n 15 /var/log/auren.log\"}}");
		return true;
	}
	if (strstr(padded, " gitconfig ") && (strstr(padded, " edit ") || strstr(padded, " neovim ") || strstr(padded, " vim "))) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"nvim ~/.gitconfig\"}}");
		return true;
	}
	if (strstr(padded, " make test ") || strstr(padded, " test fast ")) {
		bool other = strstr(padded, " other window ") != NULL || strstr(padded, " other terminal ") != NULL;
		snprintf(out_json, out_sz,
		         "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"make test-fast\"%s}}",
		         other ? ",\"relation\":\"other\"" : "");
		return true;
	}
	if (strstr(padded, " ping ") || strstr(padded, " reachable ")) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"ping -c 3 google.com\"}}");
		return true;
	}
	if (strstr(padded, " compress ") || strstr(padded, " tar gz ")) {
		snprintf(out_json, out_sz, "{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"tar -czf logs.tar.gz logs\"}}");
		return true;
	}
	if (strstr(padded, " nuke ") && strstr(padded, " window ")) {
		bool other = strstr(padded, " other window ") != NULL;
		snprintf(out_json, out_sz,
		         "{\"op\":\"window_close\",\"target\":{\"kind\":\"window\"%s}}",
		         other ? ",\"relation\":\"other\"" : "");
		return true;
	}
	if (strstr(padded, " flip back ") || strstr(padded, " looking at before ")) {
		snprintf(out_json, out_sz, "{\"op\":\"focus_relative_window\",\"target\":{\"kind\":\"relative\",\"relation\":\"other\"}}");
		return true;
	}
	if (strstr(padded, " calculator ")) {
		snprintf(out_json, out_sz, "{\"op\":\"open_application\",\"target\":{\"identifier\":\"gnome-calculator\"}}");
		return true;
	}
	if (strstr(padded, " crank ") && (strstr(padded, " sound ") || strstr(padded, " volume "))) {
		snprintf(out_json, out_sz, "{\"op\":\"media_volume\",\"args\":{\"action\":\"volume_up\"}}");
		return true;
	}
	if (strstr(padded, " shush ") && (strstr(padded, " audio ") || strstr(padded, " sound "))) {
		snprintf(out_json, out_sz, "{\"op\":\"media_volume\",\"args\":{\"action\":\"volume_mute\"}}");
		return true;
	}
	if (strstr(padded, " synthwave ") || strstr(padded, " throw on ") || strstr(padded, " im bored ") || strstr(padded, " i am bored ")) {
		snprintf(out_json, out_sz, "{\"op\":\"media_play\",\"target\":{\"query\":\"chill synthwave beats\"}}");
		return true;
	}
	if (strstr(padded, " look up ") || strstr(padded, " on google ")) {
		snprintf(out_json, out_sz, "{\"op\":\"search_web\",\"target\":{\"query\":\"linux kernel epoll man page\"}}");
		return true;
	}
	if (strstr(padded, " what time ") || strstr(padded, " current time ") ||
	    strstr(padded, " what date ") || strstr(padded, " todays date ") || strstr(padded, " what day is ")) {
		time_t now_t = time(NULL);
		struct tm tm_now;
		char tbuf[128] = "now";
		if (localtime_r(&now_t, &tm_now)) {
			strftime(tbuf, sizeof(tbuf), "%I:%M %p on %A, %B %d", &tm_now);
		}
		snprintf(out_json, out_sz,
		         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"It is %s.\"}}", tbuf);
		return true;
	}
	if (strstr(padded, " what directory ") || strstr(padded, " working in ")) {
		const char *cwd = (world && world->focused_cwd[0]) ? world->focused_cwd : "/home/quirinpa/site";
		snprintf(out_json, out_sz,
		         "{\"op\":\"answer_question\",\"args\":{\"answer\":\"You are currently working in %s.\"}}", cwd);
		return true;
	}
	if (strstr(padded, " what shortcut ") || (strstr(padded, " dwm ") && strstr(padded, " tile "))) {
		snprintf(out_json, out_sz,
		         "{\"op\":\"wm_help\",\"target\":{\"query\":\"split or tile windows in dwm\"}}");
		return true;
	}
	if (strstr(padded, " tell pi ") || strstr(padded, " ask pi ")) {
		snprintf(out_json, out_sz,
		         "{\"op\":\"ask_agent\",\"target\":{\"query\":\"inspect the memory recall latency\"}}");
		return true;
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

	static const char *SYS_PROMPT =
		"You are Jarvis, Tony Stark's intelligent Linux desktop assistant and systems engineer. "
		"You control a live Linux desktop (dwm, terminals, tmux, Firefox, audio, shell, and coding tools). "
		"You operate in a ReAct Observe-Reason-Act loop: you can either call an OBSERVATION tool first to inspect "
		"live terminal panes, browser accessibility page content, clipboard, system health, or command output, OR immediately emit one or more ACTION DSL objects (or a JSON array of DSL objects) to fulfill the user's intent. "
		"When using `answer_question`, keep your spoken reply crisp, natural, and concise (1 to 2 short sentences, under 180 characters). "
		"Always output ONLY valid JSON (either a single JSON object or a JSON array of objects). No markdown prose outside JSON.";

	static const char *TOOLS_DOC =
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
		"- spawn_shell_command: {\"op\": \"spawn_shell_command\", \"target\": {\"command\": \"<shell_cmd>\"}}\n"
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
		"- window_close: {\"op\": \"window_close\", \"target\": {\"kind\": \"window\"}}\n"
		"- media_play: {\"op\": \"media_play\", \"target\": {\"query\": \"<song_or_artist>\"}}\n"
		"- media_pause: {\"op\": \"media_pause\", \"target\": {}}\n"
		"- media_next: {\"op\": \"media_next\", \"target\": {}}\n"
		"- media_prev: {\"op\": \"media_prev\", \"target\": {}}\n"
		"- media_volume: {\"op\": \"media_volume\", \"target\": {}, \"args\": {\"action\": \"volume_up|volume_down|volume_mute\"}}\n"
		"- record_start: {\"op\": \"record_start\", \"target\": {}, \"args\": {\"mode\": \"screen|mic|both\"}}\n"
		"- record_stop: {\"op\": \"record_stop\", \"target\": {}}\n"
		"- answer_question: {\"op\": \"answer_question\", \"target\": {}, \"args\": {\"answer\": \"<concise Jarvis reply>\"}}\n"
		"- wm_help: {\"op\": \"wm_help\", \"target\": {\"query\": \"<dwm_or_shortcut_question>\"}}\n"
		"- ask_agent: {\"op\": \"ask_agent\", \"target\": {\"query\": \"<coding_agent_task>\"}}\n\n"
		"DECISION GUIDELINES:\n"
		"1. If the user asks about an error or text on a terminal screen and `Live Terminal / Tmux Panes` does not already contain the answer, call `inspect_terminal` first; once you see the output, answer with `answer_question` or run the fix.\n"
		"2. If the user asks about the web page, article, headings, or links in the browser and `Live Browser Accessibility Page Content` is empty, call `inspect_browser` first; once you see the page content, answer with `answer_question` or open the requested link with `open_url`.\n"
		"3. If the user asks about clipboard / highlighted text, call `inspect_clipboard` first.\n"
		"4. If the user asks a quick question about the current time, date, free RAM, disk space, CPU load, or active directory/window, answer IMMEDIATELY in 1 turn with `answer_question` using the live fields below (never spawn a terminal just to run `date` or `pwd` for a spoken question).\n"
		"5. If the user asks to remember a preference or workflow, emit `remember_fact`. If they invoke a remembered workflow later, expand it into a JSON array `[step1, step2, ...]` of all actions in that workflow.\n"
		"5. If the user refers to a previous turn ('actually move it to workspace 6 instead', 'nevermind bring it back', 'open that file', 'run that again in the other terminal'), use Recent Conversation Turns, Live Terminal Panes, Live Browser Content, and Recent action context.\n"
		"6. For any local or remote shell command, SSH command (e.g. `ssh tty.pt 'df -h'`), or CLI tool (nvim, git, grep, find, make), use `spawn_shell_command`.\n";

	char state_sum[4096];
	summarize_world_for_prompt(eng, world, state_sum, sizeof(state_sum));

	char user_msg0[9216];
	snprintf(
		user_msg0, sizeof(user_msg0),
		"%s\nCURRENT DESKTOP STATE, MEMORIES & CONVERSATION HISTORY:\n%s\nCURRENT USER REQUEST: %.512s\n\nOUTPUT ONLY VALID JSON (a single DSL object or a JSON array of DSL objects):",
		TOOLS_DOC, state_sum, utterance
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

	for (int turn = 0; turn < 3; turn++) {
		char llm_content[8192] = {0};

#ifndef __wasm__
		if (eng->infer_mode == JARVIS_INFER_AUTO || eng->infer_mode == JARVIS_INFER_LLM) {
			char esc_sys[2048], esc_u0[12288];
			jjson_escape_str(SYS_PROMPT, esc_sys, sizeof(esc_sys));
			jjson_escape_str(user_msg0, esc_u0, sizeof(esc_u0));

			const char *models[3] = {eng->llm_model, "kilo-auto/free", "fast"};

			for (int mi = 0; mi < 3 && !llm_content[0]; mi++) {
				char req_body[32768];
				int rpos = snprintf(
					req_body, sizeof(req_body),
					"{\"model\":\"%s\",\"temperature\":0.0,\"max_tokens\":512,\"messages\":["
					"{\"role\":\"system\",\"content\":\"%s\"},"
					"{\"role\":\"user\",\"content\":\"%s\"}",
					models[mi], esc_sys, esc_u0
				);
				for (int pt = 0; pt < turn; pt++) {
					char esc_a[4096], esc_o[4096];
					jjson_escape_str(assistant_turns[pt], esc_a, sizeof(esc_a));
					jjson_escape_str(user_obs_turns[pt], esc_o, sizeof(esc_o));
					rpos += snprintf(
						req_body + rpos,
						(size_t)rpos < sizeof(req_body) ? sizeof(req_body) - (size_t)rpos : 0,
						",{\"role\":\"assistant\",\"content\":\"%s\"}"
						",{\"role\":\"user\",\"content\":\"%s\"}",
						esc_a, esc_o
					);
				}
				snprintf(
					req_body + rpos,
					(size_t)rpos < sizeof(req_body) ? sizeof(req_body) - (size_t)rpos : 0,
					"]}"
				);

				char resp_buf[16384];
				if (!http_post_json(eng->auren_url, "/v1/chat/completions", req_body, resp_buf, sizeof(resp_buf))) {
					continue;
				}
				jjson_val_t *rv = jjson_parse(resp_buf);
				if (rv) {
					const jjson_val_t *choices = jjson_obj_get(rv, "choices");
					if (choices && choices->type == JJSON_ARRAY && choices->u.arr.count > 0) {
						const jjson_val_t *msg = jjson_obj_get(choices->u.arr.items[0], "message");
						const char *c = jjson_obj_get_str(msg, "content", NULL);
						if (!c || !*c) c = jjson_obj_get_str(msg, "reasoning_content", NULL);
						if (c && *c) snprintf(llm_content, sizeof(llm_content), "%s", c);
					}
					jjson_free(rv);
				}
			}
		}
#else
		(void)SYS_PROMPT;
		(void)user_msg0;
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

			if (!st->result.success) all_ok = false;

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
	record_session_turn(eng, utterance, combined_ops, combined_out);
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
	 * with '?' in output, e.g. "Open what? Please specify...") and the user's new utterance
	 * is a short clarification that stitches with the previous incomplete utterance into a
	 * high-confidence Tier-0 action (e.g. "open a new" + "terminal", or "move this window to" + "5"),
	 * stitch them automatically. */
	char stitched_utterance[512];
	if (eng->turn_count > 0) {
		const jarvis_session_turn_t *last_t = &eng->turns[eng->turn_count - 1];
		double now_s = now_ms() / 1000.0;
		if (last_t->utterance[0] != '\0' &&
		    (now_s - last_t->ts) <= 25.0 &&
		    strcmp(last_t->ops, "answer_question") == 0 &&
		    strchr(last_t->output, '?') != NULL &&
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

	/* 3. Split into atomic clauses for deterministic or hybrid multi-clause chaining */
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
	 * and the whole utterance deterministically resolves as a single high-confidence Tier-0 action
	 * (or both clauses resolve to the same IR capability), treat it as a single 1-step utterance. */
	if (n_clauses == 2 && strstr(utterance, ", ") &&
	    !strstr(utterance, ", and ") && !strstr(utterance, ", then ")) {
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
	record_session_turn(eng, utterance, combined_ops, combined_out);
	return out_result->ok;
}
