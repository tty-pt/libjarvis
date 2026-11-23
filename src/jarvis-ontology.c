#include "jarvis-internal.h"

static const jarvis_ontology_t *g_active_ontology = NULL;
static jarvis_ontology_t       *g_default_ontology_singleton = NULL;

const jarvis_ontology_t *jarvis_ontology_get_active(void)
{
	if (g_active_ontology) return g_active_ontology;
	if (!g_default_ontology_singleton) {
		g_default_ontology_singleton = jarvis_ontology_create(NULL);
	}
	return g_default_ontology_singleton;
}

static char fold_latin1_u8(unsigned char c2)
{
	/* Map U+00C0..U+00FF (UTF-8 0xC3 0x80..0xBF) to lowercase ASCII base */
	if (c2 == 0x9F) return 's';               /* U+00DF ß -> s */
	if (c2 == 0x97 || c2 == 0xB7) return 0;   /* U+00D7 × / U+00F7 ÷ -> separator */
	unsigned char lo = c2 | 0x20;             /* fold 0x80..0x9E to 0xA0..0xBE */
	if (lo >= 0xA0 && lo <= 0xA6) return 'a';
	if (lo == 0xA7) return 'c';
	if (lo >= 0xA8 && lo <= 0xAB) return 'e';
	if (lo >= 0xAC && lo <= 0xAF) return 'i';
	if (lo == 0xB0) return 'd';
	if (lo == 0xB1) return 'n';
	if (lo >= 0xB2 && lo <= 0xB6) return 'o';
	if (lo == 0xB8) return 'o';
	if (lo >= 0xB9 && lo <= 0xBC) return 'u';
	if (lo == 0xBD || lo == 0xBF) return 'y';
	return 0;
}

static uint32_t decode_utf8_cp(const unsigned char **pp)
{
	const unsigned char *p = *pp;
	unsigned char c = *p;
	if (c < 0x80) {
		*pp = p + 1;
		return (uint32_t)tolower(c);
	}
	if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
		uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
		/* Fold Cyrillic uppercase U+0410..U+042F to lowercase U+0430..U+044F */
		if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;
		else if (cp == 0x0401) cp = 0x0451; /* Ё -> ё */
		/* Fold Greek uppercase U+0391..U+03A9 to lowercase U+03B1..U+03C9 */
		else if (cp >= 0x0391 && cp <= 0x03A9) cp += 0x20;
		*pp = p + 2;
		return cp;
	}
	if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
		uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
		              ((uint32_t)(p[1] & 0x3F) << 6) |
		              (uint32_t)(p[2] & 0x3F);
		*pp = p + 3;
		return cp;
	}
	if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
	    (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
		uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
		              ((uint32_t)(p[1] & 0x3F) << 12) |
		              ((uint32_t)(p[2] & 0x3F) << 6) |
		              (uint32_t)(p[3] & 0x3F);
		*pp = p + 4;
		return cp;
	}
	*pp = p + 1;
	return (uint32_t)c;
}

static bool is_non_speech_cp(uint32_t cp)
{
	if (cp <= 0x20) return true;
	/* General Punctuation, CJK Symbols & Punctuation, Fullwidth ASCII punctuation */
	if (cp >= 0x2000 && cp <= 0x206F) return true;
	if (cp >= 0x3000 && cp <= 0x303F) return true;
	if (cp == 0x30FC) return true; /* Katakana-Hiragana prolonged sound mark ー */
	if (cp >= 0xFF01 && cp <= 0xFF0F) return true;
	if (cp >= 0xFF1A && cp <= 0xFF20) return true;
	/* Musical symbols / notes / dingbats / arrows */
	if (cp >= 0x2190 && cp <= 0x27BF) return true;
	if (cp >= 0x1D100 && cp <= 0x1D1FF) return true;
	if (cp >= 0x1F300 && cp <= 0x1F9FF) return true;
	return false;
}

size_t jarvis_clean_phrase(const char *text, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return 0;
	out[0] = '\0';
	if (!text) return 0;

	size_t w = 0;
	bool prev_space = true;

	for (const unsigned char *p = (const unsigned char *)text; *p && w + 1 < out_sz; p++) {
		unsigned char c = *p;
		if ((c == '\'' || c == '`') && p > (const unsigned char *)text &&
		    isalnum(p[-1]) && isalnum(p[1])) {
			continue;
		}
		if (c == 0xC3 && (p[1] & 0xC0) == 0x80) {
			char folded = fold_latin1_u8(p[1]);
			p++;
			if (folded) {
				out[w++] = folded;
				prev_space = false;
			} else if (!prev_space && w + 1 < out_sz) {
				out[w++] = ' ';
				prev_space = true;
			}
			continue;
		}
		/* Fold Cyrillic uppercase А..Я (0xD0 0x90..0xAF) and Ё (0xD0 0x81) to lowercase UTF-8 */
		if (c == 0xD0 && (p[1] & 0xC0) == 0x80 && w + 2 < out_sz) {
			unsigned char c2 = p[1];
			if (c2 >= 0x90 && c2 <= 0x9F) {
				out[w++] = (char)0xD0;
				out[w++] = (char)(c2 + 0x20);
				p++;
				prev_space = false;
				continue;
			}
			if (c2 >= 0xA0 && c2 <= 0xAF) {
				out[w++] = (char)0xD1;
				out[w++] = (char)(c2 - 0x20);
				p++;
				prev_space = false;
				continue;
			}
			if (c2 == 0x81) { /* Ё -> ё (0xD1 0x91) */
				out[w++] = (char)0xD1;
				out[w++] = (char)0x91;
				p++;
				prev_space = false;
				continue;
			}
		}
		/* Fold Greek uppercase Α..Ω (0xCE 0x91..0xA9, excluding unassigned 0xA2) to lowercase UTF-8 */
		if (c == 0xCE && (p[1] & 0xC0) == 0x80 && w + 2 < out_sz) {
			unsigned char c2 = p[1];
			if (c2 >= 0x91 && c2 <= 0x9F) {
				out[w++] = (char)0xCE;
				out[w++] = (char)(c2 + 0x20);
				p++;
				prev_space = false;
				continue;
			}
			if (c2 >= 0xA0 && c2 <= 0xA9 && c2 != 0xA2) {
				out[w++] = (char)0xCF;
				out[w++] = (char)(c2 - 0x20);
				p++;
				prev_space = false;
				continue;
			}
		}
		/* Treat common multi-byte Unicode punctuation (C2 A1..BF, E2 80.., E3 80.., EF BC..) as spaces */
		if (c == 0xC2 && (p[1] == 0xA1 || p[1] == 0xBF || p[1] == 0xAB || p[1] == 0xBB)) {
			p++;
			if (!prev_space && w + 1 < out_sz) {
				out[w++] = ' ';
				prev_space = true;
			}
			continue;
		}
		if ((c == 0xE2 && p[1] == 0x80 && (p[2] & 0xC0) == 0x80) ||
		    (c == 0xE3 && p[1] == 0x80 && (p[2] >= 0x80 && p[2] <= 0x9F)) ||
		    (c == 0xEF && p[1] == 0xBC && ((p[2] >= 0x81 && p[2] <= 0x8F) || (p[2] >= 0x9A && p[2] <= 0xA0)))) {
			p += 2;
			if (!prev_space && w + 1 < out_sz) {
				out[w++] = ' ';
				prev_space = true;
			}
			continue;
		}
		if (isalnum(c) || c >= 0x80) {
			out[w++] = (char)tolower(c);
			prev_space = false;
		} else {
			if (!prev_space && w + 1 < out_sz) {
				out[w++] = ' ';
				prev_space = true;
			}
		}
	}
	if (w > 0 && out[w - 1] == ' ') {
		w--;
	}
	out[w] = '\0';
	return w;
}

