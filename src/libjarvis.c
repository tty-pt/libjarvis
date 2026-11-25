#include "jarvis-internal.h"

#ifndef __wasm__
#include "jarvis-platform.h"
#endif

static int j_strcasecmp(const char *a, const char *b)
{
	if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
	for (;; a++, b++) {
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb || ca == 0) return ca - cb;
	}
}

static int j_strncasecmp(const char *a, const char *b, size_t n)
{
	if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
	for (size_t i = 0; i < n; i++) {
		int ca = tolower((unsigned char)a[i]);
		int cb = tolower((unsigned char)b[i]);
		if (ca != cb || ca == 0) return ca - cb;
	}
	return 0;
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

	/* 1. Strong sentence/clause punctuation (. ! ? ; : \n) followed by whitespace */
	size_t max_sent = is_first_clause ? 54 : 160;
	for (size_t i = 3; i <= max_sent && i + 1 < len; i++) {
		char c = unspoken[i];
		if ((c == '.' || c == '!' || c == '?' || c == ';' || c == ':' || c == '\n') &&
		    (isspace((unsigned char)unspoken[i + 1]) || unspoken[i + 1] == '\0')) {
			size_t clen = (i + 1) < clause_sz - 1 ? (i + 1) : clause_sz - 1;
			memcpy(out_clause, unspoken, clen);
			out_clause[clen] = '\0';
			size_t j = i + 1;
			while (j < len && isspace((unsigned char)unspoken[j])) j++;
			snprintf(out_remainder, rem_sz, "%s", unspoken + j);
			return true;
		}
	}

	/* 2. Comma boundary after a natural initial phrase */
	size_t min_comma = is_first_clause ? 14 : 28;
	size_t max_comma = is_first_clause ? 54 : 120;
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

	/* 3. For the very first clause without early punctuation, split at preposition/conjunction (18-50 chars) */
	if (is_first_clause && len >= 34) {
		for (size_t i = 18; i <= 50 && i + 2 < len; i++) {
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

	/* 4. Fallback word boundary if a long unpunctuated run exceeds limit */
	size_t max_run = is_first_clause ? 52 : 110;
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
 * ~/.jarvisrc Configuration Helpers
 * ------------------------------------------------------------------------- */

static void resolve_jarvisrc_path(char *out, size_t out_sz, bool for_write)
{
	const char *env_rc = getenv("JARVISRC");
	if (env_rc && *env_rc) {
		snprintf(out, out_sz, "%s", env_rc);
		return;
	}
	char hbuf[512] = {0};
	const char *home = getenv("HOME");
#ifndef __wasm__
	if (!home || !*home) {
		stl_home_dir(hbuf, sizeof(hbuf));
		home = hbuf[0] ? hbuf : ".";
	}
#else
	if (!home || !*home) home = "/tmp";
#endif
	snprintf(out, out_sz, "%s/.jarvisrc", home);
#ifndef __wasm__
	if (!for_write && !jarvis_plat_is_readable(out)) {
		char alt[512];
		snprintf(alt, sizeof(alt), "%s/.config/jarvis/jarvisrc", home);
		if (jarvis_plat_is_readable(alt)) {
			snprintf(out, out_sz, "%s", alt);
			return;
		}
		char cdir[512];
		if (stl_config_dir("jarvis", cdir, sizeof(cdir)) && cdir[0]) {
			snprintf(alt, sizeof(alt), "%s/jarvisrc", cdir);
			if (jarvis_plat_is_readable(alt)) {
				snprintf(out, out_sz, "%s", alt);
			}
		}
	}
#else
	(void)for_write;
#endif
}

static void expand_tilde_path(const char *val, char *out, size_t out_sz)
{
	if (!val || !out || out_sz == 0) return;
	if (val[0] == '~' && (val[1] == '/' || val[1] == '\\' || val[1] == '\0')) {
		char hbuf[512] = {0};
		const char *home = getenv("HOME");
#ifndef __wasm__
		if (!home || !*home) {
			stl_home_dir(hbuf, sizeof(hbuf));
			home = hbuf[0] ? hbuf : NULL;
		}
#endif
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
		if (j_strcasecmp(k, key) != 0) continue;

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
	if (j_strcasecmp(buf, "true") == 0 || j_strcasecmp(buf, "yes") == 0 || j_strcasecmp(buf, "on") == 0) {
		return 1;
	}
	if (j_strcasecmp(buf, "false") == 0 || j_strcasecmp(buf, "no") == 0 || j_strcasecmp(buf, "off") == 0) {
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
					if (j_strcasecmp(p, key) == 0) {
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
#ifdef _WIN32
	remove(rc_path);
#endif
	return rename(tmp_path, rc_path) == 0;
#else
	(void)key;
	(void)value;
	return false;
#endif
}

bool jarvis_rc_unset(const char *key)
{
	if (!key || !*key) return false;
#ifndef __wasm__
	char rc_path[512];
	resolve_jarvisrc_path(rc_path, sizeof(rc_path), true);

	FILE *fp = fopen(rc_path, "r");
	if (!fp) return true;

	char lines[128][1024];
	size_t n_lines = 0;
	bool removed = false;
	char buf[1024];

	while (fgets(buf, sizeof(buf), fp)) {
		bool match = false;
		char copy[1024];
		snprintf(copy, sizeof(copy), "%s", buf);
		char *p = copy;
		while (*p && isspace((unsigned char)*p)) p++;
		if (*p && *p != '#' && *p != ';') {
			char *eq = strchr(p, '=');
			if (eq) {
				*eq = '\0';
				size_t kl = strlen(p);
				while (kl > 0 && isspace((unsigned char)p[kl - 1])) p[--kl] = '\0';
				if (j_strcasecmp(p, key) == 0) {
					match = true;
					removed = true;
				}
			}
		}
		if (!match && n_lines < 128) {
			snprintf(lines[n_lines++], sizeof(lines[0]), "%s", buf);
		}
	}
	fclose(fp);
	if (!removed) return true;

	char tmp_path[540];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", rc_path);
	FILE *out = fopen(tmp_path, "w");
	if (!out) return false;
	for (size_t i = 0; i < n_lines; i++) {
		fputs(lines[i], out);
	}
	fclose(out);
#ifdef _WIN32
	remove(rc_path);
#endif
	return rename(tmp_path, rc_path) == 0;
#else
	(void)key;
	return false;
#endif
}

static bool normalize_lang_tag(const char *raw, char *out, size_t out_sz)
{
	if (!raw || !*raw || !out || out_sz == 0) return false;
	char buf[32];
	size_t w = 0;
	while (*raw && isspace((unsigned char)*raw)) raw++;
	for (size_t i = 0; raw[i] && w + 1 < sizeof(buf); i++) {
		char c = raw[i];
		if (c == '.' || c == '@') break;
		if (c == '_') c = '-';
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		buf[w++] = c;
	}
	while (w > 0 && isspace((unsigned char)buf[w - 1])) w--;
	buf[w] = '\0';
	if (!buf[0] || strcmp(buf, "c") == 0 || strcmp(buf, "posix") == 0) {
		return false;
	}
	snprintf(out, out_sz, "%s", buf);
	return true;
}

static bool resolve_kbd_layout_lang(char *out, size_t out_sz)
{
#ifndef __wasm__
	char path[512], buf[32] = {0};
	if (jarvis_plat_state_path("dwm-kbd", ".state", path, sizeof(path)) == 0) {
		FILE *fp = fopen(path, "r");
		if (fp) {
			bool ok = false;
			if (fgets(buf, sizeof(buf), fp)) {
				char code[8] = {0};
				size_t ci = 0;
				for (size_t i = 0; buf[i] && ci < 2; i++) {
					if (isalpha((unsigned char)buf[i])) {
						code[ci++] = (char)tolower((unsigned char)buf[i]);
					}
				}
				code[ci] = '\0';
				if (strcmp(code, "us") == 0 || strcmp(code, "gb") == 0 || strcmp(code, "uk") == 0) {
					snprintf(out, out_sz, "en");
					ok = true;
				} else if (strcmp(code, "br") == 0) {
					snprintf(out, out_sz, "pt-br");
					ok = true;
				} else if (ci == 2) {
					snprintf(out, out_sz, "%s", code);
					ok = true;
				}
			}
			fclose(fp);
			if (ok) return true;
		}
	}
	jarvis_plat_kbd_lang(out, out_sz);
	return out[0] != '\0';
#else
	(void)out;
	(void)out_sz;
	return false;
#endif
}

void jarvis_resolve_lang(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';

	char tag[32] = {0};
	const char *env_l = getenv("JARVIS_LANG");
	if (env_l && normalize_lang_tag(env_l, tag, sizeof(tag))) {
		if (strcmp(tag, "kbd") == 0) {
			if (resolve_kbd_layout_lang(out, out_sz)) return;
		} else if (strcmp(tag, "auto") != 0) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}

	char rc_lang[32] = {0};
	if (jarvis_rc_get("lang", NULL, rc_lang, sizeof(rc_lang)) &&
	    normalize_lang_tag(rc_lang, tag, sizeof(tag))) {
		if (strcmp(tag, "kbd") == 0) {
			if (resolve_kbd_layout_lang(out, out_sz)) return;
		} else if (strcmp(tag, "auto") != 0) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}

	static const char *const lc_vars[] = { "LC_ALL", "LC_MESSAGES", "LANG", NULL };
	for (size_t i = 0; lc_vars[i]; i++) {
		const char *lv = getenv(lc_vars[i]);
		if (lv && normalize_lang_tag(lv, tag, sizeof(tag))) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}

	if (ont) {
		const char *ol = jarvis_ontology_get_lang(ont);
		if (ol && normalize_lang_tag(ol, tag, sizeof(tag))) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}

	snprintf(out, out_sz, "en");
}

void jarvis_resolve_wake_word(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';

	const char *env_w = getenv("JARVIS_WAKE_WORD");
	if (env_w && *env_w) {
		snprintf(out, out_sz, "%s", env_w);
		return;
	}

	char rc_wake[64] = {0};
	if (jarvis_rc_get("wake_word", NULL, rc_wake, sizeof(rc_wake)) &&
	    rc_wake[0] && j_strcasecmp(rc_wake, "auto") != 0) {
		snprintf(out, out_sz, "%s", rc_wake);
		return;
	}

	if (ont) {
		const char *ow = jarvis_ontology_get_wake_word(ont);
		if (ow && *ow && strcmp(ow, "assistant") != 0) {
			snprintf(out, out_sz, "%s", ow);
			return;
		}
	}

	char lang[32] = {0};
	jarvis_resolve_lang(ont, lang, sizeof(lang));
	if (strncmp(lang, "pt", 2) == 0) {
		snprintf(out, out_sz, "jarbas");
	} else {
		snprintf(out, out_sz, "jarvis");
	}
}

static bool is_wake_ack_disabled_val(const char *v)
{
	if (!v || !*v) return false;
	return j_strcasecmp(v, "off") == 0 ||
	       j_strcasecmp(v, "none") == 0 ||
	       j_strcasecmp(v, "0") == 0 ||
	       j_strcasecmp(v, "false") == 0;
}

bool jarvis_resolve_wake_ack(const jarvis_ontology_t *ont, const char *lang_hint, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';

	const char *env_ack = getenv("JARVIS_WAKE_ACK");
	if (env_ack && *env_ack) {
		if (is_wake_ack_disabled_val(env_ack)) return false;
		if (j_strcasecmp(env_ack, "auto") != 0) {
			snprintf(out, out_sz, "%s", env_ack);
			return true;
		}
	}

	char rc_ack[128] = {0};
	if (jarvis_rc_get("wake_ack", NULL, rc_ack, sizeof(rc_ack)) && rc_ack[0]) {
		if (is_wake_ack_disabled_val(rc_ack)) return false;
		if (j_strcasecmp(rc_ack, "auto") != 0) {
			snprintf(out, out_sz, "%s", rc_ack);
			return true;
		}
	}

	char eff_lang[32] = {0};
	if (lang_hint && normalize_lang_tag(lang_hint, eff_lang, sizeof(eff_lang)) &&
	    strcmp(eff_lang, "auto") != 0 && strcmp(eff_lang, "kbd") != 0) {
		/* Use caller-supplied effective language */
	} else {
		jarvis_resolve_lang(ont, eff_lang, sizeof(eff_lang));
	}

	const char *ack = jarvis_ontology_get_wake_ack_for_lang(ont, eff_lang);
	if (ack && *ack) {
		snprintf(out, out_sz, "%s", ack);
		return true;
	}

	snprintf(out, out_sz, "Yes?");
	return true;
}

void jarvis_resolve_tts_voice(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';

	const char *env_v = getenv("JARVIS_VOICE");
	if (env_v && *env_v && j_strcasecmp(env_v, "auto") != 0) {
		snprintf(out, out_sz, "%s", env_v);
		return;
	}

	char rc_v[64] = {0};
	if (jarvis_rc_get("tts_voice", NULL, rc_v, sizeof(rc_v)) &&
	    rc_v[0] && j_strcasecmp(rc_v, "auto") != 0) {
		snprintf(out, out_sz, "%s", rc_v);
		return;
	}

	char lang[32] = {0};
	const char *env_tl = getenv("JARVIS_TTS_LANG");
	if (!env_tl || !normalize_lang_tag(env_tl, lang, sizeof(lang)) || strcmp(lang, "auto") == 0) {
		char rc_tl[32] = {0};
		if (!jarvis_rc_get("tts_lang", NULL, rc_tl, sizeof(rc_tl)) ||
		    !normalize_lang_tag(rc_tl, lang, sizeof(lang)) || strcmp(lang, "auto") == 0) {
			jarvis_resolve_lang(ont, lang, sizeof(lang));
		}
	}

	static const struct { const char *prefix; const char *voice; } voice_map[] = {
		{ "pt-br", "edge:pt-BR-FranciscaNeural" },
		{ "pt",    "edge:pt-PT-RaquelNeural"    },
		{ "en-us", "edge:en-US-AndrewNeural"    },
		{ "en",    "edge:en-GB-RyanNeural"      },
		{ "es",    "edge:es-ES-AlvaroNeural"    },
		{ "fr",    "edge:fr-FR-HenriNeural"     },
		{ "de",    "edge:de-DE-ConradNeural"    },
		{ "it",    "edge:it-IT-DiegoNeural"     },
		{ "nl",    "edge:nl-NL-MaartenNeural"   },
		{ "pl",    "edge:pl-PL-MarekNeural"     },
		{ "ru",    "edge:ru-RU-DmitryNeural"    },
		{ "uk",    "edge:uk-UA-OstapNeural"     },
		{ "ja",    "edge:ja-JP-KeitaNeural"     },
		{ "zh",    "edge:zh-CN-YunxiNeural"     },
		{ "ko",    "edge:ko-KR-InJoonNeural"    },
		{ "ar",    "edge:ar-SA-HamedNeural"     },
		{ "hi",    "edge:hi-IN-MadhurNeural"    },
		{ "tr",    "edge:tr-TR-AhmetNeural"     },
		{ "sv",    "edge:sv-SE-MattiasNeural"   },
		{ "no",    "edge:nb-NO-FinnNeural"      },
		{ "nb",    "edge:nb-NO-FinnNeural"      },
		{ "da",    "edge:da-DK-JeppeNeural"     },
		{ "fi",    "edge:fi-FI-HarriNeural"     },
		{ "cs",    "edge:cs-CZ-AntoninNeural"   },
		{ "el",    "edge:el-GR-NestorasNeural"  },
		{ "ro",    "edge:ro-RO-EmilNeural"      },
		{ "hu",    "edge:hu-HU-TamasNeural"     },
		{ "he",    "edge:he-IL-AvriNeural"      },
		{ "vi",    "edge:vi-VN-NamMinhNeural"   },
		{ "th",    "edge:th-TH-NiwatNeural"     },
		{ "id",    "edge:id-ID-ArdiNeural"      },
		{ "ca",    "edge:ca-ES-EnricNeural"     },
		{ "gl",    "edge:gl-ES-RoiNeural"       },
		{ NULL,    NULL }
	};
	for (size_t i = 0; voice_map[i].prefix; i++) {
		size_t plen = strlen(voice_map[i].prefix);
		if (strncmp(lang, voice_map[i].prefix, plen) == 0 &&
		    (lang[plen] == '\0' || lang[plen] == '-')) {
			snprintf(out, out_sz, "%s", voice_map[i].voice);
			return;
		}
	}
	snprintf(out, out_sz, "edge:en-GB-RyanNeural");
}

static bool str_contains_ci(const char *hay, const char *needle)
{
	if (!hay || !needle || !*needle) return false;
	size_t nlen = strlen(needle);
	for (const char *p = hay; *p; p++) {
		if (j_strncasecmp(p, needle, nlen) == 0) return true;
	}
	return false;
}

void jarvis_resolve_wm(char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';

	const char *env_wm = getenv("JARVIS_WM");
	if (env_wm && *env_wm && j_strcasecmp(env_wm, "auto") != 0) {
		snprintf(out, out_sz, "%s", env_wm);
		return;
	}

	char rc_wm[64] = {0};
	if (jarvis_rc_get("wm", NULL, rc_wm, sizeof(rc_wm)) &&
	    rc_wm[0] && j_strcasecmp(rc_wm, "auto") != 0) {
		snprintf(out, out_sz, "%s", rc_wm);
		return;
	}

	const char *hypr = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	if (hypr && *hypr) {
		snprintf(out, out_sz, "hyprland");
		return;
	}

	const char *sway = getenv("SWAYSOCK");
	if (sway && *sway) {
		snprintf(out, out_sz, "sway");
		return;
	}

	const char *xdg_cur = getenv("XDG_CURRENT_DESKTOP");
	const char *dsess = getenv("DESKTOP_SESSION");
	if (str_contains_ci(xdg_cur, "hyprland") || str_contains_ci(dsess, "hyprland")) {
		snprintf(out, out_sz, "hyprland");
		return;
	}
	if (str_contains_ci(xdg_cur, "sway") || str_contains_ci(dsess, "sway")) {
		snprintf(out, out_sz, "sway");
		return;
	}
	if (str_contains_ci(xdg_cur, "dwm") || str_contains_ci(dsess, "dwm")) {
		snprintf(out, out_sz, "dwm");
		return;
	}

#ifndef __wasm__
	char pid_p[512];
	if (jarvis_plat_state_path("dwm-bar", ".pid", pid_p, sizeof(pid_p)) == 0 &&
	    jarvis_plat_is_readable(pid_p)) {
		snprintf(out, out_sz, "dwm");
		return;
	}
#endif

	const char *disp = getenv("DISPLAY");
	if (disp && *disp) {
		snprintf(out, out_sz, "x11");
		return;
	}

#ifdef __APPLE__
	snprintf(out, out_sz, "macos");
#elif defined(_WIN32)
	snprintf(out, out_sz, "win32");
#else
	snprintf(out, out_sz, "desktop");
#endif
}
