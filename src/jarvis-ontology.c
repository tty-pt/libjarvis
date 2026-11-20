#include "jarvis-internal.h"

#ifndef __wasm__
#include <dirent.h>
#include <sys/stat.h>
#endif

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
		/* Treat common multi-byte Unicode punctuation (C2 A1..BF, E2 80.., E3 80..) as spaces */
		if (c == 0xC2 && (p[1] == 0xA1 || p[1] == 0xBF || p[1] == 0xAB || p[1] == 0xBB)) {
			p++;
			if (!prev_space && w + 1 < out_sz) {
				out[w++] = ' ';
				prev_space = true;
			}
			continue;
		}
		if ((c == 0xE2 && p[1] == 0x80 && (p[2] & 0xC0) == 0x80) ||
		    (c == 0xE3 && p[1] == 0x80 && (p[2] >= 0x80 && p[2] <= 0x9F))) {
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

static void add_vocab_words_from_phrase(jarvis_ontology_t *ont, const char *clean_p)
{
	char buf[JARVIS_NAME_LEN];
	snprintf(buf, sizeof(buf), "%s", clean_p);
	char *save = NULL;
	for (char *tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		bool exists = false;
		for (size_t i = 0; i < ont->vocab_word_count; i++) {
			if (strcmp(ont->vocab_words[i], tok) == 0) {
				exists = true;
				break;
			}
		}
		if (!exists && ont->vocab_word_count < JARVIS_MAX_VOCAB_WORDS) {
			char *dst = ont->vocab_words[ont->vocab_word_count++];
			snprintf(dst, sizeof(ont->vocab_words[0]), "%s", tok);
		}
	}
}

static void add_verb_head(jarvis_ontology_t *ont, const char *clean_p)
{
	char head[JARVIS_SHORT_LEN];
	size_t i = 0;
	while (clean_p[i] && clean_p[i] != ' ' && i + 1 < sizeof(head)) {
		head[i] = clean_p[i];
		i++;
	}
	head[i] = '\0';
	if (!head[0]) return;

	for (size_t j = 0; j < ont->verb_head_count; j++) {
		if (strcmp(ont->verb_heads[j], head) == 0) return;
	}
	if (ont->verb_head_count < JARVIS_MAX_VERB_HEADS) {
		char *dst = ont->verb_heads[ont->verb_head_count++];
		snprintf(dst, sizeof(ont->verb_heads[0]), "%s", head);
	}
}

static void add_verb_phrase(jarvis_ontology_t *ont, const char *raw_phrase, const char *concept, double weight)
{
	char clean[JARVIS_ID_LEN];
	if (jarvis_clean_phrase(raw_phrase, clean, sizeof(clean)) == 0) return;

	add_verb_head(ont, clean);
	add_vocab_words_from_phrase(ont, clean);

	for (size_t i = 0; i < ont->verb_phrase_count; i++) {
		if (strcmp(ont->verb_phrases[i].phrase, clean) == 0) {
			for (size_t t = 0; t < ont->verb_phrases[i].tag_count; t++) {
				if (strcmp(ont->verb_phrases[i].tags[t].concept, concept) == 0) {
					if (weight > ont->verb_phrases[i].tags[t].weight) {
						ont->verb_phrases[i].tags[t].weight = weight;
					}
					return;
				}
			}
			if (ont->verb_phrases[i].tag_count < JARVIS_MAX_VERB_CONCEPTS) {
				size_t idx = ont->verb_phrases[i].tag_count++;
				snprintf(ont->verb_phrases[i].tags[idx].concept,
				         sizeof(ont->verb_phrases[i].tags[idx].concept), "%s", concept);
				ont->verb_phrases[i].tags[idx].weight = weight;
			}
			return;
		}
	}

	if (ont->verb_phrase_count < JARVIS_MAX_VERB_MAP) {
		jarvis_verb_entry_t *e = &ont->verb_phrases[ont->verb_phrase_count++];
		memset(e, 0, sizeof(*e));
		snprintf(e->phrase, sizeof(e->phrase), "%s", clean);
		snprintf(e->tags[0].concept, sizeof(e->tags[0].concept), "%s", concept);
		e->tags[0].weight = weight;
		e->tag_count = 1;
	}
}

static void add_entity_phrase(jarvis_ontology_t *ont, const char *raw_phrase, const char *etype, const char *eid)
{
	char clean[JARVIS_NAME_LEN];
	if (jarvis_clean_phrase(raw_phrase, clean, sizeof(clean)) == 0) return;

	add_vocab_words_from_phrase(ont, clean);

	for (size_t i = 0; i < ont->entity_phrase_count; i++) {
		if (strcmp(ont->entity_phrases[i].phrase, clean) == 0) {
			for (size_t t = 0; t < ont->entity_phrases[i].tag_count; t++) {
				if (strcmp(ont->entity_phrases[i].tags[t].type, etype) == 0 &&
				    strcmp(ont->entity_phrases[i].tags[t].id, eid) == 0) {
					return;
				}
			}
			if (ont->entity_phrases[i].tag_count < JARVIS_MAX_ENTITY_TAGS) {
				size_t idx = ont->entity_phrases[i].tag_count++;
				snprintf(ont->entity_phrases[i].tags[idx].type,
				         sizeof(ont->entity_phrases[i].tags[idx].type), "%s", etype);
				snprintf(ont->entity_phrases[i].tags[idx].id,
				         sizeof(ont->entity_phrases[i].tags[idx].id), "%s", eid);
			}
			return;
		}
	}

	if (ont->entity_phrase_count < JARVIS_MAX_ENTITY_MAP) {
		jarvis_entity_entry_t *e = &ont->entity_phrases[ont->entity_phrase_count++];
		memset(e, 0, sizeof(*e));
		snprintf(e->phrase, sizeof(e->phrase), "%s", clean);
		snprintf(e->tags[0].type, sizeof(e->tags[0].type), "%s", etype);
		snprintf(e->tags[0].id, sizeof(e->tags[0].id), "%s", eid);
		e->tag_count = 1;
	}
}

static void append_locative_list(const jjson_val_t *arr, char dst[][JARVIS_WORD_LEN], size_t *inout_count)
{
	if (!arr || arr->type != JJSON_ARRAY) return;
	for (size_t i = 0; i < arr->u.arr.count && *inout_count < JARVIS_MAX_LOCATIVES; i++) {
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

/*
 * Abstract structural capability graph (contains zero natural-language words;
 * only abstract verb concept identifiers and entity type identifiers).
 * Used only when a custom test JSON omits the "capabilities" array.
 */
static const char *const JARVIS_ABSTRACT_CAPABILITIES_JSON =
	"["
	"{\"id\":\"fs.list\",\"verbs\":[\"PRESENT\",\"OPEN_LAUNCH\"],\"required_entities\":[\"fs.collection\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"ui.container_kind\",\"git.object\"],\"forbid_residual\":true},"
	"{\"id\":\"fs.metric\",\"verbs\":[\"PRESENT\",\"OPEN_LAUNCH\"],\"required_entities\":[\"fs.metric\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"ui.container_kind\",\"git.object\"],\"forbid_residual\":true},"
	"{\"id\":\"fs.find\",\"verbs\":[\"SEARCH_FS\",\"PRESENT\"],\"required_entities\":[\"fs.file_type\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"ui.container_kind\",\"git.object\"],\"forbid_residual\":true},"
	"{\"id\":\"fs.navigate\",\"verbs\":[\"NAVIGATE_FS\",\"FOCUS_NAV\"],\"required_entities\":[\"fs.location\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"ui.container_kind\",\"git.object\",\"fs.collection\",\"fs.file_type\"],\"forbid_residual\":true},"
	"{\"id\":\"git.inspect\",\"verbs\":[\"PRESENT\",\"OPEN_LAUNCH\",\"RUN_SHELL\"],\"required_entities\":[\"git.object\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"ui.container_kind\"],\"forbid_residual\":true},"
	"{\"id\":\"shell.run\",\"verbs\":[\"RUN_SHELL\",\"OPEN_LAUNCH\"],\"required_entities\":[\"shell.program\"],\"forbid_entities\":[\"media.object\"],\"forbid_residual\":false},"
	"{\"id\":\"window.new\",\"verbs\":[\"OPEN_LAUNCH\"],\"required_entities\":[\"ui.container_kind\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"web.shortcut\"],\"forbid_residual\":true},"
	"{\"id\":\"edit.clear\",\"verbs\":[\"EDIT_CLEAR\",\"INPUT_TYPE\"],\"required_entities\":[\"edit.unit\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"ui.container_kind\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\"],\"forbid_residual\":true},"
	"{\"id\":\"media.play\",\"verbs\":[\"PLAY_STREAM\",\"PLAY_RESUME\"],\"forbid_entities\":[\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"git.object\",\"shell.program\",\"edit.unit\",\"ui.container_kind\",\"web.shortcut\"],\"required_any\":[\"media.object\",\"residual_with_play_verb\"],\"forbid_residual\":false},"
	"{\"id\":\"media.control\",\"verbs\":[\"PAUSE_STOP\",\"PLAY_STREAM\",\"MEDIA_ADJUST\"],\"required_entities\":[\"media.object\"],\"forbid_residual\":true},"
	"{\"id\":\"record.control\",\"verbs\":[\"OPEN_LAUNCH\",\"PAUSE_STOP\",\"RECORD_START\",\"RECORD_STOP\"],\"required_entities\":[\"record.object\"],\"forbid_residual\":true},"
	"{\"id\":\"window.focus\",\"verbs\":[\"FOCUS_NAV\",\"OPEN_LAUNCH\",\"FOCUS_ACTIVATE\"],\"required_entities\":[\"ui.window\"],\"forbid_entities\":[\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"git.object\",\"ui.workspace\"],\"forbid_residual\":true},"
	"{\"id\":\"window.focus_relative\",\"verbs\":[\"FOCUS_NAV\",\"FOCUS_ACTIVATE\"],\"required_entities\":[\"ui.relative_window\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"ui.workspace\"],\"forbid_residual\":true},"
	"{\"id\":\"web.open\",\"verbs\":[\"OPEN_LAUNCH\",\"FOCUS_NAV\",\"PRESENT\"],\"required_entities\":[\"web.shortcut\"],\"forbid_entities\":[\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"git.object\",\"shell.program\"],\"forbid_residual\":true},"
	"{\"id\":\"web.search\",\"verbs\":[\"SEARCH_WEB\",\"SEARCH_FIND\"],\"forbid_entities\":[\"media.object\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"ui.container_kind\",\"ui.relative_window\",\"edit.unit\"],\"forbid_residual\":false},"
	"{\"id\":\"workspace.switch\",\"verbs\":[\"WORKSPACE_SWITCH\",\"FOCUS_NAV\",\"FOCUS_ACTIVATE\"],\"required_entities\":[\"ui.workspace\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"ui.container_kind\",\"ui.relative_window\",\"edit.unit\"],\"forbid_residual\":true},"
	"{\"id\":\"workspace.move_window\",\"verbs\":[\"WORKSPACE_MOVE\",\"MOVE_SEND\"],\"required_entities\":[\"ui.workspace\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\"],\"forbid_residual\":true},"
	"{\"id\":\"window.control\",\"verbs\":[\"WINDOW_CONTROL\",\"CLOSE_QUIT\",\"TOGGLE_SET\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"edit.unit\",\"ui.workspace\"],\"forbid_residual\":true},"
	"{\"id\":\"ui.scroll\",\"verbs\":[\"SCROLL_NAV\",\"SCROLL_PAN\"],\"required_entities\":[\"ui.scroll_direction\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\"],\"forbid_residual\":true},"
	"{\"id\":\"input.dictate\",\"verbs\":[\"INPUT_TYPE\",\"TYPE_WRITE\",\"TYPE_SUBMIT\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"ui.container_kind\",\"ui.relative_window\",\"record.object\"],\"forbid_residual\":false},"
	"{\"id\":\"app.launch\",\"verbs\":[\"OPEN_LAUNCH\",\"FOCUS_NAV\"],\"required_entities\":[\"gui.application\"],\"forbid_entities\":[\"media.object\",\"shell.program\",\"git.object\",\"fs.collection\",\"fs.metric\",\"fs.file_type\",\"fs.location\",\"ui.container_kind\",\"ui.relative_window\",\"record.object\",\"edit.unit\",\"web.shortcut\"],\"forbid_residual\":true}"
	"]";

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

jarvis_ontology_t *jarvis_ontology_create_from_json(const char *json_str)
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
	load_short_str_list(jjson_obj_get(root, "dictation_verbs"), ont->dictation_verbs, JARVIS_MAX_DICTATION_VERBS, &ont->dictation_verb_count);
	load_short_str_list(jjson_obj_get(root, "deictic_pronouns"), ont->deictic_pronouns, JARVIS_MAX_DEICTIC_PRONOUNS, &ont->deictic_pronoun_count);
	load_id_str_list(jjson_obj_get(root, "tts_interrupt_phrases"), ont->tts_interrupts, JARVIS_MAX_TTS_INTERRUPTS, &ont->tts_interrupt_count);
	load_id_str_list(jjson_obj_get(root, "lockdown_phrases"), ont->lockdown_phrases, JARVIS_MAX_LOCKDOWN_PHRASES, &ont->lockdown_phrase_count);
	load_id_str_list(jjson_obj_get(root, "hallucination_phrases"), ont->hallucinations, JARVIS_MAX_HALLUCINATIONS, &ont->hallucination_count);

	/* 1. Stopwords */
	load_short_str_list(jjson_obj_get(root, "stopwords"), ont->stopwords, JARVIS_MAX_STOPWORDS, &ont->stopword_count);

	/* 2. Clause separators & conjunctions */
	load_short_str_list(jjson_obj_get(root, "clause_separators"), ont->clause_seps, JARVIS_MAX_CLAUSE_SEPS, &ont->clause_sep_count);
	load_short_str_list(jjson_obj_get(root, "conjunctions"), ont->conjunctions, JARVIS_MAX_CONJUNCTIONS, &ont->conjunction_count);

	/* 2b. Argument prepositions, interrogatives, agent triggers, and live-index templates */
	load_short_str_list(jjson_obj_get(root, "arg_preps"), ont->arg_preps, JARVIS_MAX_ARG_PREPS, &ont->arg_prep_count);
	append_short_str_list(jjson_obj_get(root, "arg_prepositions"), ont->arg_preps, JARVIS_MAX_ARG_PREPS, &ont->arg_prep_count);
	load_short_str_list(jjson_obj_get(root, "interrogatives"), ont->interrogatives, JARVIS_MAX_INTERROGATIVES, &ont->interrogative_count);
	load_id_str_list(jjson_obj_get(root, "agent_triggers"), ont->agent_triggers, JARVIS_MAX_AGENT_TRIGGERS, &ont->agent_trigger_count);
	load_short_str_list(jjson_obj_get(root, "fs_templates"), ont->fs_templates, JARVIS_MAX_TEMPLATES, &ont->fs_template_count);
	load_short_str_list(jjson_obj_get(root, "window_templates"), ont->window_templates, JARVIS_MAX_TEMPLATES, &ont->window_template_count);
	load_short_str_list(jjson_obj_get(root, "tab_templates"), ont->tab_templates, JARVIS_MAX_TEMPLATES, &ont->tab_template_count);

	/* 2c. ASR corrections */
	{
		const jjson_val_t *asr_arr = jjson_obj_get(root, "asr_corrections");
		if (asr_arr && asr_arr->type == JJSON_ARRAY) {
			for (size_t i = 0; i < asr_arr->u.arr.count && ont->asr_fix_count < JARVIS_MAX_ASR_FIXES; i++) {
				const jjson_val_t *it = asr_arr->u.arr.items[i];
				const char *from_s = jjson_obj_get_str(it, "from", NULL);
				const char *to_s   = jjson_obj_get_str(it, "to", NULL);
				if (from_s && to_s) {
					jarvis_asr_fix_t *fx = &ont->asr_fixes[ont->asr_fix_count++];
					jarvis_clean_phrase(from_s, fx->from, sizeof(fx->from));
					snprintf(fx->to, sizeof(fx->to), "%s", to_s);
				}
			}
		}
	}

	/* 2d. Anaphora resolution rules */
	{
		const jjson_val_t *ana = jjson_obj_get(root, "anaphora");
		if (ana && ana->type == JJSON_OBJECT) {
			load_id_str_list(jjson_obj_get(ana, "pause_phrases"), ont->anaphora.pause_phrases, JARVIS_MAX_ANAPHORA_PHRASES, &ont->anaphora.pause_phrase_count);
			for (size_t i = 0; i < ont->anaphora.pause_phrase_count; i++) {
				char c[JARVIS_ID_LEN];
				jarvis_clean_phrase(ont->anaphora.pause_phrases[i], c, sizeof(c));
				snprintf(ont->anaphora.pause_phrases[i], sizeof(ont->anaphora.pause_phrases[i]), "%s", c);
			}
			snprintf(ont->anaphora.pause_target, sizeof(ont->anaphora.pause_target), "%s", jjson_obj_get_str(ana, "pause_target", ""));
			load_id_str_list(jjson_obj_get(ana, "resume_phrases"), ont->anaphora.resume_phrases, JARVIS_MAX_ANAPHORA_PHRASES, &ont->anaphora.resume_phrase_count);
			for (size_t i = 0; i < ont->anaphora.resume_phrase_count; i++) {
				char c[JARVIS_ID_LEN];
				jarvis_clean_phrase(ont->anaphora.resume_phrases[i], c, sizeof(c));
				snprintf(ont->anaphora.resume_phrases[i], sizeof(ont->anaphora.resume_phrases[i]), "%s", c);
			}
			snprintf(ont->anaphora.resume_prefix, sizeof(ont->anaphora.resume_prefix), "%s", jjson_obj_get_str(ana, "resume_prefix", ""));
			snprintf(ont->anaphora.resume_default, sizeof(ont->anaphora.resume_default), "%s", jjson_obj_get_str(ana, "resume_default", ""));
			load_short_str_list(jjson_obj_get(ana, "spatial_pronouns"), ont->anaphora.spatial_pronouns, JARVIS_MAX_ANAPHORA_PHRASES, &ont->anaphora.spatial_pronoun_count);
			for (size_t i = 0; i < ont->anaphora.spatial_pronoun_count; i++) {
				char c[JARVIS_SHORT_LEN];
				jarvis_clean_phrase(ont->anaphora.spatial_pronouns[i], c, sizeof(c));
				snprintf(ont->anaphora.spatial_pronouns[i], sizeof(ont->anaphora.spatial_pronouns[i]), "%s", c);
			}
			snprintf(ont->anaphora.spatial_prep, sizeof(ont->anaphora.spatial_prep), "%s", jjson_obj_get_str(ana, "spatial_prep", ""));
		}
	}

	/* 2e. Declarative Tier-0 idioms */
	{
		const jjson_val_t *id_arr = jjson_obj_get(root, "idioms");
		if (id_arr && id_arr->type == JJSON_ARRAY) {
			for (size_t i = 0; i < id_arr->u.arr.count && ont->idiom_count < JARVIS_MAX_IDIOMS; i++) {
				const jjson_val_t *it = id_arr->u.arr.items[i];
				if (!it || it->type != JJSON_OBJECT) continue;
				jarvis_idiom_t *dst = &ont->idioms[ont->idiom_count++];
				memset(dst, 0, sizeof(*dst));
				load_id_str_list(jjson_obj_get(it, "phrases"), dst->phrases, JARVIS_MAX_IDIOM_PHRASES, &dst->phrase_count);
				for (size_t k = 0; k < dst->phrase_count; k++) {
					char c[JARVIS_ID_LEN];
					jarvis_clean_phrase(dst->phrases[k], c, sizeof(c));
					snprintf(dst->phrases[k], sizeof(dst->phrases[k]), "%s", c);
					add_vocab_words_from_phrase(ont, c);
				}
				load_short_str_list(jjson_obj_get(it, "require_any"), dst->require_any, JARVIS_MAX_IDIOM_REQ_ANY, &dst->require_any_count);
				for (size_t k = 0; k < dst->require_any_count; k++) {
					char c[JARVIS_SHORT_LEN];
					jarvis_clean_phrase(dst->require_any[k], c, sizeof(c));
					snprintf(dst->require_any[k], sizeof(dst->require_any[k]), "%s", c);
				}
				load_short_str_list(jjson_obj_get(it, "forbid_any"), dst->forbid_any, JARVIS_MAX_IDIOM_REQ_ANY, &dst->forbid_any_count);
				for (size_t k = 0; k < dst->forbid_any_count; k++) {
					char c[JARVIS_SHORT_LEN];
					jarvis_clean_phrase(dst->forbid_any[k], c, sizeof(c));
					snprintf(dst->forbid_any[k], sizeof(dst->forbid_any[k]), "%s", c);
				}
				const char *cap_str = jjson_obj_get_str(it, "cap", NULL);
				if (!cap_str) cap_str = jjson_obj_get_str(it, "intent", "");
				snprintf(dst->cap, sizeof(dst->cap), "%s", cap_str);
				snprintf(dst->action, sizeof(dst->action), "%s", jjson_obj_get_str(it, "action", ""));
				snprintf(dst->text, sizeof(dst->text), "%s", jjson_obj_get_str(it, "text", ""));
				snprintf(dst->kind, sizeof(dst->kind), "%s", jjson_obj_get_str(it, "kind", ""));
				snprintf(dst->relation, sizeof(dst->relation), "%s", jjson_obj_get_str(it, "relation", ""));
				snprintf(dst->surface, sizeof(dst->surface), "%s", jjson_obj_get_str(it, "surface", "any"));
				dst->score = jjson_obj_get_num(it, "score", 0.96);
			}
		}
	}

	/* 2f. Declarative ReAct & Contextual Intent Rules */
	{
		const jjson_val_t *rr_arr = jjson_obj_get(root, "react_rules");
		if (rr_arr && rr_arr->type == JJSON_ARRAY) {
			for (size_t i = 0; i < rr_arr->u.arr.count && ont->react_rule_count < JARVIS_MAX_REACT_RULES; i++) {
				const jjson_val_t *it = rr_arr->u.arr.items[i];
				if (!it || it->type != JJSON_OBJECT) continue;
				jarvis_react_rule_t *dst = &ont->react_rules[ont->react_rule_count++];
				memset(dst, 0, sizeof(*dst));
				snprintf(dst->kind, sizeof(dst->kind), "%s", jjson_obj_get_str(it, "kind", ""));
				dst->turn_filter = jjson_obj_get_int(it, "turn", -1);
				snprintf(dst->obs_op, sizeof(dst->obs_op), "%s", jjson_obj_get_str(it, "obs_op", ""));
				load_id_str_list(jjson_obj_get(it, "phrases"), dst->phrases, JARVIS_MAX_REACT_PHRASES, &dst->phrase_count);
				for (size_t k = 0; k < dst->phrase_count; k++) {
					char c[JARVIS_ID_LEN];
					jarvis_clean_phrase(dst->phrases[k], c, sizeof(c));
					snprintf(dst->phrases[k], sizeof(dst->phrases[k]), "%s", c);
				}
				load_id_str_list(jjson_obj_get(it, "require_any"), dst->require_any, JARVIS_MAX_REACT_REQ_ANY, &dst->require_any_count);
				for (size_t k = 0; k < dst->require_any_count; k++) {
					char c[JARVIS_ID_LEN];
					jarvis_clean_phrase(dst->require_any[k], c, sizeof(c));
					snprintf(dst->require_any[k], sizeof(dst->require_any[k]), "%s", c);
				}
				load_id_str_list(jjson_obj_get(it, "forbid_any"), dst->forbid_any, JARVIS_MAX_REACT_REQ_ANY, &dst->forbid_any_count);
				for (size_t k = 0; k < dst->forbid_any_count; k++) {
					char c[JARVIS_ID_LEN];
					jarvis_clean_phrase(dst->forbid_any[k], c, sizeof(c));
					snprintf(dst->forbid_any[k], sizeof(dst->forbid_any[k]), "%s", c);
				}
				load_short_str_list(jjson_obj_get(it, "line_triggers"), dst->line_triggers, JARVIS_MAX_REACT_REQ_ANY, &dst->line_trigger_count);
				for (size_t k = 0; k < dst->line_trigger_count; k++) {
					char c[JARVIS_SHORT_LEN];
					jarvis_clean_phrase(dst->line_triggers[k], c, sizeof(c));
					snprintf(dst->line_triggers[k], sizeof(dst->line_triggers[k]), "%s", c);
				}
				snprintf(dst->op, sizeof(dst->op), "%s", jjson_obj_get_str(it, "op", ""));
				snprintf(dst->command, sizeof(dst->command), "%s", jjson_obj_get_str(it, "command", ""));
				snprintf(dst->arg, sizeof(dst->arg), "%s", jjson_obj_get_str(it, "arg", ""));
			}
		}
	}

	/* 3. Verbs (supports both {"CONCEPT": {"primary":[...],"light":[...]}} and {"CONCEPT": [...]}) */
	{
		const jjson_val_t *verbs = jjson_obj_get(root, "verbs");
		if (verbs && verbs->type == JJSON_OBJECT) {
			for (size_t i = 0; i < verbs->u.obj.count; i++) {
				const char *concept = verbs->u.obj.pairs[i].key;
				const jjson_val_t *grp = verbs->u.obj.pairs[i].val;
				if (!grp) continue;
				if (grp->type == JJSON_ARRAY) {
					for (size_t j = 0; j < grp->u.arr.count; j++) {
						if (grp->u.arr.items[j]->type == JJSON_STRING) {
							add_verb_phrase(ont, grp->u.arr.items[j]->u.str, concept, 1.0);
						}
					}
				} else if (grp->type == JJSON_OBJECT) {
					const jjson_val_t *prim = jjson_obj_get(grp, "primary");
					if (prim && prim->type == JJSON_ARRAY) {
						for (size_t j = 0; j < prim->u.arr.count; j++) {
							if (prim->u.arr.items[j]->type == JJSON_STRING) {
								add_verb_phrase(ont, prim->u.arr.items[j]->u.str, concept, 1.0);
							}
						}
					}
					const jjson_val_t *light = jjson_obj_get(grp, "light");
					if (light && light->type == JJSON_ARRAY) {
						for (size_t j = 0; j < light->u.arr.count; j++) {
							if (light->u.arr.items[j]->type == JJSON_STRING) {
								add_verb_phrase(ont, light->u.arr.items[j]->u.str, concept, 0.82);
							}
						}
					}
				}
			}
		}
	}

	/* 4. Entities (supports both {"type": [{"id":"...","phrases":[...]}]} and {"type": {"id":[...]}}) */
	{
		const jjson_val_t *ents_src = jjson_obj_get(root, "entities");
		if (ents_src && ents_src->type == JJSON_OBJECT) {
			for (size_t i = 0; i < ents_src->u.obj.count; i++) {
				const char *etype = ents_src->u.obj.pairs[i].key;
				const jjson_val_t *items = ents_src->u.obj.pairs[i].val;
				if (!items) continue;
				if (items->type == JJSON_ARRAY) {
					for (size_t j = 0; j < items->u.arr.count; j++) {
						const jjson_val_t *item = items->u.arr.items[j];
						const char *eid = jjson_obj_get_str(item, "id", "");
						const jjson_val_t *phrases = jjson_obj_get(item, "phrases");
						if (phrases && phrases->type == JJSON_ARRAY) {
							for (size_t k = 0; k < phrases->u.arr.count; k++) {
								if (phrases->u.arr.items[k]->type == JJSON_STRING) {
									add_entity_phrase(ont, phrases->u.arr.items[k]->u.str, etype, eid);
								}
							}
						}
					}
				} else if (items->type == JJSON_OBJECT) {
					for (size_t j = 0; j < items->u.obj.count; j++) {
						const char *eid = items->u.obj.pairs[j].key;
						const jjson_val_t *phrases = items->u.obj.pairs[j].val;
						if (phrases && phrases->type == JJSON_ARRAY) {
							for (size_t k = 0; k < phrases->u.arr.count; k++) {
								if (phrases->u.arr.items[k]->type == JJSON_STRING) {
									add_entity_phrase(ont, phrases->u.arr.items[k]->u.str, etype, eid);
								}
							}
						}
					}
				}
			}
		}
	}

	/* 5. Self locatives & modifiers */
	const jjson_val_t *locs = jjson_obj_get(root, "self_locatives");
	if (locs && locs->type == JJSON_OBJECT) {
		append_locative_list(jjson_obj_get(locs, "submit"), ont->loc_submit, &ont->loc_submit_count);
		append_locative_list(jjson_obj_get(locs, "other"), ont->loc_other, &ont->loc_other_count);
		append_locative_list(jjson_obj_get(locs, "terminal"), ont->loc_terminal, &ont->loc_terminal_count);
		append_locative_list(jjson_obj_get(locs, "any"), ont->loc_any, &ont->loc_any_count);
		append_locative_list(jjson_obj_get(locs, "new_terminal"), ont->loc_new_terminal, &ont->loc_new_terminal_count);
		append_locative_list(jjson_obj_get(locs, "background"), ont->loc_background, &ont->loc_background_count);
	}
	const jjson_val_t *mods = jjson_obj_get(root, "modifiers");
	if (mods && mods->type == JJSON_OBJECT) {
		append_locative_list(jjson_obj_get(mods, "target_other_window"), ont->loc_other, &ont->loc_other_count);
		append_locative_list(jjson_obj_get(mods, "target_new_terminal"), ont->loc_new_terminal, &ont->loc_new_terminal_count);
		append_locative_list(jjson_obj_get(mods, "background_mode"), ont->loc_background, &ont->loc_background_count);
	}

	/* Normalize stopwords, polite prefixes, dictation verbs, deictic pronouns, tts_interrupts, interrogatives */
	for (size_t i = 0; i < ont->stopword_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->stopwords[i], c, sizeof(c));
		snprintf(ont->stopwords[i], sizeof(ont->stopwords[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->polite_prefix_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->polite_prefixes[i], c, sizeof(c));
		snprintf(ont->polite_prefixes[i], sizeof(ont->polite_prefixes[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->dictation_verb_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->dictation_verbs[i], c, sizeof(c));
		snprintf(ont->dictation_verbs[i], sizeof(ont->dictation_verbs[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->deictic_pronoun_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->deictic_pronouns[i], c, sizeof(c));
		snprintf(ont->deictic_pronouns[i], sizeof(ont->deictic_pronouns[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->tts_interrupt_count; i++) {
		char c[JARVIS_ID_LEN];
		jarvis_clean_phrase(ont->tts_interrupts[i], c, sizeof(c));
		snprintf(ont->tts_interrupts[i], sizeof(ont->tts_interrupts[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->interrogative_count; i++) {
		char c[JARVIS_SHORT_LEN];
		jarvis_clean_phrase(ont->interrogatives[i], c, sizeof(c));
		snprintf(ont->interrogatives[i], sizeof(ont->interrogatives[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->lockdown_phrase_count; i++) {
		char c[JARVIS_ID_LEN];
		jarvis_clean_phrase(ont->lockdown_phrases[i], c, sizeof(c));
		snprintf(ont->lockdown_phrases[i], sizeof(ont->lockdown_phrases[i]), "%s", c);
	}
	for (size_t i = 0; i < ont->hallucination_count; i++) {
		char c[JARVIS_ID_LEN];
		jarvis_clean_phrase(ont->hallucinations[i], c, sizeof(c));
		snprintf(ont->hallucinations[i], sizeof(ont->hallucinations[i]), "%s", c);
	}

	/* 6. Capabilities (with declarative slot bindings & compilation rules) */
	jjson_val_t *abs_caps = NULL;
	const jjson_val_t *caps = jjson_obj_get(root, "capabilities");
	if (!caps) {
		abs_caps = jjson_parse(JARVIS_ABSTRACT_CAPABILITIES_JSON);
		caps = abs_caps;
	}
	if (caps && caps->type == JJSON_ARRAY) {
		for (size_t i = 0; i < caps->u.arr.count && ont->capability_count < JARVIS_MAX_CAPABILITIES; i++) {
			const jjson_val_t *c = caps->u.arr.items[i];
			if (!c || c->type != JJSON_OBJECT) continue;
			jarvis_capability_t *dst = &ont->capabilities[ont->capability_count++];
			memset(dst, 0, sizeof(*dst));
			snprintf(dst->id, sizeof(dst->id), "%s", jjson_obj_get_str(c, "id", ""));
			snprintf(dst->surface, sizeof(dst->surface), "%s", jjson_obj_get_str(c, "surface", "any"));
			snprintf(dst->open_arg, sizeof(dst->open_arg), "%s", jjson_obj_get_str(c, "open_arg", ""));
			dst->forbid_residual = jjson_obj_get_bool(c, "forbid_residual", false);

			const jjson_val_t *cv = jjson_obj_get(c, "verbs");
			if (cv && cv->type == JJSON_ARRAY) {
				for (size_t j = 0; j < cv->u.arr.count && dst->verb_count < JARVIS_MAX_CAP_VERBS; j++) {
					if (cv->u.arr.items[j]->type == JJSON_STRING) {
						char *slot = dst->verbs[dst->verb_count++];
						snprintf(slot, sizeof(dst->verbs[0]), "%s", cv->u.arr.items[j]->u.str);
					}
				}
			}
			const jjson_val_t *creq = jjson_obj_get(c, "required_entities");
			if (creq && creq->type == JJSON_ARRAY) {
				for (size_t j = 0; j < creq->u.arr.count && dst->req_ent_count < JARVIS_MAX_CAP_REQ_ENTS; j++) {
					if (creq->u.arr.items[j]->type == JJSON_STRING) {
						size_t idx = dst->req_ent_count++;
						snprintf(dst->required_entities[idx], sizeof(dst->required_entities[0]), "%s", creq->u.arr.items[j]->u.str);
						snprintf(dst->required_entity_vals[idx], sizeof(dst->required_entity_vals[0]), "*");
					}
				}
			}
			const jjson_val_t *creq_obj = jjson_obj_get(c, "required");
			if (creq_obj && creq_obj->type == JJSON_OBJECT) {
				for (size_t j = 0; j < creq_obj->u.obj.count && dst->req_ent_count < JARVIS_MAX_CAP_REQ_ENTS; j++) {
					const char *ek = creq_obj->u.obj.pairs[j].key;
					const jjson_val_t *ev = creq_obj->u.obj.pairs[j].val;
					if (ek && ev && ev->type == JJSON_STRING) {
						size_t idx = dst->req_ent_count++;
						snprintf(dst->required_entities[idx], sizeof(dst->required_entities[0]), "%s", ek);
						snprintf(dst->required_entity_vals[idx], sizeof(dst->required_entity_vals[0]), "%s", ev->u.str);
					}
				}
			}
			const jjson_val_t *cfbd = jjson_obj_get(c, "forbid_entities");
			if (cfbd && cfbd->type == JJSON_ARRAY) {
				for (size_t j = 0; j < cfbd->u.arr.count && dst->forbid_ent_count < JARVIS_MAX_CAP_FORBID_ENTS; j++) {
					if (cfbd->u.arr.items[j]->type == JJSON_STRING) {
						char *slot = dst->forbid_entities[dst->forbid_ent_count++];
						snprintf(slot, sizeof(dst->forbid_entities[0]), "%s", cfbd->u.arr.items[j]->u.str);
					}
				}
			}
			const jjson_val_t *cany = jjson_obj_get(c, "required_any");
			if (cany && cany->type == JJSON_ARRAY) {
				for (size_t j = 0; j < cany->u.arr.count && dst->req_any_count < JARVIS_MAX_CAP_REQ_ANY; j++) {
					if (cany->u.arr.items[j]->type == JJSON_STRING) {
						char *slot = dst->required_any[dst->req_any_count++];
						snprintf(slot, sizeof(dst->required_any[0]), "%s", cany->u.arr.items[j]->u.str);
					}
				}
			}
			load_id_str_list(jjson_obj_get(c, "phrases"), dst->phrases, JARVIS_MAX_CAP_PHRASES, &dst->phrase_count);
			for (size_t j = 0; j < dst->phrase_count; j++) {
				char clean_p[JARVIS_ID_LEN];
				jarvis_clean_phrase(dst->phrases[j], clean_p, sizeof(clean_p));
				snprintf(dst->phrases[j], sizeof(dst->phrases[j]), "%s", clean_p);
			}

			/* Parse declarative slot rules */
			const jjson_val_t *cslots = jjson_obj_get(c, "slots");
			if (cslots && cslots->type == JJSON_OBJECT) {
				for (size_t j = 0; j < cslots->u.obj.count && dst->slot_rule_count < JARVIS_MAX_CAP_SLOTS; j++) {
					const char *sname = cslots->u.obj.pairs[j].key;
					const jjson_val_t *sval = cslots->u.obj.pairs[j].val;
					if (!sname || !sval || sval->type != JJSON_OBJECT) continue;
					jarvis_cap_slot_rule_t *sr = &dst->slot_rules[dst->slot_rule_count++];
					memset(sr, 0, sizeof(*sr));
					snprintf(sr->slot_name, sizeof(sr->slot_name), "%s", sname);
					snprintf(sr->source, sizeof(sr->source), "%s", jjson_obj_get_str(sval, "source", "entity_id"));
					const char *def_v = jjson_obj_get_str(sval, "default", NULL);
					if (!def_v) def_v = jjson_obj_get_str(sval, "value", "");
					snprintf(sr->default_val, sizeof(sr->default_val), "%s", def_v);
					const char *single_t = jjson_obj_get_str(sval, "type", NULL);
					if (single_t && *single_t) {
						snprintf(sr->types[sr->type_count++], sizeof(sr->types[0]), "%s", single_t);
					}
					const jjson_val_t *tarr = jjson_obj_get(sval, "types");
					if (tarr && tarr->type == JJSON_ARRAY) {
						for (size_t k = 0; k < tarr->u.arr.count && sr->type_count < JARVIS_MAX_CAP_SLOT_TYPES; k++) {
							if (tarr->u.arr.items[k]->type == JJSON_STRING) {
								snprintf(sr->types[sr->type_count++], sizeof(sr->types[0]), "%s", tarr->u.arr.items[k]->u.str);
							}
						}
					}
				}
			}

			/* Parse declarative compilation rules */
			const jjson_val_t *ccomp = jjson_obj_get(c, "compile");
			if (ccomp && ccomp->type == JJSON_OBJECT) {
				snprintf(dst->compile.op, sizeof(dst->compile.op), "%s", jjson_obj_get_str(ccomp, "op", ""));
				snprintf(dst->compile.op_if_slot, sizeof(dst->compile.op_if_slot), "%s", jjson_obj_get_str(ccomp, "op_if_slot", ""));
				snprintf(dst->compile.op_if_equals, sizeof(dst->compile.op_if_equals), "%s", jjson_obj_get_str(ccomp, "op_if_equals", ""));
				snprintf(dst->compile.op_then, sizeof(dst->compile.op_then), "%s", jjson_obj_get_str(ccomp, "op_then", ""));
				snprintf(dst->compile.op_else, sizeof(dst->compile.op_else), "%s", jjson_obj_get_str(ccomp, "op_else", ""));
				for (size_t j = 0; j < ccomp->u.obj.count && dst->compile.field_count < JARVIS_MAX_CAP_COMPILE_FIELDS; j++) {
					const char *fk = ccomp->u.obj.pairs[j].key;
					const jjson_val_t *fv = ccomp->u.obj.pairs[j].val;
					if (!fk || !fv) continue;
					if (strcmp(fk, "op") == 0 || strncmp(fk, "op_", 3) == 0) continue;
					if (fv->type == JJSON_STRING) {
						jarvis_cap_compile_field_t *cf = &dst->compile.fields[dst->compile.field_count++];
						snprintf(cf->field, sizeof(cf->field), "%s", fk);
						snprintf(cf->tpl, sizeof(cf->tpl), "%s", fv->u.str);
					} else if (fv->type == JJSON_OBJECT &&
					           (strcmp(fk, "target") == 0 || strcmp(fk, "args") == 0)) {
						for (size_t nk = 0; nk < fv->u.obj.count && dst->compile.field_count < JARVIS_MAX_CAP_COMPILE_FIELDS; nk++) {
							const char *sub_k = fv->u.obj.pairs[nk].key;
							const jjson_val_t *sub_v = fv->u.obj.pairs[nk].val;
							if (!sub_k || !sub_v || sub_v->type != JJSON_STRING) continue;
							jarvis_cap_compile_field_t *cf = &dst->compile.fields[dst->compile.field_count++];
							snprintf(cf->field, sizeof(cf->field), "%s.%s", fk, sub_k);
							snprintf(cf->tpl, sizeof(cf->tpl), "%s", sub_v->u.str);
						}
					}
				}
			}
		}
	}
	if (abs_caps) jjson_free(abs_caps);

	jjson_free(root);
	g_active_ontology = ont;
	return ont;
}

static jarvis_ontology_t *try_load_ontology_file(const char *path)
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
	jarvis_ontology_t *ont = jarvis_ontology_create_from_json(buf);
	free(buf);
	return ont;
#else
	(void)path;
	return NULL;
#endif
}

jarvis_ontology_t *jarvis_ontology_create(const char *json_path)
{
#ifndef __wasm__
	if (json_path && *json_path) {
		jarvis_ontology_t *ont = try_load_ontology_file(json_path);
		if (ont) return ont;
	}
	const char *env_p = getenv("JARVIS_ONTOLOGY_JSON");
	if (env_p && *env_p) {
		jarvis_ontology_t *ont = try_load_ontology_file(env_p);
		if (ont) return ont;
	}
	const char *home = getenv("HOME");
	if (home && *home) {
		char home_cfg[512];
		snprintf(home_cfg, sizeof(home_cfg), "%s/.config/jarvis/ontology.json", home);
		jarvis_ontology_t *ont = try_load_ontology_file(home_cfg);
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
		jarvis_ontology_t *ont = try_load_ontology_file(fallback_paths[i]);
		if (ont) return ont;
	}
#else
	(void)json_path;
#endif
	return jarvis_ontology_create_from_json("{}");
}

const char *jarvis_ontology_get_wake_word(const jarvis_ontology_t *ont)
{
	if (!ont) ont = jarvis_ontology_get_active();
	return (ont && ont->wake_word[0]) ? ont->wake_word : "jarvis";
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

bool jarvis_ontology_is_verb_head(const jarvis_ontology_t *ont, const char *word)
{
	if (!ont || !word) return false;
	for (size_t i = 0; i < ont->verb_head_count; i++) {
		if (strcmp(ont->verb_heads[i], word) == 0) return true;
	}
	return false;
}

bool jarvis_ontology_is_vocab_word(const jarvis_ontology_t *ont, const char *word)
{
	if (!ont || !word) return false;
	for (size_t i = 0; i < ont->vocab_word_count; i++) {
		if (strcmp(ont->vocab_words[i], word) == 0) return true;
	}
	return false;
}

bool jarvis_ontology_is_polite_prefix(const jarvis_ontology_t *ont, const char *phrase)
{
	if (!ont || !phrase) return false;
	for (size_t i = 0; i < ont->polite_prefix_count; i++) {
		if (strcmp(ont->polite_prefixes[i], phrase) == 0) return true;
	}
	if (ont->wake_word[0] && strcmp(ont->wake_word, phrase) == 0) return true;
	return false;
}

bool jarvis_ontology_is_dictation_verb(const jarvis_ontology_t *ont, const char *word)
{
	if (!ont || !word) return false;
	for (size_t i = 0; i < ont->dictation_verb_count; i++) {
		if (strcmp(ont->dictation_verbs[i], word) == 0) return true;
	}
	return false;
}

bool jarvis_ontology_is_deictic_pronoun(const jarvis_ontology_t *ont, const char *word)
{
	if (!ont || !word) return false;
	for (size_t i = 0; i < ont->deictic_pronoun_count; i++) {
		if (strcmp(ont->deictic_pronouns[i], word) == 0) return true;
	}
	return false;
}

bool jarvis_ontology_is_tts_interrupt(const jarvis_ontology_t *ont, const char *clean_clause)
{
	if (!ont || !clean_clause) return false;
	for (size_t i = 0; i < ont->tts_interrupt_count; i++) {
		if (strcmp(ont->tts_interrupts[i], clean_clause) == 0) return true;
	}
	return false;
}

bool jarvis_ontology_is_interrogative(const jarvis_ontology_t *ont, const char *word)
{
	if (!ont || !word) return false;
	for (size_t i = 0; i < ont->interrogative_count; i++) {
		if (strcmp(ont->interrogatives[i], word) == 0) return true;
	}
	return false;
}

bool jarvis_ontology_clause_targets_other(const jarvis_ontology_t *ont, const char *clause)
{
	if (!ont || !clause) return false;
	char clean[JARVIS_CLAUSE_LEN];
	jarvis_clean_phrase(clause, clean, sizeof(clean));
	char padded[JARVIS_CLAUSE_LEN + 8];
	snprintf(padded, sizeof(padded), " %s ", clean);

	for (size_t i = 0; i < ont->loc_other_count; i++) {
		char lc[JARVIS_WORD_LEN], probe[JARVIS_WORD_LEN + 4];
		jarvis_clean_phrase(ont->loc_other[i], lc, sizeof(lc));
		if (lc[0]) {
			snprintf(probe, sizeof(probe), " %s ", lc);
			if (strstr(padded, probe)) return true;
		}
	}
	for (size_t i = 0; i < ont->entity_phrase_count; i++) {
		for (size_t t = 0; t < ont->entity_phrases[i].tag_count; t++) {
			if (strcmp(ont->entity_phrases[i].tags[t].type, "ui.relative_window") == 0 &&
			    strcmp(ont->entity_phrases[i].tags[t].id, "other") == 0) {
				char probe[JARVIS_NAME_LEN + 4];
				snprintf(probe, sizeof(probe), " %s ", ont->entity_phrases[i].phrase);
				if (strstr(padded, probe)) return true;
			}
		}
	}
	return false;
}

int jarvis_ontology_extract_workspace_number(const jarvis_ontology_t *ont, const char *clause, int def_ws)
{
	if (!clause) return def_ws;
	char clean[JARVIS_CLAUSE_LEN];
	jarvis_clean_phrase(clause, clean, sizeof(clean));
	char *save = NULL;
	for (char *tok = strtok_r(clean, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		if (tok[0] >= '1' && tok[0] <= '9' && tok[1] == '\0') {
			return tok[0] - '0';
		}
		if (ont) {
			for (size_t i = 0; i < ont->entity_phrase_count; i++) {
				if (strcmp(ont->entity_phrases[i].phrase, tok) == 0) {
					for (size_t t = 0; t < ont->entity_phrases[i].tag_count; t++) {
						if (strcmp(ont->entity_phrases[i].tags[t].type, "ui.workspace") == 0) {
							int v = atoi(ont->entity_phrases[i].tags[t].id);
							if (v >= 1 && v <= 9) return v;
						}
					}
				}
			}
		}
	}
	return def_ws;
}

const jarvis_verb_entry_t *jarvis_ontology_find_verb_phrase(const jarvis_ontology_t *ont, const char *phrase)
{
	if (!ont || !phrase) return NULL;
	for (size_t i = 0; i < ont->verb_phrase_count; i++) {
		if (strcmp(ont->verb_phrases[i].phrase, phrase) == 0) {
			return &ont->verb_phrases[i];
		}
	}
	return NULL;
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

const jarvis_verb_entry_t *jarvis_ontology_find_verb_phrase_fuzzy(
	const jarvis_ontology_t *ont,
	const char              *phrase,
	double                  *out_sim
)
{
	if (out_sim) *out_sim = 0.0;
	if (!ont || !phrase || !*phrase) return NULL;
	const jarvis_verb_entry_t *exact = jarvis_ontology_find_verb_phrase(ont, phrase);
	if (exact) {
		if (out_sim) *out_sim = 1.0;
		return exact;
	}
	if (strlen(phrase) < 5) return NULL;

	const jarvis_verb_entry_t *best = NULL;
	double best_sim = 0.78;
	bool phrase_has_space = (strchr(phrase, ' ') != NULL);
	for (size_t i = 0; i < ont->verb_phrase_count; i++) {
		const char *vp = ont->verb_phrases[i].phrase;
		if (strlen(vp) < 5) continue;
		if ((strchr(vp, ' ') != NULL) != phrase_has_space) continue;
		double sim = jarvis_token_similarity(phrase, vp);
		if (sim > best_sim) {
			best_sim = sim;
			best = &ont->verb_phrases[i];
		}
	}
	if (best && out_sim) *out_sim = best_sim;
	return best;
}

static void live_index_add(jarvis_live_index_t *idx, const char *clean_p, const char *etype, const char *eid)
{
	if (!clean_p || !*clean_p) return;
	for (size_t i = 0; i < idx->count; i++) {
		if (strcmp(idx->entries[i].phrase, clean_p) == 0) {
			for (size_t t = 0; t < idx->entries[i].tag_count; t++) {
				if (strcmp(idx->entries[i].tags[t].type, etype) == 0 &&
				    strcmp(idx->entries[i].tags[t].id, eid) == 0) {
					return;
				}
			}
			if (idx->entries[i].tag_count < JARVIS_MAX_ENTITY_TAGS) {
				size_t ti = idx->entries[i].tag_count++;
				snprintf(idx->entries[i].tags[ti].type,
				         sizeof(idx->entries[i].tags[ti].type), "%s", etype);
				snprintf(idx->entries[i].tags[ti].id,
				         sizeof(idx->entries[i].tags[ti].id), "%s", eid);
			}
			return;
		}
	}
	if (idx->count < sizeof(idx->entries) / sizeof(idx->entries[0])) {
		jarvis_entity_entry_t *e = &idx->entries[idx->count++];
		memset(e, 0, sizeof(*e));
		snprintf(e->phrase, sizeof(e->phrase), "%s", clean_p);
		snprintf(e->tags[0].type, sizeof(e->tags[0].type), "%s", etype);
		snprintf(e->tags[0].id, sizeof(e->tags[0].id), "%s", eid);
		e->tag_count = 1;
	}
}

static void format_template_phrase(const char *tmpl, const char *name, char *out, size_t out_sz)
{
	out[0] = '\0';
	const char *pct = strstr(tmpl, "%s");
	if (pct) {
		size_t pre = (size_t)(pct - tmpl);
		char raw[192];
		snprintf(raw, sizeof(raw), "%.*s%s%s", (int)pre, tmpl, name, pct + 2);
		jarvis_clean_phrase(raw, out, out_sz);
	} else {
		jarvis_clean_phrase(name, out, out_sz);
	}
}

void jarvis_ontology_build_live_index(
	const jarvis_ontology_t *ont,
	const jarvis_world_t    *world,
	jarvis_live_index_t     *out_idx
)
{
	if (!out_idx) return;
	out_idx->count = 0;
	if (ont) {
		for (size_t i = 0; i < ont->entity_phrase_count; i++) {
			out_idx->entries[out_idx->count++] = ont->entity_phrases[i];
		}
	}

#ifndef __wasm__
	/* Dynamically add $HOME subdirectories as fs.location using ontology templates */
	const char *home = getenv("HOME");
	if (home && *home) {
		DIR *d = opendir(home);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] == '.') continue;
				char full_p[512];
				snprintf(full_p, sizeof(full_p), "%s/%s", home, de->d_name);
				struct stat st;
				if (stat(full_p, &st) == 0 && S_ISDIR(st.st_mode)) {
					char ec[128];
					if (jarvis_clean_phrase(de->d_name, ec, sizeof(ec)) >= 2) {
						char rel[300];
						snprintf(rel, sizeof(rel), "~/%s", de->d_name);
						if (!ont || (!jarvis_ontology_find_verb_phrase(ont, ec) && !jarvis_live_index_find(out_idx, ec))) {
							live_index_add(out_idx, ec, "fs.location", rel);
						}
						if (ont) {
							for (size_t ti = 0; ti < ont->fs_template_count; ti++) {
								char tp[192];
								format_template_phrase(ont->fs_templates[ti], ec, tp, sizeof(tp));
								if (tp[0]) live_index_add(out_idx, tp, "fs.location", rel);
							}
						}
					}
				}
			}
			closedir(d);
		}
	}
#endif

	if (!world) return;

	/* Index custom entities contributed by XY modules (desktop apps, site entities, ND mobs) */
	for (size_t i = 0; i < world->custom_entity_count; i++) {
		const jarvis_live_entity_t *ce = &world->custom_entities[i];
		char clean_p[128];
		if (jarvis_clean_phrase(ce->phrase, clean_p, sizeof(clean_p)) >= 2) {
			if (!ont || !jarvis_ontology_find_verb_phrase(ont, clean_p)) {
				live_index_add(out_idx, clean_p, ce->type, ce->id);
			}
		}
	}

	/* Index live windows */
	for (size_t i = 0; i < world->client_count; i++) {
		const jarvis_window_t *w = &world->clients[i];
		const char *names[2] = {w->wm_class, w->title};
		for (int n = 0; n < 2; n++) {
			if (!names[n] || !*names[n]) continue;
			char nc[128];
			if (jarvis_clean_phrase(names[n], nc, sizeof(nc)) >= 2) {
				live_index_add(out_idx, nc, "ui.window", w->id);
				if (ont) {
					for (size_t ti = 0; ti < ont->window_template_count; ti++) {
						char tp[192];
						format_template_phrase(ont->window_templates[ti], nc, tp, sizeof(tp));
						if (tp[0]) live_index_add(out_idx, tp, "ui.window", w->id);
					}
					for (size_t ti = 0; ti < ont->tab_template_count; ti++) {
						char tp[192];
						format_template_phrase(ont->tab_templates[ti], nc, tp, sizeof(tp));
						if (tp[0]) live_index_add(out_idx, tp, "ui.window", w->id);
					}
				}
			}
		}
	}

	/* Index live browser tabs */
	for (size_t i = 0; i < world->tab_count; i++) {
		const jarvis_tab_t *t = &world->tabs[i];
		char nc[128];
		if (jarvis_clean_phrase(t->title, nc, sizeof(nc)) >= 2) {
			live_index_add(out_idx, nc, "ui.tab", t->url);
			if (ont) {
				for (size_t ti = 0; ti < ont->tab_template_count; ti++) {
					char tp[192];
					format_template_phrase(ont->tab_templates[ti], nc, tp, sizeof(tp));
					if (tp[0]) live_index_add(out_idx, tp, "ui.tab", t->url);
				}
			}
		}
	}
}

const jarvis_entity_entry_t *jarvis_live_index_find(
	const jarvis_live_index_t *idx,
	const char                *phrase
)
{
	if (!idx || !phrase) return NULL;
	for (size_t i = 0; i < idx->count; i++) {
		if (strcmp(idx->entries[i].phrase, phrase) == 0) {
			return &idx->entries[i];
		}
	}
	return NULL;
}

const jarvis_entity_entry_t *jarvis_live_index_find_fuzzy(
	const jarvis_live_index_t *idx,
	const char                *phrase,
	double                    *out_sim
)
{
	if (out_sim) *out_sim = 0.0;
	if (!idx || !phrase || !*phrase) return NULL;
	const jarvis_entity_entry_t *exact = jarvis_live_index_find(idx, phrase);
	if (exact) {
		if (out_sim) *out_sim = 1.0;
		return exact;
	}
	if (strlen(phrase) < 5) return NULL;

	const jarvis_entity_entry_t *best = NULL;
	double best_sim = 0.75;
	bool phrase_has_space = (strchr(phrase, ' ') != NULL);
	for (size_t i = 0; i < idx->count; i++) {
		const char *ep = idx->entries[i].phrase;
		if (strlen(ep) < 5) continue;
		if ((strchr(ep, ' ') != NULL) != phrase_has_space) continue;
		double sim = jarvis_token_similarity(phrase, ep);
		if (sim > best_sim) {
			best_sim = sim;
			best = &idx->entries[i];
		}
	}
	if (best && out_sim) *out_sim = best_sim;
	return best;
}

static bool is_open_arg_verb_token(const jarvis_ontology_t *ont, const char *w_clean)
{
	if (!ont || !w_clean || !*w_clean) return false;
	if (jarvis_ontology_is_dictation_verb(ont, w_clean)) return true;
	const jarvis_verb_entry_t *ve = jarvis_ontology_find_verb_phrase(ont, w_clean);
	if (!ve) return false;
	for (size_t t = 0; t < ve->tag_count; t++) {
		const char *c = ve->tags[t].concept;
		if (strcmp(c, "INPUT_TYPE") == 0 ||
		    strcmp(c, "TYPE_WRITE") == 0 ||
		    strcmp(c, "TYPE_SUBMIT") == 0 ||
		    strcmp(c, "SEARCH_WEB") == 0 ||
		    strcmp(c, "PLAY_STREAM") == 0) {
			return true;
		}
	}
	return false;
}

void jarvis_ontology_normalize_asr(
	const jarvis_ontology_t *ont,
	const jarvis_world_t    *world,
	const char              *raw_text,
	char                    *out,
	size_t                   out_sz
)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!raw_text) return;

	while (*raw_text && isspace((unsigned char)*raw_text)) raw_text++;
	if (!*raw_text) return;

	char work[512];
	snprintf(work, sizeof(work), "%s", raw_text);
	size_t wlen = strlen(work);
	while (wlen > 0 && isspace((unsigned char)work[wlen - 1])) {
		work[--wlen] = '\0';
	}

	/* 1. Structural prefix-echo truncation after sentence period */
	char *dot = strstr(work, ". ");
	if (dot) {
		char *last_dot = dot;
		for (char *p = strstr(dot + 2, ". "); p; p = strstr(p + 2, ". ")) {
			last_dot = p;
		}
		char first_c[256], last_c[256];
		char first_part[256];
		size_t flen = (size_t)(dot - work);
		if (flen >= sizeof(first_part)) flen = sizeof(first_part) - 1;
		memcpy(first_part, work, flen);
		first_part[flen] = '\0';

		jarvis_clean_phrase(first_part, first_c, sizeof(first_c));
		jarvis_clean_phrase(last_dot + 2, last_c, sizeof(last_c));
		if (first_c[0] && last_c[0] && strlen(last_c) < strlen(first_c) &&
		    strncmp(first_c, last_c, strlen(last_c)) == 0) {
			*last_dot = '\0';
		}
	}

	jarvis_live_index_t *live_idx = calloc(1, sizeof(*live_idx));
	if (!live_idx) return;
	jarvis_ontology_build_live_index(ont, world, live_idx);

	/* 2. Tokenize and fold two-word compound terms from ontology asr_corrections or split vocab words */
	char tok_buf[512];
	snprintf(tok_buf, sizeof(tok_buf), "%s", work);
	char *raw_tokens[64];
	size_t n_raw = 0;
	char *save = NULL;
	for (char *t = strtok_r(tok_buf, " \t\r\n", &save); t && n_raw < 64; t = strtok_r(NULL, " \t\r\n", &save)) {
		raw_tokens[n_raw++] = t;
	}

	char folded_toks[64][JARVIS_ID_LEN];
	char clean_toks[64][JARVIS_ID_LEN];
	size_t n_tok = 0;
	for (size_t i = 0; i < n_raw && n_tok < 64; ) {
		if (i + 1 < n_raw) {
			char pair_raw[128], pair_clean[128], joined[128];
			snprintf(pair_raw, sizeof(pair_raw), "%s %s", raw_tokens[i], raw_tokens[i + 1]);
			jarvis_clean_phrase(pair_raw, pair_clean, sizeof(pair_clean));

			if (ont) {
				const char *fixed = NULL;
				for (size_t fi = 0; fi < ont->asr_fix_count; fi++) {
					if (strcmp(pair_clean, ont->asr_fixes[fi].from) == 0) {
						fixed = ont->asr_fixes[fi].to;
						break;
					}
				}
				if (fixed) {
					snprintf(folded_toks[n_tok], sizeof(folded_toks[0]), "%s", fixed);
					jarvis_clean_phrase(fixed, clean_toks[n_tok], sizeof(clean_toks[0]));
					n_tok++;
					i += 2;
					continue;
				}
			}

			size_t jw = 0;
			for (size_t k = 0; pair_clean[k] && jw + 1 < sizeof(joined); k++) {
				if (pair_clean[k] != ' ') joined[jw++] = pair_clean[k];
			}
			joined[jw] = '\0';

			if (ont && jarvis_ontology_is_vocab_word(ont, joined) &&
			    !jarvis_ontology_find_verb_phrase(ont, pair_clean) &&
			    !jarvis_live_index_find(live_idx, pair_clean)) {
				snprintf(folded_toks[n_tok], sizeof(folded_toks[0]), "%s", joined);
				snprintf(clean_toks[n_tok], sizeof(clean_toks[0]), "%s", joined);
				n_tok++;
				i += 2;
				continue;
			}
		}
		snprintf(folded_toks[n_tok], sizeof(folded_toks[0]), "%s", raw_tokens[i]);
		jarvis_clean_phrase(raw_tokens[i], clean_toks[n_tok], sizeof(clean_toks[0]));
		n_tok++;
		i++;
	}

	/* 3. Strip leading Whisper prompt-echo fragments ("o que e" / "o que") when immediately
	 *    followed by a polite modal or command verb (e.g. "o que épode ir ...").
	 */
	size_t start_idx = 0;
	if (ont && n_tok >= 3 &&
	    strcmp(clean_toks[0], "o") == 0 && strcmp(clean_toks[1], "que") == 0) {
		size_t check_idx = 2;
		if (strcmp(clean_toks[2], "e") == 0 && n_tok >= 4) {
			check_idx = 3;
		}
		const char *nxt = clean_toks[check_idx];
		bool followed_by_modal_or_verb =
			jarvis_ontology_is_polite_prefix(ont, nxt) ||
			jarvis_ontology_is_verb_head(ont, nxt) ||
			jarvis_token_similarity(nxt, "podes") >= 0.75 ||
			jarvis_token_similarity(nxt, "pode") >= 0.75;
		if (followed_by_modal_or_verb) {
			start_idx = check_idx;
		}
	}

	/* Snap elided leading modal token (e.g. "épode" -> "podes") */
	if (ont && start_idx < n_tok && !jarvis_ontology_is_vocab_word(ont, clean_toks[start_idx])) {
		for (size_t pi = 0; pi < ont->polite_prefix_count; pi++) {
			const char *pp = ont->polite_prefixes[pi];
			if (!strchr(pp, ' ') && strlen(pp) >= 4 &&
			    jarvis_token_similarity(clean_toks[start_idx], pp) >= 0.76) {
				snprintf(folded_toks[start_idx], sizeof(folded_toks[0]), "%s", pp);
				snprintf(clean_toks[start_idx], sizeof(clean_toks[0]), "%s", pp);
				break;
			}
		}
	}

	/* 4. Strip trailing phonetically-distorted polite suffixes (e.g. "esfçibora" -> "se faz favor")
	 *    when preceded by at least one content word.
	 */
	if (ont && n_tok > start_idx + 1) {
		const char *last_c = clean_toks[n_tok - 1];
		if (strlen(last_c) >= 6 &&
		    !jarvis_ontology_is_vocab_word(ont, last_c) &&
		    !jarvis_live_index_find(live_idx, last_c)) {
			bool is_polite_tail = false;
			for (size_t pi = 0; pi < ont->polite_prefix_count && !is_polite_tail; pi++) {
				if (jarvis_token_similarity(last_c, ont->polite_prefixes[pi]) >= 0.74) {
					is_polite_tail = true;
				}
			}
			for (size_t li = 0; li < ont->loc_any_count && !is_polite_tail; li++) {
				char lc[JARVIS_WORD_LEN];
				jarvis_clean_phrase(ont->loc_any[li], lc, sizeof(lc));
				if (jarvis_token_similarity(last_c, lc) >= 0.74) {
					is_polite_tail = true;
				}
			}
			if (is_polite_tail) {
				n_tok--;
				if (n_tok > start_idx) {
					size_t prev_len = strlen(folded_toks[n_tok - 1]);
					while (prev_len > 0 && strchr(",.!?;:", folded_toks[n_tok - 1][prev_len - 1])) {
						folded_toks[n_tok - 1][--prev_len] = '\0';
					}
				}
			}
		}
	}

	/* 5. Phonetic Lexicon Snapping for out-of-vocabulary tokens (e.g. "coculdora" -> "calculadora")
	 *    unless the clause is an open-argument dictation, search, or media query.
	 */
	bool in_open_arg = false;
	if (ont) {
		for (size_t i = start_idx; i < n_tok; i++) {
			if (is_open_arg_verb_token(ont, clean_toks[i])) {
				in_open_arg = true;
				break;
			}
		}
	}

	if (ont && !in_open_arg) {
		for (size_t i = start_idx; i < n_tok; i++) {
			const char *ct = clean_toks[i];
			size_t ct_len = strlen(ct);
			if (ct_len < 5) continue;
			if (jarvis_ontology_is_vocab_word(ont, ct) ||
			    jarvis_ontology_is_stopword(ont, ct) ||
			    jarvis_live_index_find(live_idx, ct)) {
				continue;
			}
			/* Skip contractions, URLs, or path-like tokens */
			if (strchr(folded_toks[i], '\'') || strchr(folded_toks[i], '/') || strchr(folded_toks[i], '.')) {
				continue;
			}

			const char *best_word = NULL;
			double best_sim = (ct_len >= 7) ? 0.74 : 0.78;

			/* If this token is immediately followed by another content word (i + 1 < n_tok),
			 * check stopwords first (e.g. "a vinha coculdora" -> "a minha calculadora") so
			 * possessive determiners before a noun do not get snapped to an unrelated noun ("linha").
			 */
			if (i + 1 < n_tok) {
				for (size_t si = 0; si < ont->stopword_count; si++) {
					const char *sw = ont->stopwords[si];
					if (strlen(sw) < 5) continue;
					double sim = jarvis_token_similarity(ct, sw);
					if (sim > best_sim) {
						best_sim = sim;
						best_word = sw;
					}
				}
			}

			/* Check single-word live entities */
			for (size_t ei = 0; ei < live_idx->count; ei++) {
				const char *ep = live_idx->entries[ei].phrase;
				if (strchr(ep, ' ') || strlen(ep) < 5) continue;
				double sim = jarvis_token_similarity(ct, ep);
				if (sim > best_sim) {
					best_sim = sim;
					best_word = ep;
				}
			}
			/* Check ontology vocabulary words of length >= 7 */
			if (ct_len >= 7) {
				for (size_t vi = 0; vi < ont->vocab_word_count; vi++) {
					const char *vw = ont->vocab_words[vi];
					if (strlen(vw) < 7) continue;
					double sim = jarvis_token_similarity(ct, vw);
					if (sim > best_sim) {
						best_sim = sim;
						best_word = vw;
					}
				}
			}
			if (!best_word) {
				for (size_t si = 0; si < ont->stopword_count; si++) {
					const char *sw = ont->stopwords[si];
					if (strlen(sw) < 5) continue;
					double sim = jarvis_token_similarity(ct, sw);
					if (sim > best_sim) {
						best_sim = sim;
						best_word = sw;
					}
				}
			}

			if (best_word) {
				snprintf(folded_toks[i], sizeof(folded_toks[0]), "%s", best_word);
				snprintf(clean_toks[i], sizeof(clean_toks[0]), "%s", best_word);
			}
		}
	}

	size_t pos = 0;
	for (size_t i = start_idx; i < n_tok; i++) {
		pos += (size_t)snprintf(out + pos, pos < out_sz ? out_sz - pos : 0,
		                        "%s%s", (i > start_idx) ? " " : "", folded_toks[i]);
	}
	free(live_idx);
}