static void clean_phrase_preserve_wake_tpl(const char *src, char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0) return;
	dst[0] = '\0';
	if (!src || !*src) return;

	if (!strstr(src, "{wake}") && !strstr(src, "{Wake}")) {
		jarvis_clean_phrase(src, dst, dst_sz);
		return;
	}

	static const char *const SENTINEL = "xjarviswakex";
	const size_t slen = strlen(SENTINEL);
	char masked[JARVIS_STR_LEN];
	size_t mw = 0;
	for (size_t i = 0; src[i] && mw + slen + 1 < sizeof(masked); ) {
		if (strncmp(&src[i], "{wake}", 6) == 0 || strncmp(&src[i], "{Wake}", 6) == 0) {
			memcpy(masked + mw, SENTINEL, slen);
			mw += slen;
			i += 6;
		} else {
			masked[mw++] = src[i++];
		}
	}
	masked[mw] = '\0';

	char cleaned[JARVIS_STR_LEN];
	jarvis_clean_phrase(masked, cleaned, sizeof(cleaned));

	size_t dw = 0;
	for (size_t i = 0; cleaned[i] && dw + 1 < dst_sz; ) {
		if (strncmp(&cleaned[i], SENTINEL, slen) == 0 && dw + 6 < dst_sz) {
			memcpy(dst + dw, "{wake}", 6);
			dw += 6;
			i += slen;
		} else {
			dst[dw++] = cleaned[i++];
		}
	}
	dst[dw] = '\0';
}

static void expand_wake_tpl_str(const char *tpl, const char *wake, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!tpl) return;

	const char *w_lo = (wake && *wake) ? wake : "assistant";
	char w_cap[JARVIS_SHORT_LEN];
	snprintf(w_cap, sizeof(w_cap), "%s", w_lo);
	if (w_cap[0] >= 'a' && w_cap[0] <= 'z') {
		w_cap[0] = (char)(w_cap[0] - 'a' + 'A');
	}

	size_t w = 0;
	for (size_t i = 0; tpl[i] && w + 1 < out_sz; ) {
		if (strncmp(&tpl[i], "{wake}", 6) == 0) {
			w += (size_t)snprintf(out + w, out_sz - w, "%s", w_lo);
			i += 6;
		} else if (strncmp(&tpl[i], "{Wake}", 6) == 0) {
			w += (size_t)snprintf(out + w, out_sz - w, "%s", w_cap);
			i += 6;
		} else {
			out[w++] = tpl[i++];
		}
	}
	if (w < out_sz) out[w] = '\0';
	else out[out_sz - 1] = '\0';
}

bool jarvis_is_non_speech_hallucination(const char *text)
{
	if (!text) return true;
	char clean[JARVIS_CLAUSE_LEN];
	size_t clean_len = jarvis_clean_phrase(text, clean, sizeof(clean));
	if (clean_len < 2) return true;

	const jarvis_ontology_t *ont = jarvis_ontology_get_active();
	if (ont) {
		for (size_t i = 0; i < ont->hallucination_count; i++) {
			if (ont->hallucinations[i][0] && strcmp(clean, ont->hallucinations[i]) == 0) {
				return true;
			}
		}
	}

	uint32_t cps[256];
	size_t n = 0;
	const unsigned char *p = (const unsigned char *)clean;
	while (*p && n < 256) {
		uint32_t cp = decode_utf8_cp(&p);
		if (cp < 0x80) {
			if (isalnum((int)cp)) cps[n++] = cp;
		} else if (!is_non_speech_cp(cp)) {
			cps[n++] = cp;
		}
	}
	if (n < 2) return true;

	size_t max_run = 1;
	size_t cur_run = 1;
	for (size_t i = 1; i < n; i++) {
		if (cps[i] == cps[i - 1]) {
			cur_run++;
			if (cur_run > max_run) max_run = cur_run;
		} else {
			cur_run = 1;
		}
	}
	if (max_run >= 4) return true;

	if (n >= 4) {
		size_t most = 0;
		for (size_t i = 0; i < n; i++) {
			size_t cnt = 1;
			for (size_t j = i + 1; j < n; j++) {
				if (cps[j] == cps[i]) cnt++;
			}
			if (cnt > most) most = cnt;
		}
		if (most * 10 >= n * 8) return true;
	}
	return false;
}

