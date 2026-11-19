#include "jarvis-internal.h"
#include "jarvis-default-ontology.h"

#ifndef __wasm__
#include <dirent.h>
#include <sys/stat.h>
#endif

size_t jarvis_clean_phrase(const char *text, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return 0;
	out[0] = '\0';
	if (!text) return 0;

	size_t w = 0;
	bool prev_space = true;

	for (const unsigned char *p = (const unsigned char *)text; *p && w + 1 < out_sz; p++) {
		unsigned char c = *p;
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
	char alnum[256];
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p && n < sizeof(alnum); p++) {
		if (isalnum(*p)) {
			alnum[n++] = (char)tolower(*p);
		}
	}
	if (n < 2) return true;

	size_t max_run = 1;
	size_t cur_run = 1;
	for (size_t i = 1; i < n; i++) {
		if (alnum[i] == alnum[i - 1]) {
			cur_run++;
			if (cur_run > max_run) max_run = cur_run;
		} else {
			cur_run = 1;
		}
	}
	if (max_run >= 4) return true;

	if (n >= 4) {
		size_t counts[256] = {0};
		size_t most = 0;
		for (size_t i = 0; i < n; i++) {
			unsigned char k = (unsigned char)alnum[i];
			counts[k]++;
			if (counts[k] > most) most = counts[k];
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

static void load_locative_list(const jjson_val_t *arr, char dst[][JARVIS_WORD_LEN], size_t *out_count)
{
	*out_count = 0;
	if (!arr || arr->type != JJSON_ARRAY) return;
	for (size_t i = 0; i < arr->u.arr.count && *out_count < JARVIS_MAX_LOCATIVES; i++) {
		const jjson_val_t *it = arr->u.arr.items[i];
		if (it && it->type == JJSON_STRING && it->u.str) {
			char *slot = dst[(*out_count)++];
			snprintf(slot, sizeof(dst[0]), "%s", it->u.str);
		}
	}
}

static void load_short_str_list(const jjson_val_t *arr, char dst[][JARVIS_SHORT_LEN], size_t max_items, size_t *out_count)
{
	*out_count = 0;
	if (!arr || arr->type != JJSON_ARRAY) return;
	for (size_t i = 0; i < arr->u.arr.count && *out_count < max_items; i++) {
		const jjson_val_t *it = arr->u.arr.items[i];
		if (it && it->type == JJSON_STRING && it->u.str) {
			char *slot = dst[(*out_count)++];
			snprintf(slot, sizeof(dst[0]), "%s", it->u.str);
		}
	}
}

static void load_id_str_list(const jjson_val_t *arr, char dst[][JARVIS_ID_LEN], size_t max_items, size_t *out_count)
{
	*out_count = 0;
	if (!arr || arr->type != JJSON_ARRAY) return;
	for (size_t i = 0; i < arr->u.arr.count && *out_count < max_items; i++) {
		const jjson_val_t *it = arr->u.arr.items[i];
		if (it && it->type == JJSON_STRING && it->u.str) {
			char *slot = dst[(*out_count)++];
			snprintf(slot, sizeof(dst[0]), "%s", it->u.str);
		}
	}
}

jarvis_ontology_t *jarvis_ontology_create_from_json(const char *json_str)
{
	const char *src = (json_str && *json_str) ? json_str : JARVIS_DEFAULT_ONTOLOGY_JSON;
	jjson_val_t *root = jjson_parse(src);
	if (!root || root->type != JJSON_OBJECT) {
		jjson_free(root);
		return NULL;
	}

	jarvis_ontology_t *ont = (jarvis_ontology_t *)calloc(1, sizeof(jarvis_ontology_t));
	if (!ont) {
		jjson_free(root);
		return NULL;
	}

	/* 0. Declarative lexical tables (wake words, polite prefixes, dictation verbs, etc.) */
	const jjson_val_t *def_root = (src != JARVIS_DEFAULT_ONTOLOGY_JSON) ? jjson_parse(JARVIS_DEFAULT_ONTOLOGY_JSON) : NULL;
	const char *ww_str = jjson_obj_get_str(root, "wake_word", NULL);
	if (!ww_str && def_root) ww_str = jjson_obj_get_str(def_root, "wake_word", "");
	snprintf(ont->wake_word, sizeof(ont->wake_word), "%s", ww_str ? ww_str : "");

	#define LOAD_SHORT_WITH_FALLBACK(key, field, max_cnt, cnt_field) do { \
		const jjson_val_t *_a = jjson_obj_get(root, (key)); \
		if (!_a && def_root) _a = jjson_obj_get(def_root, (key)); \
		load_short_str_list(_a, (field), (max_cnt), &(cnt_field)); \
	} while (0)

	#define LOAD_ID_WITH_FALLBACK(key, field, max_cnt, cnt_field) do { \
		const jjson_val_t *_a = jjson_obj_get(root, (key)); \
		if (!_a && def_root) _a = jjson_obj_get(def_root, (key)); \
		load_id_str_list(_a, (field), (max_cnt), &(cnt_field)); \
	} while (0)

	LOAD_SHORT_WITH_FALLBACK("wake_variants", ont->wake_variants, JARVIS_MAX_WAKE_VARIANTS, ont->wake_variant_count);
	LOAD_SHORT_WITH_FALLBACK("polite_prefixes", ont->polite_prefixes, JARVIS_MAX_POLITE_PREFIXES, ont->polite_prefix_count);
	LOAD_SHORT_WITH_FALLBACK("dictation_verbs", ont->dictation_verbs, JARVIS_MAX_DICTATION_VERBS, ont->dictation_verb_count);
	LOAD_SHORT_WITH_FALLBACK("deictic_pronouns", ont->deictic_pronouns, JARVIS_MAX_DEICTIC_PRONOUNS, ont->deictic_pronoun_count);
	LOAD_ID_WITH_FALLBACK("tts_interrupt_phrases", ont->tts_interrupts, JARVIS_MAX_TTS_INTERRUPTS, ont->tts_interrupt_count);
	LOAD_ID_WITH_FALLBACK("lockdown_phrases", ont->lockdown_phrases, JARVIS_MAX_LOCKDOWN_PHRASES, ont->lockdown_phrase_count);
	LOAD_ID_WITH_FALLBACK("hallucination_phrases", ont->hallucinations, JARVIS_MAX_HALLUCINATIONS, ont->hallucination_count);

	#undef LOAD_SHORT_WITH_FALLBACK
	#undef LOAD_ID_WITH_FALLBACK
	if (def_root) jjson_free((jjson_val_t *)def_root);

	/* 1. Stopwords */
	load_short_str_list(jjson_obj_get(root, "stopwords"), ont->stopwords, JARVIS_MAX_STOPWORDS, &ont->stopword_count);

	/* 2. Clause separators */
	load_short_str_list(jjson_obj_get(root, "clause_separators"), ont->clause_seps, JARVIS_MAX_CLAUSE_SEPS, &ont->clause_sep_count);

	/* 3. Verbs */
	const jjson_val_t *verbs = jjson_obj_get(root, "verbs");
	if (verbs && verbs->type == JJSON_OBJECT) {
		for (size_t i = 0; i < verbs->u.obj.count; i++) {
			const char *concept = verbs->u.obj.pairs[i].key;
			const jjson_val_t *grp = verbs->u.obj.pairs[i].val;
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

	/* 4. Entities */
	const jjson_val_t *ents = jjson_obj_get(root, "entities");
	if (ents && ents->type == JJSON_OBJECT) {
		for (size_t i = 0; i < ents->u.obj.count; i++) {
			const char *etype = ents->u.obj.pairs[i].key;
			const jjson_val_t *items = ents->u.obj.pairs[i].val;
			if (!items || items->type != JJSON_ARRAY) continue;
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
		}
	}

	/* 5. Self locatives */
	const jjson_val_t *locs = jjson_obj_get(root, "self_locatives");
	if (locs && locs->type == JJSON_OBJECT) {
		load_locative_list(jjson_obj_get(locs, "submit"), ont->loc_submit, &ont->loc_submit_count);
		load_locative_list(jjson_obj_get(locs, "other"), ont->loc_other, &ont->loc_other_count);
		load_locative_list(jjson_obj_get(locs, "terminal"), ont->loc_terminal, &ont->loc_terminal_count);
		load_locative_list(jjson_obj_get(locs, "any"), ont->loc_any, &ont->loc_any_count);
	}

	/* 6. Capabilities */
	const jjson_val_t *caps = jjson_obj_get(root, "capabilities");
	if (caps && caps->type == JJSON_ARRAY) {
		for (size_t i = 0; i < caps->u.arr.count && ont->capability_count < JARVIS_MAX_CAPABILITIES; i++) {
			const jjson_val_t *c = caps->u.arr.items[i];
			if (!c || c->type != JJSON_OBJECT) continue;
			jarvis_capability_t *dst = &ont->capabilities[ont->capability_count++];
			memset(dst, 0, sizeof(*dst));
			snprintf(dst->id, sizeof(dst->id), "%s", jjson_obj_get_str(c, "id", ""));
			snprintf(dst->surface, sizeof(dst->surface), "%s", jjson_obj_get_str(c, "surface", "any"));
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
						char *slot = dst->required_entities[dst->req_ent_count++];
						snprintf(slot, sizeof(dst->required_entities[0]), "%s", creq->u.arr.items[j]->u.str);
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
		}
	}

	jjson_free(root);
	return ont;
}

jarvis_ontology_t *jarvis_ontology_create(const char *json_path)
{
#ifndef __wasm__
	char resolved[512] = {0};
	if (json_path && *json_path) {
		snprintf(resolved, sizeof(resolved), "%s", json_path);
	} else {
		const char *env_p = getenv("JARVIS_ONTOLOGY_JSON");
		if (env_p && *env_p) {
			snprintf(resolved, sizeof(resolved), "%s", env_p);
		} else {
			const char *home = getenv("HOME");
			if (home && *home) {
				snprintf(resolved, sizeof(resolved), "%s/.config/jarvis/ontology.json", home);
			}
		}
	}

	if (resolved[0]) {
		FILE *fp = fopen(resolved, "rb");
		if (fp) {
			fseek(fp, 0, SEEK_END);
			long sz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (sz > 0 && sz < 1024 * 1024) {
				char *buf = (char *)malloc((size_t)sz + 1);
				if (buf) {
					size_t rd = fread(buf, 1, (size_t)sz, fp);
					buf[rd] = '\0';
					fclose(fp);
					jarvis_ontology_t *ont = jarvis_ontology_create_from_json(buf);
					free(buf);
					if (ont) return ont;
				} else {
					fclose(fp);
				}
			} else {
				fclose(fp);
			}
		}
	}
#else
	(void)json_path;
#endif
	return jarvis_ontology_create_from_json(JARVIS_DEFAULT_ONTOLOGY_JSON);
}

void jarvis_ontology_destroy(jarvis_ontology_t *ont)
{
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

	/* Standard XDG user directories */
	static const struct { const char *name; const char *path; } xdg_dirs[] = {
		{"downloads", "~/Downloads"},
		{"documents", "~/Documents"},
		{"desktop", "~/Desktop"},
		{"pictures", "~/Pictures"},
		{"music", "~/Music"},
		{"videos", "~/Videos"},
		{NULL, NULL}
	};
	for (size_t i = 0; xdg_dirs[i].name; i++) {
		char p_folder[64], p_dir[64];
		snprintf(p_folder, sizeof(p_folder), "%s folder", xdg_dirs[i].name);
		snprintf(p_dir, sizeof(p_dir), "%s directory", xdg_dirs[i].name);
		live_index_add(out_idx, xdg_dirs[i].name, "fs.location", xdg_dirs[i].path);
		live_index_add(out_idx, p_folder, "fs.location", xdg_dirs[i].path);
		live_index_add(out_idx, p_dir, "fs.location", xdg_dirs[i].path);
	}

#ifndef __wasm__
	/* Dynamically add $HOME subdirectories as fs.location */
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
						char rel[300], p_folder[160], p_dir[160];
						snprintf(rel, sizeof(rel), "~/%s", de->d_name);
						snprintf(p_folder, sizeof(p_folder), "%s folder", ec);
						snprintf(p_dir, sizeof(p_dir), "%s directory", ec);
						live_index_add(out_idx, ec, "fs.location", rel);
						live_index_add(out_idx, p_folder, "fs.location", rel);
						live_index_add(out_idx, p_dir, "fs.location", rel);
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
				char nw[160], nt[160];
				snprintf(nw, sizeof(nw), "%s window", nc);
				snprintf(nt, sizeof(nt), "%s tab", nc);
				live_index_add(out_idx, nc, "ui.window", w->id);
				live_index_add(out_idx, nw, "ui.window", w->id);
				live_index_add(out_idx, nt, "ui.window", w->id);
			}
		}
	}

	/* Index live browser tabs */
	for (size_t i = 0; i < world->tab_count; i++) {
		const jarvis_tab_t *t = &world->tabs[i];
		char nc[128];
		if (jarvis_clean_phrase(t->title, nc, sizeof(nc)) >= 2) {
			char nt[160];
			snprintf(nt, sizeof(nt), "%s tab", nc);
			live_index_add(out_idx, nc, "ui.tab", t->url);
			live_index_add(out_idx, nt, "ui.tab", t->url);
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

	/* 2. Fold two-word compound terms ("low fee" -> "lofi", or split vocab words) */
	char tok_buf[512];
	snprintf(tok_buf, sizeof(tok_buf), "%s", work);
	char *tokens[64];
	size_t n_tok = 0;
	char *save = NULL;
	for (char *t = strtok_r(tok_buf, " \t\r\n", &save); t && n_tok < 64; t = strtok_r(NULL, " \t\r\n", &save)) {
		tokens[n_tok++] = t;
	}

	size_t pos = 0;
	size_t i = 0;
	while (i < n_tok) {
		if (i + 1 < n_tok) {
			char pair_raw[128], pair_clean[128], joined[128];
			snprintf(pair_raw, sizeof(pair_raw), "%s %s", tokens[i], tokens[i + 1]);
			jarvis_clean_phrase(pair_raw, pair_clean, sizeof(pair_clean));

			if (strcmp(pair_clean, "low fee") == 0 || strcmp(pair_clean, "lo fee") == 0 ||
			    strcmp(pair_clean, "low fi") == 0 || strcmp(pair_clean, "lo fi") == 0 ||
			    strcmp(pair_clean, "low fly") == 0) {
				pos += (size_t)snprintf(out + pos, pos < out_sz ? out_sz - pos : 0, "%slofi", pos ? " " : "");
				i += 2;
				continue;
			}

			size_t jw = 0;
			for (size_t k = 0; pair_clean[k] && jw + 1 < sizeof(joined); k++) {
				if (pair_clean[k] != ' ') joined[jw++] = pair_clean[k];
			}
			joined[jw] = '\0';

			if (ont && jarvis_ontology_is_vocab_word(ont, joined) &&
			    !jarvis_ontology_find_verb_phrase(ont, pair_clean) &&
			    !jarvis_live_index_find(live_idx, pair_clean)) {
				pos += (size_t)snprintf(out + pos, pos < out_sz ? out_sz - pos : 0, "%s%s", pos ? " " : "", joined);
				i += 2;
				continue;
			}
		}
		pos += (size_t)snprintf(out + pos, pos < out_sz ? out_sz - pos : 0, "%s%s", pos ? " " : "", tokens[i]);
		i++;
	}
	free(live_idx);
}