static bool contains_ci_token_seq(const char *hay, const char *tok1, const char *tok2)
{
	char clean[512];
	jarvis_clean_phrase(hay, clean, sizeof(clean));
	/* Also examine raw lowercased string for flags like -rf / --hard */
	char low[512];
	size_t i = 0;
	for (; hay[i] && i + 1 < sizeof(low); i++) low[i] = (char)tolower((unsigned char)hay[i]);
	low[i] = '\0';

	const char *p = strstr(low, tok1);
	if (!p) return false;
	if (p > low && isalnum((unsigned char)p[-1])) return false;
	p += strlen(tok1);
	if (!isspace((unsigned char)*p)) return false;
	while (*p && isspace((unsigned char)*p)) p++;
	if (!tok2) return true;
	return strncmp(p, tok2, strlen(tok2)) == 0;
}

bool jarvis_is_destructive_command(const char *cmd)
{
	if (!cmd || !*cmd) return false;
	char low[512];
	size_t n = 0;
	for (; cmd[n] && n + 1 < sizeof(low); n++) {
		low[n] = (char)tolower((unsigned char)cmd[n]);
	}
	low[n] = '\0';

	/* Check rm -rf / rm -fr / rm -f -r */
	const char *rm = strstr(low, "rm ");
	while (rm) {
		if (rm == low || !isalnum((unsigned char)rm[-1])) {
			const char *arg = rm + 3;
			while (*arg == ' ') arg++;
			if (*arg == '-') {
				bool has_r = false, has_f = false;
				const char *f = arg + 1;
				while (*f && !isspace((unsigned char)*f)) {
					if (*f == 'r') has_r = true;
					if (*f == 'f') has_f = true;
					f++;
				}
				if (has_r && has_f) return true;
			}
		}
		rm = strstr(rm + 2, "rm ");
	}

	if (contains_ci_token_seq(low, "git", "reset --hard")) return true;
	if (contains_ci_token_seq(low, "git", "clean -f") || contains_ci_token_seq(low, "git", "clean -fd") || contains_ci_token_seq(low, "git", "clean -df")) return true;
	if (contains_ci_token_seq(low, "dd", "if=")) return true;
	if (contains_ci_token_seq(low, "kill", "-9")) return true;
	if (contains_ci_token_seq(low, "pkill", "-9")) return true;
	if (contains_ci_token_seq(low, "killall", "-9")) return true;

	char clean[512];
	jarvis_clean_phrase(low, clean, sizeof(clean));
	char padded[520];
	snprintf(padded, sizeof(padded), " %s ", clean);
	if (strstr(padded, " mkfs ") || strstr(padded, " shutdown ") ||
	    strstr(padded, " reboot ") || strstr(padded, " poweroff ")) {
		return true;
	}
	return false;
}

static void append_short_str_list(const jjson_val_t *arr, char dst[][JARVIS_SHORT_LEN], size_t max_items, size_t *inout_count)
{
	if (!arr || arr->type != JJSON_ARRAY) return;
	for (size_t i = 0; i < arr->u.arr.count && *inout_count < max_items; i++) {
		const jjson_val_t *it = arr->u.arr.items[i];
		if (it && it->type == JJSON_STRING && it->u.str) {
			bool exists = false;
			for (size_t j = 0; j < *inout_count; j++) {
				if (strcmp(dst[j], it->u.str) == 0) { exists = true; break; }
			}
			if (!exists) {
				char *slot = dst[(*inout_count)++];
				snprintf(slot, sizeof(dst[0]), "%s", it->u.str);
			}
		}
	}
}

static void append_id_str_list(const jjson_val_t *arr, char dst[][JARVIS_ID_LEN], size_t max_items, size_t *inout_count)
{
	if (!arr || arr->type != JJSON_ARRAY) return;
	for (size_t i = 0; i < arr->u.arr.count && *inout_count < max_items; i++) {
		const jjson_val_t *it = arr->u.arr.items[i];
		if (it && it->type == JJSON_STRING && it->u.str) {
			bool exists = false;
			for (size_t j = 0; j < *inout_count; j++) {
				if (strcmp(dst[j], it->u.str) == 0) { exists = true; break; }
			}
			if (!exists) {
				char *slot = dst[(*inout_count)++];
				snprintf(slot, sizeof(dst[0]), "%s", it->u.str);
			}
		}
	}
}

static void load_short_str_list(const jjson_val_t *arr, char dst[][JARVIS_SHORT_LEN], size_t max_items, size_t *out_count)
{
	*out_count = 0;
	append_short_str_list(arr, dst, max_items, out_count);
}

static void load_id_str_list(const jjson_val_t *arr, char dst[][JARVIS_ID_LEN], size_t max_items, size_t *out_count)
{
	*out_count = 0;
	append_id_str_list(arr, dst, max_items, out_count);
}

static void load_kv_str_obj(const jjson_val_t *obj, jarvis_kv_str_t *dst, size_t max_items, size_t *out_count)
{
	*out_count = 0;
	if (!obj || obj->type != JJSON_OBJECT) return;
	for (size_t i = 0; i < obj->u.obj.count && *out_count < max_items; i++) {
		const char *k = obj->u.obj.pairs[i].key;
		const jjson_val_t *v = obj->u.obj.pairs[i].val;
		if (k && v && v->type == JJSON_STRING && v->u.str) {
			jarvis_kv_str_t *kv = &dst[(*out_count)++];
			snprintf(kv->key, sizeof(kv->key), "%s", k);
			snprintf(kv->val, sizeof(kv->val), "%s", v->u.str);
		}
	}
}

static void upsert_kv_str_obj(const jjson_val_t *obj, jarvis_kv_str_t *dst, size_t max_items, size_t *inout_count)
{
	if (!obj || obj->type != JJSON_OBJECT) return;
	for (size_t i = 0; i < obj->u.obj.count; i++) {
		const char *k = obj->u.obj.pairs[i].key;
		const jjson_val_t *v = obj->u.obj.pairs[i].val;
		if (!k || !v || v->type != JJSON_STRING || !v->u.str) continue;
		bool updated = false;
		for (size_t j = 0; j < *inout_count; j++) {
			if (strcmp(dst[j].key, k) == 0) {
				snprintf(dst[j].val, sizeof(dst[j].val), "%s", v->u.str);
				updated = true;
				break;
			}
		}
		if (!updated && *inout_count < max_items) {
			jarvis_kv_str_t *kv = &dst[(*inout_count)++];
			snprintf(kv->key, sizeof(kv->key), "%s", k);
			snprintf(kv->val, sizeof(kv->val), "%s", v->u.str);
		}
	}
}

static const jjson_val_t *find_locale_obj(const jjson_val_t *locales, const char *lang)
{
	if (!locales || locales->type != JJSON_OBJECT || !lang || !*lang) return NULL;

	char norm[JARVIS_SHORT_LEN];
	size_t nw = 0;
	for (size_t i = 0; lang[i] && nw + 1 < sizeof(norm); i++) {
		char c = lang[i];
		if (c == '.' || c == '@') break;
		if (c == '_') c = '-';
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		norm[nw++] = c;
	}
	norm[nw] = '\0';
	if (!norm[0]) return NULL;

	const jjson_val_t *v = jjson_obj_get(locales, norm);
	if (v && v->type == JJSON_OBJECT) return v;

	for (size_t i = 0; i < locales->u.obj.count; i++) {
		const char *k = locales->u.obj.pairs[i].key;
		const jjson_val_t *kv = locales->u.obj.pairs[i].val;
		if (k && kv && kv->type == JJSON_OBJECT && strcasecmp(k, norm) == 0) {
			return kv;
		}
	}

	char *dash = strchr(norm, '-');
	if (dash) {
		*dash = '\0';
		v = jjson_obj_get(locales, norm);
		if (v && v->type == JJSON_OBJECT) return v;
		for (size_t i = 0; i < locales->u.obj.count; i++) {
			const char *k = locales->u.obj.pairs[i].key;
			const jjson_val_t *kv = locales->u.obj.pairs[i].val;
			if (k && kv && kv->type == JJSON_OBJECT && strcasecmp(k, norm) == 0) {
				return kv;
			}
		}
	}
	return NULL;
}

static void apply_locale_override(jarvis_ontology_t *ont, const jjson_val_t *locales, const char *target_lang)
{
	const jjson_val_t *loc = find_locale_obj(locales, target_lang);
	if (!loc) return;

	const char *s = jjson_obj_get_str(loc, "lang", NULL);
	if (s && *s) snprintf(ont->lang, sizeof(ont->lang), "%s", s);

	s = jjson_obj_get_str(loc, "lang_name", NULL);
	if (s && *s) snprintf(ont->lang_name, sizeof(ont->lang_name), "%s", s);

	s = jjson_obj_get_str(loc, "wake_word", NULL);
	if (s && *s) snprintf(ont->wake_word, sizeof(ont->wake_word), "%s", s);

	s = jjson_obj_get_str(loc, "whisper_wake_prompt", NULL);
	if (s && *s) snprintf(ont->whisper_wake_prompt, sizeof(ont->whisper_wake_prompt), "%s", s);

	s = jjson_obj_get_str(loc, "whisper_cmd_prompt", NULL);
	if (s && *s) snprintf(ont->whisper_cmd_prompt, sizeof(ont->whisper_cmd_prompt), "%s", s);

	upsert_kv_str_obj(jjson_obj_get(loc, "ui_strings"), ont->ui_strings, JARVIS_MAX_UI_STRINGS, &ont->ui_string_count);
	upsert_kv_str_obj(jjson_obj_get(loc, "reply_templates"), ont->reply_templates, JARVIS_MAX_REPLY_TEMPLATES, &ont->reply_template_count);

	append_short_str_list(jjson_obj_get(loc, "wake_variants"), ont->wake_variants, JARVIS_MAX_WAKE_VARIANTS, &ont->wake_variant_count);
	append_short_str_list(jjson_obj_get(loc, "polite_prefixes"), ont->polite_prefixes, JARVIS_MAX_POLITE_PREFIXES, &ont->polite_prefix_count);
	append_id_str_list(jjson_obj_get(loc, "tts_interrupt_phrases"), ont->tts_interrupts, JARVIS_MAX_TTS_INTERRUPTS, &ont->tts_interrupt_count);
	append_id_str_list(jjson_obj_get(loc, "op_abort_phrases"), ont->op_abort_phrases, JARVIS_MAX_OP_ABORTS, &ont->op_abort_count);
	append_id_str_list(jjson_obj_get(loc, "lockdown_phrases"), ont->lockdown_phrases, JARVIS_MAX_LOCKDOWN_PHRASES, &ont->lockdown_phrase_count);
	append_short_str_list(jjson_obj_get(loc, "screen_lock_keywords"), ont->screen_lock_keywords, JARVIS_MAX_SCREEN_LOCK_KW, &ont->screen_lock_kw_count);
	append_short_str_list(jjson_obj_get(loc, "tier1_triggers"), ont->tier1_triggers, JARVIS_MAX_TIER1_TRIGGERS, &ont->tier1_trigger_count);
	append_id_str_list(jjson_obj_get(loc, "hallucination_phrases"), ont->hallucinations, JARVIS_MAX_HALLUCINATIONS, &ont->hallucination_count);
	append_short_str_list(jjson_obj_get(loc, "stopwords"), ont->stopwords, JARVIS_MAX_STOPWORDS, &ont->stopword_count);
	append_short_str_list(jjson_obj_get(loc, "conjunctions"), ont->conjunctions, JARVIS_MAX_CONJUNCTIONS, &ont->conjunction_count);
}

jarvis_ontology_t *jarvis_ontology_create_from_json_for_lang(const char *json_str, const char *lang)
{
	if (!json_str || !*json_str) return NULL;
	jjson_val_t *root = jjson_parse(json_str);
	if (!root || root->type != JJSON_OBJECT) {
		jjson_free(root);
		return NULL;
	}

	jarvis_ontology_t *ont = (jarvis_ontology_t *)calloc(1, sizeof(jarvis_ontology_t));
	if (!ont) {
		jjson_free(root);
		return NULL;
	}

	/* 0. Language, wake word, Whisper prompts, UI strings & reply templates (strictly from JSON) */
	snprintf(ont->lang, sizeof(ont->lang), "%s", jjson_obj_get_str(root, "lang", ""));
	snprintf(ont->lang_name, sizeof(ont->lang_name), "%s", jjson_obj_get_str(root, "lang_name", ""));
	snprintf(ont->wake_word, sizeof(ont->wake_word), "%s", jjson_obj_get_str(root, "wake_word", ""));
	snprintf(ont->whisper_wake_prompt, sizeof(ont->whisper_wake_prompt), "%s", jjson_obj_get_str(root, "whisper_wake_prompt", ""));
	snprintf(ont->whisper_cmd_prompt, sizeof(ont->whisper_cmd_prompt), "%s", jjson_obj_get_str(root, "whisper_cmd_prompt", ""));

	load_kv_str_obj(jjson_obj_get(root, "ui_strings"), ont->ui_strings, JARVIS_MAX_UI_STRINGS, &ont->ui_string_count);
	load_kv_str_obj(jjson_obj_get(root, "reply_templates"), ont->reply_templates, JARVIS_MAX_REPLY_TEMPLATES, &ont->reply_template_count);

	load_short_str_list(jjson_obj_get(root, "wake_variants"), ont->wake_variants, JARVIS_MAX_WAKE_VARIANTS, &ont->wake_variant_count);
	load_short_str_list(jjson_obj_get(root, "polite_prefixes"), ont->polite_prefixes, JARVIS_MAX_POLITE_PREFIXES, &ont->polite_prefix_count);
	load_id_str_list(jjson_obj_get(root, "tts_interrupt_phrases"), ont->tts_interrupts, JARVIS_MAX_TTS_INTERRUPTS, &ont->tts_interrupt_count);
	load_id_str_list(jjson_obj_get(root, "op_abort_phrases"), ont->op_abort_phrases, JARVIS_MAX_OP_ABORTS, &ont->op_abort_count);
	load_id_str_list(jjson_obj_get(root, "lockdown_phrases"), ont->lockdown_phrases, JARVIS_MAX_LOCKDOWN_PHRASES, &ont->lockdown_phrase_count);
	load_short_str_list(jjson_obj_get(root, "screen_lock_keywords"), ont->screen_lock_keywords, JARVIS_MAX_SCREEN_LOCK_KW, &ont->screen_lock_kw_count);
	load_short_str_list(jjson_obj_get(root, "tier1_triggers"), ont->tier1_triggers, JARVIS_MAX_TIER1_TRIGGERS, &ont->tier1_trigger_count);
	load_id_str_list(jjson_obj_get(root, "hallucination_phrases"), ont->hallucinations, JARVIS_MAX_HALLUCINATIONS, &ont->hallucination_count);

	/* 1. Stopwords & conjunctions */
	load_short_str_list(jjson_obj_get(root, "stopwords"), ont->stopwords, JARVIS_MAX_STOPWORDS, &ont->stopword_count);
	load_short_str_list(jjson_obj_get(root, "conjunctions"), ont->conjunctions, JARVIS_MAX_CONJUNCTIONS, &ont->conjunction_count);

	/* 1b. Optional locale overlay (`"locales": { "en": {...}, "pt": {...} }`) */
	const jjson_val_t *locales = jjson_obj_get(root, "locales");
	if (locales && locales->type == JJSON_OBJECT) {
		char eff_lang[JARVIS_SHORT_LEN] = {0};
		if (lang && *lang) {
			snprintf(eff_lang, sizeof(eff_lang), "%s", lang);
		} else {
			jarvis_resolve_lang(ont, eff_lang, sizeof(eff_lang));
		}
		apply_locale_override(ont, locales, eff_lang);
	}

	/* 2. Normalize stopwords, conjunctions, polite prefixes, interrupts, lockdown & triggers */
	for (size_t i = 0; i < ont->stopword_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->stopwords[i], c, sizeof(c));
		snprintf(ont->stopwords[i], sizeof(ont->stopwords[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->conjunction_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->conjunctions[i], c, sizeof(c));
		snprintf(ont->conjunctions[i], sizeof(ont->conjunctions[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->polite_prefix_count; i++) {
		char c[JARVIS_SHORT_LEN];
		clean_phrase_preserve_wake_tpl(ont->polite_prefixes[i], c, sizeof(c));
		snprintf(ont->polite_prefixes[i], sizeof(ont->polite_prefixes[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->tts_interrupt_count; i++) {
		char c[JARVIS_ID_LEN];
		clean_phrase_preserve_wake_tpl(ont->tts_interrupts[i], c, sizeof(c));
		snprintf(ont->tts_interrupts[i], sizeof(ont->tts_interrupts[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->op_abort_count; i++) {
		char c[JARVIS_ID_LEN];
		clean_phrase_preserve_wake_tpl(ont->op_abort_phrases[i], c, sizeof(c));
		snprintf(ont->op_abort_phrases[i], sizeof(ont->op_abort_phrases[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->lockdown_phrase_count; i++) {
		char c[JARVIS_ID_LEN];
		clean_phrase_preserve_wake_tpl(ont->lockdown_phrases[i], c, sizeof(c));
		snprintf(ont->lockdown_phrases[i], sizeof(ont->lockdown_phrases[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->screen_lock_kw_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->screen_lock_keywords[i], c, sizeof(c));
		snprintf(ont->screen_lock_keywords[i], sizeof(ont->screen_lock_keywords[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->tier1_trigger_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->tier1_triggers[i], c, sizeof(c));
		if (c[0]) {
			const char *raw = ont->tier1_triggers[i];
			size_t rlen = strlen(raw);
			bool lead_sp = (rlen > 0 && isspace((unsigned char)raw[0]));
			bool trail_sp = (rlen > 0 && isspace((unsigned char)raw[rlen - 1]));
			char buf[JARVIS_SHORT_LEN + 4];
			snprintf(buf, sizeof(buf), "%s%s%s",
			         lead_sp ? " " : "", c, trail_sp ? " " : "");
			snprintf(ont->tier1_triggers[i], sizeof(ont->tier1_triggers[i]), "%.*s",
			         (int)(sizeof(ont->tier1_triggers[i]) - 1), buf);
		} else {
			ont->tier1_triggers[i][0] = '\0';
		}
	}
	for (size_t i = 0; i < ont->hallucination_count; i++) {
		char c[JARVIS_ID_LEN];
		jarvis_clean_phrase(ont->hallucinations[i], c, sizeof(c));
		snprintf(ont->hallucinations[i], sizeof(ont->hallucinations[i]), "%s", c);
	}

	jjson_free(root);
	g_active_ontology = ont;
	return ont;
}

jarvis_ontology_t *jarvis_ontology_create_from_json(const char *json_str)
{
	return jarvis_ontology_create_from_json_for_lang(json_str, NULL);
}

static jarvis_ontology_t *try_load_ontology_file_for_lang(const char *path, const char *lang)
{
#ifndef __wasm__
	if (!path || !*path) return NULL;
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0 || sz >= 2 * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	size_t rd = fread(buf, 1, (size_t)sz, fp);
	buf[rd] = '\0';
	fclose(fp);
	jarvis_ontology_t *ont = jarvis_ontology_create_from_json_for_lang(buf, lang);
	free(buf);
	return ont;
#else
	(void)path;
	(void)lang;
	return NULL;
#endif
}

jarvis_ontology_t *jarvis_ontology_create_for_lang(const char *json_path, const char *lang)
{
#ifndef __wasm__
	if (json_path && *json_path) {
		jarvis_ontology_t *ont = try_load_ontology_file_for_lang(json_path, lang);
		if (ont) return ont;
	}
	const char *env_p = getenv("JARVIS_ONTOLOGY_JSON");
	if (env_p && *env_p) {
		jarvis_ontology_t *ont = try_load_ontology_file_for_lang(env_p, lang);
		if (ont) return ont;
	}
	const char *home = getenv("HOME");
	if (home && *home) {
		char home_cfg[512];
		snprintf(home_cfg, sizeof(home_cfg), "%s/.config/jarvis/ontology.json", home);
		jarvis_ontology_t *ont = try_load_ontology_file_for_lang(home_cfg, lang);
		if (ont) return ont;

		snprintf(home_cfg, sizeof(home_cfg), "%s/.local/share/jarvis/ontology.json", home);
		ont = try_load_ontology_file_for_lang(home_cfg, lang);
		if (ont) return ont;
	}
	static const char *const fallback_paths[] = {
		"data/ontology.json",
		"../data/ontology.json",
		"../../data/ontology.json",
		"/usr/local/share/jarvis/ontology.json",
		"/usr/share/jarvis/ontology.json",
		NULL
	};
	for (size_t i = 0; fallback_paths[i]; i++) {
		jarvis_ontology_t *ont = try_load_ontology_file_for_lang(fallback_paths[i], lang);
		if (ont) return ont;
	}
#else
	(void)json_path;
#endif
	return jarvis_ontology_create_from_json_for_lang("{}", lang);
}

jarvis_ontology_t *jarvis_ontology_create(const char *json_path)
{
	return jarvis_ontology_create_for_lang(json_path, NULL);
}

const char *jarvis_ontology_get_wake_word(const jarvis_ontology_t *ont)
{
	if (!ont) ont = jarvis_ontology_get_active();
	return (ont && ont->wake_word[0]) ? ont->wake_word : "assistant";
}

const char *jarvis_ontology_get_lang(const jarvis_ontology_t *ont)
{
	if (!ont) ont = jarvis_ontology_get_active();
	return (ont && ont->lang[0]) ? ont->lang : "en";
}

const char *jarvis_ontology_get_lang_name(const jarvis_ontology_t *ont)
{
	if (!ont) ont = jarvis_ontology_get_active();
	return (ont && ont->lang_name[0]) ? ont->lang_name : "English";
}

const char *jarvis_ontology_get_wake_prompt(const jarvis_ontology_t *ont)
{
	if (!ont) ont = jarvis_ontology_get_active();
	return (ont && ont->whisper_wake_prompt[0]) ? ont->whisper_wake_prompt : "";
}

const char *jarvis_ontology_get_cmd_prompt(const jarvis_ontology_t *ont)
{
	if (!ont) ont = jarvis_ontology_get_active();
	return (ont && ont->whisper_cmd_prompt[0]) ? ont->whisper_cmd_prompt : "";
}

const char *jarvis_ontology_get_ui_string(const jarvis_ontology_t *ont, const char *key, const char *fallback)
{
	if (!ont) ont = jarvis_ontology_get_active();
	if (ont && key) {
		for (size_t i = 0; i < ont->ui_string_count; i++) {
			if (strcmp(ont->ui_strings[i].key, key) == 0) {
				return ont->ui_strings[i].val;
			}
		}
	}
	return fallback ? fallback : "";
}

void jarvis_ontology_format_ui(
	const jarvis_ontology_t *ont,
	const char              *key,
	const char              *custom_wake_word,
	const char              *fallback,
	char                    *out,
	size_t                   out_sz
)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!ont) ont = jarvis_ontology_get_active();
	const char *tpl = jarvis_ontology_get_ui_string(ont, key, fallback);
	const char *eff_wake = (custom_wake_word && *custom_wake_word)
		? custom_wake_word
		: jarvis_ontology_get_wake_word(ont);
	expand_wake_tpl_str(tpl, eff_wake, out, out_sz);
}

const char *jarvis_ontology_get_reply_template(const jarvis_ontology_t *ont, const char *key)
{
	if (!ont) ont = jarvis_ontology_get_active();
	if (ont && key) {
		for (size_t i = 0; i < ont->reply_template_count; i++) {
			if (strcmp(ont->reply_templates[i].key, key) == 0) {
				return ont->reply_templates[i].val;
			}
		}
	}
	return NULL;
}

void jarvis_ontology_destroy(jarvis_ontology_t *ont)
{
	if (g_active_ontology == ont) g_active_ontology = NULL;
	free(ont);
}

bool jarvis_ontology_is_stopword(const jarvis_ontology_t *ont, const char *word)
{
	if (!ont || !word) return false;
	for (size_t i = 0; i < ont->stopword_count; i++) {
		if (strcmp(ont->stopwords[i], word) == 0) return true;
	}
	return false;
}

static bool match_phrase_with_wake_tpl(
	const char *clean_clause,
	const char *tpl,
	const char *wake_clean
)
{
	if (!clean_clause || !tpl || !tpl[0]) return false;
	if (!strstr(tpl, "{wake}")) {
		return strcmp(tpl, clean_clause) == 0;
	}
	if (!wake_clean || !wake_clean[0]) return false;
	char expanded[JARVIS_ID_LEN];
	expand_wake_tpl_str(tpl, wake_clean, expanded, sizeof(expanded));
	return strcmp(expanded, clean_clause) == 0;
}

bool jarvis_ontology_is_tts_interrupt_ex(
	const jarvis_ontology_t *ont,
	const char              *clean_clause,
	const char              *custom_wake_word
)
{
	if (!ont || !clean_clause || !clean_clause[0]) return false;
	const char *eff_wake = (custom_wake_word && *custom_wake_word) ? custom_wake_word : ont->wake_word;
	char wake_clean[JARVIS_SHORT_LEN] = {0};
	if (eff_wake && *eff_wake) {
		jarvis_clean_phrase(eff_wake, wake_clean, sizeof(wake_clean));
	}
	for (size_t i = 0; i < ont->tts_interrupt_count; i++) {
		if (match_phrase_with_wake_tpl(clean_clause, ont->tts_interrupts[i], wake_clean)) {
			return true;
		}
	}
	for (size_t i = 0; i < ont->op_abort_count; i++) {
		if (match_phrase_with_wake_tpl(clean_clause, ont->op_abort_phrases[i], wake_clean)) {
			return true;
		}
	}
	return false;
}

bool jarvis_ontology_is_tts_interrupt(const jarvis_ontology_t *ont, const char *clean_clause)
{
	return jarvis_ontology_is_tts_interrupt_ex(ont, clean_clause, NULL);
}

bool jarvis_ontology_is_op_abort(
	const jarvis_ontology_t *ont,
	const char              *phrase,
	const char              *custom_wake_word
)
{
	if (!ont || !phrase || !*phrase) return false;
	char clean[JARVIS_ID_LEN];
	jarvis_clean_phrase(phrase, clean, sizeof(clean));
	if (!clean[0]) return false;

	const char *eff_wake = (custom_wake_word && *custom_wake_word) ? custom_wake_word : ont->wake_word;
	char wake_clean[JARVIS_SHORT_LEN] = {0};
	if (eff_wake && *eff_wake) {
		jarvis_clean_phrase(eff_wake, wake_clean, sizeof(wake_clean));
	}
	for (size_t i = 0; i < ont->op_abort_count; i++) {
		if (match_phrase_with_wake_tpl(clean, ont->op_abort_phrases[i], wake_clean)) {
			return true;
		}
	}
	return false;
}

bool jarvis_ontology_is_screen_lock_phrase(const jarvis_ontology_t *ont, const char *phrase)
{
	if (!ont || !phrase || !*phrase) return false;
	char clean[JARVIS_ID_LEN];
	jarvis_clean_phrase(phrase, clean, sizeof(clean));
	if (!clean[0]) return false;
	for (size_t i = 0; i < ont->screen_lock_kw_count; i++) {
		if (ont->screen_lock_keywords[i][0] && strstr(clean, ont->screen_lock_keywords[i]) != NULL) {
			return true;
		}
	}
	return false;
}

bool jarvis_ontology_matches_tier1_trigger(const jarvis_ontology_t *ont, const char *text)
{
	if (!ont || !text || !*text) return false;
	char clean[512];
	jarvis_clean_phrase(text, clean, sizeof(clean));
	if (!clean[0]) return false;
	char padded[520];
	snprintf(padded, sizeof(padded), " %s ", clean);
	for (size_t i = 0; i < ont->tier1_trigger_count; i++) {
		if (ont->tier1_triggers[i][0] && strstr(padded, ont->tier1_triggers[i]) != NULL) {
			return true;
		}
	}
	return false;
}

void jarvis_phonetic_encode(
	const char *clean_str,
	char       *out_phon,
	size_t      phon_sz,
	char       *out_skel,
	size_t      skel_sz
)
{
	if (out_phon && phon_sz > 0) out_phon[0] = '\0';
	if (out_skel && skel_sz > 0) out_skel[0] = '\0';
	if (!clean_str || !*clean_str) return;

	char phon[256];
	size_t pw = 0;

	for (size_t i = 0; clean_str[i] && pw + 2 < sizeof(phon); ) {
		unsigned char c = (unsigned char)clean_str[i];
		if (c == ' ' || c == '\t' || c == '-' || c == '_') {
			i++;
			continue;
		}
		if (c >= 0x80) {
			phon[pw++] = (char)c;
			i++;
			continue;
		}
		char c0 = (char)tolower(c);
		char c1 = clean_str[i + 1] ? (char)tolower((unsigned char)clean_str[i + 1]) : '\0';
		char c2 = (c1 && clean_str[i + 2]) ? (char)tolower((unsigned char)clean_str[i + 2]) : '\0';

		char emit = '\0';
		size_t step = 1;

		if (c0 == 'q' && c1 == 'u') {
			emit = 'k';
			step = 2;
		} else if (c0 == 'g' && c1 == 'u' && (c2 == 'e' || c2 == 'i' || c2 == 'y')) {
			emit = 'g';
			step = 2;
		} else if ((c0 == 'c' || c0 == 's') && c1 == 'h') {
			emit = 'x';
			step = 2;
		} else if (c0 == 'n' && c1 == 'h') {
			emit = 'n';
			step = 2;
		} else if (c0 == 'l' && c1 == 'h') {
			emit = 'l';
			step = 2;
		} else if (c0 == 'p' && c1 == 'h') {
			emit = 'f';
			step = 2;
		} else if (c0 == 'c' && c1 == 'k') {
			emit = 'k';
			step = 2;
		} else if (c0 == 's' && c1 == 's') {
			emit = 's';
			step = 2;
		} else if (c0 == 's' && c1 == 'c' && (c2 == 'e' || c2 == 'i' || c2 == 'y')) {
			emit = 's';
			step = 2;
		} else if (c0 == 'r' && c1 == 'r') {
			emit = 'r';
			step = 2;
		} else if (c0 == 'h') {
			emit = '\0';
			step = 1;
		} else if (c0 == 'c') {
			emit = (c1 == 'e' || c1 == 'i' || c1 == 'y') ? 's' : 'k';
		} else if (c0 == 'q') {
			emit = 'k';
		} else if (c0 == 'z') {
			emit = 's';
		} else if (c0 == 'x' || c0 == 'j') {
			emit = 'x';
		} else if (c0 == 'g') {
			emit = (c1 == 'e' || c1 == 'i' || c1 == 'y') ? 'x' : 'g';
		} else if (c0 == 'v' || c0 == 'b') {
			emit = 'v';
		} else if (c0 == 'o' || c0 == 'u') {
			emit = 'u';
		} else if (c0 == 'e' || c0 == 'i' || c0 == 'y') {
			emit = 'i';
		} else if (c0 == 'a') {
			emit = 'a';
		} else if (c0 == 'm' || c0 == 'n') {
			bool next_is_vowel = (c1 == 'a' || c1 == 'e' || c1 == 'i' || c1 == 'o' || c1 == 'u' || c1 == 'y' || c1 == 'h');
			emit = next_is_vowel ? c0 : 'm';
		} else {
			emit = c0;
		}

		if (emit != '\0') {
			if (pw == 0 || phon[pw - 1] != emit) {
				phon[pw++] = emit;
			}
		}
		i += step;
	}
	phon[pw] = '\0';

	if (out_phon && phon_sz > 0) {
		snprintf(out_phon, phon_sz, "%s", phon);
	}

	if (out_skel && skel_sz > 0) {
		size_t sw = 0;
		for (size_t i = 0; i < pw && sw + 1 < skel_sz; i++) {
			char pc = phon[i];
			if (pc == 'a' || pc == 'i' || pc == 'u') continue;
			out_skel[sw++] = pc;
		}
		out_skel[sw] = '\0';
	}
}

int jarvis_damerau_levenshtein(const char *a, const char *b)
{
	if (!a || !b) return 999;
	size_t la = strlen(a);
	size_t lb = strlen(b);
	if (la == 0) return (int)lb;
	if (lb == 0) return (int)la;
	if (la > 96) la = 96;
	if (lb > 96) lb = 96;

	int prev2[98], prev1[98], cur[98];
	for (size_t j = 0; j <= lb; j++) {
		prev1[j] = (int)j;
		prev2[j] = (int)j;
	}

	for (size_t i = 1; i <= la; i++) {
		cur[0] = (int)i;
		for (size_t j = 1; j <= lb; j++) {
			int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			int del  = prev1[j] + 1;
			int ins  = cur[j - 1] + 1;
			int sub  = prev1[j - 1] + cost;
			int best = del < ins ? del : ins;
			if (sub < best) best = sub;

			if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
				int tr = prev2[j - 2] + 1;
				if (tr < best) best = tr;
			}
			cur[j] = best;
		}
		memcpy(prev2, prev1, (lb + 1) * sizeof(int));
		memcpy(prev1, cur, (lb + 1) * sizeof(int));
	}
	return prev1[lb];
}

double jarvis_token_similarity(const char *a_clean, const char *b_clean)
{
	if (!a_clean || !b_clean || !*a_clean || !*b_clean) return 0.0;
	if (strcmp(a_clean, b_clean) == 0) return 1.0;

	/* Reject cross-script comparisons (e.g. Latin vs Cyrillic/CJK) */
	bool a_hi = ((unsigned char)a_clean[0] >= 0x80);
	bool b_hi = ((unsigned char)b_clean[0] >= 0x80);
	if (a_hi != b_hi) return 0.0;

	/* Never fuzzy-match differing digits */
	for (const char *p = a_clean; *p; p++) {
		if (isdigit((unsigned char)*p)) return 0.0;
	}
	for (const char *p = b_clean; *p; p++) {
		if (isdigit((unsigned char)*p)) return 0.0;
	}

	char a_ns[128], b_ns[128];
	size_t na = 0, nb = 0;
	for (size_t i = 0; a_clean[i] && na + 1 < sizeof(a_ns); i++) {
		if (a_clean[i] != ' ') a_ns[na++] = a_clean[i];
	}
	a_ns[na] = '\0';
	for (size_t i = 0; b_clean[i] && nb + 1 < sizeof(b_ns); i++) {
		if (b_clean[i] != ' ') b_ns[nb++] = b_clean[i];
	}
	b_ns[nb] = '\0';

	if (na == 0 || nb == 0) return 0.0;
	if (strcmp(a_ns, b_ns) == 0) return 0.98;

	/* Short words (<= 3 chars) require exact match */
	if (na < 4 || nb < 4) return 0.0;
	int len_diff = (int)na - (int)nb;
	if (len_diff < 0) len_diff = -len_diff;
	if (len_diff > 4) return 0.0;

	size_t max_o = na > nb ? na : nb;
	int dl_o = jarvis_damerau_levenshtein(a_ns, b_ns);
	double sim_o = 1.0 - ((double)dl_o / (double)max_o);

	if (a_hi || b_hi) {
		return sim_o > 0.0 ? sim_o : 0.0;
	}

	char a_phon[128], a_skel[128];
	char b_phon[128], b_skel[128];
	jarvis_phonetic_encode(a_ns, a_phon, sizeof(a_phon), a_skel, sizeof(a_skel));
	jarvis_phonetic_encode(b_ns, b_phon, sizeof(b_phon), b_skel, sizeof(b_skel));

	double sim_p = 0.0;
	size_t lpa = strlen(a_phon), lpb = strlen(b_phon);
	if (lpa >= 3 && lpb >= 3) {
		size_t max_p = lpa > lpb ? lpa : lpb;
		int dl_p = jarvis_damerau_levenshtein(a_phon, b_phon);
		sim_p = 1.0 - ((double)dl_p / (double)max_p);
	}

	double sim_s = 0.0;
	size_t lsa = strlen(a_skel), lsb = strlen(b_skel);
	if (lsa >= 4 && lsb >= 4 && len_diff <= 3 && a_skel[0] == b_skel[0]) {
		size_t max_s = lsa > lsb ? lsa : lsb;
		int dl_s = jarvis_damerau_levenshtein(a_skel, b_skel);
		sim_s = 1.0 - ((double)dl_s / (double)max_s);
	}

	double best = sim_o;
	if (0.97 * sim_p > best) best = 0.97 * sim_p;
	if (0.94 * sim_s > best) best = 0.94 * sim_s;
	return best > 0.0 ? best : 0.0;
}

