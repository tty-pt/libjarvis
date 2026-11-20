#include "jarvis-internal.h"

static void trim_punct_ws(char *s)
{
	if (!s) return;
	char *p = s;
	while (*p && (isspace((unsigned char)*p) || strchr(",.!?;:", *p))) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t len = strlen(s);
	while (len > 0 && (isspace((unsigned char)s[len - 1]) || strchr(",.!?;:", s[len - 1]))) {
		s[--len] = '\0';
	}
}

static bool ends_with_ci_space(const char *str, const char *tail)
{
	char str_c[512], tail_c[128];
	jarvis_clean_phrase(str, str_c, sizeof(str_c));
	jarvis_clean_phrase(tail, tail_c, sizeof(tail_c));
	size_t slen = strlen(str_c);
	size_t tlen = strlen(tail_c);
	if (tlen == 0 || slen < tlen + 1) return false;
	if (str_c[slen - tlen - 1] != ' ') return false;
	return strcmp(str_c + slen - tlen, tail_c) == 0;
}

static void strip_tail_words(char *str, const char *tail_phrase)
{
	char tail_c[128];
	jarvis_clean_phrase(tail_phrase, tail_c, sizeof(tail_c));
	size_t n_words = 0;
	for (size_t i = 0; tail_c[i]; i++) {
		if (i == 0 || (tail_c[i] != ' ' && tail_c[i - 1] == ' ')) n_words++;
	}
	size_t len = strlen(str);
	while (n_words > 0 && len > 0) {
		while (len > 0 && (isspace((unsigned char)str[len - 1]) || strchr(",.!?;:", str[len - 1]))) len--;
		while (len > 0 && !isspace((unsigned char)str[len - 1])) len--;
		n_words--;
	}
	str[len] = '\0';
	trim_punct_ws(str);
}

static bool starts_with_ci_space(const char *str, const char *head)
{
	char str_c[512], head_c[128];
	jarvis_clean_phrase(str, str_c, sizeof(str_c));
	jarvis_clean_phrase(head, head_c, sizeof(head_c));
	size_t slen = strlen(str_c);
	size_t hlen = strlen(head_c);
	if (hlen == 0 || slen < hlen + 1) return false;
	if (str_c[hlen] != ' ') return false;
	return strncmp(str_c, head_c, hlen) == 0;
}

static void strip_head_words(char *str, const char *head_phrase)
{
	char head_c[128];
	jarvis_clean_phrase(head_phrase, head_c, sizeof(head_c));
	size_t n_words = 0;
	for (size_t i = 0; head_c[i]; i++) {
		if (i == 0 || (head_c[i] != ' ' && head_c[i - 1] == ' ')) n_words++;
	}
	char *p = str;
	while (n_words > 0 && *p) {
		while (*p && (isspace((unsigned char)*p) || strchr(",.!?;:", *p))) p++;
		while (*p && !isspace((unsigned char)*p)) p++;
		n_words--;
	}
	while (*p && (isspace((unsigned char)*p) || strchr(",.!?;:", *p))) p++;
	memmove(str, p, strlen(p) + 1);
	trim_punct_ws(str);
}

static bool match_and_strip_modifier(char *str, const char *mod_phrase)
{
	if (ends_with_ci_space(str, mod_phrase)) {
		strip_tail_words(str, mod_phrase);
		return true;
	}
	if (starts_with_ci_space(str, mod_phrase)) {
		strip_head_words(str, mod_phrase);
		return true;
	}
	return false;
}

static void strip_locative_modifiers(
	const jarvis_ontology_t *ont,
	const char              *clause,
	const char              *active_surface,
	char                    *out_stripped,
	size_t                   stripped_sz,
	char                    *out_surface,
	size_t                   surf_sz,
	bool                    *out_explicit_term,
	bool                    *out_target_other,
	bool                    *out_submit_enter,
	bool                    *out_new_terminal,
	bool                    *out_bg_play
)
{
	snprintf(out_stripped, stripped_sz, "%s", clause ? clause : "");
	trim_punct_ws(out_stripped);
	snprintf(out_surface, surf_sz, "%s", (active_surface && *active_surface) ? active_surface : "any");
	*out_explicit_term = false;
	*out_target_other  = false;
	*out_submit_enter  = false;
	*out_new_terminal  = false;
	*out_bg_play       = false;
	if (!ont) return;

	bool changed = true;
	while (changed) {
		changed = false;
		for (size_t i = 0; i < ont->loc_submit_count; i++) {
			if (match_and_strip_modifier(out_stripped, ont->loc_submit[i])) {
				*out_submit_enter = true;
				changed = true;
				break;
			}
		}
		if (changed) continue;

		for (size_t i = 0; i < ont->loc_new_terminal_count; i++) {
			if (match_and_strip_modifier(out_stripped, ont->loc_new_terminal[i])) {
				*out_new_terminal = true;
				snprintf(out_surface, surf_sz, "terminal");
				*out_explicit_term = true;
				changed = true;
				break;
			}
		}
		if (changed) continue;

		for (size_t i = 0; i < ont->loc_background_count; i++) {
			if (match_and_strip_modifier(out_stripped, ont->loc_background[i])) {
				*out_bg_play = true;
				changed = true;
				break;
			}
		}
		if (changed) continue;

		for (size_t i = 0; i < ont->loc_other_count; i++) {
			if (match_and_strip_modifier(out_stripped, ont->loc_other[i])) {
				*out_target_other = true;
				changed = true;
				break;
			}
		}
		if (changed) continue;

		for (size_t i = 0; i < ont->loc_terminal_count; i++) {
			if (match_and_strip_modifier(out_stripped, ont->loc_terminal[i])) {
				snprintf(out_surface, surf_sz, "terminal");
				*out_explicit_term = true;
				changed = true;
				break;
			}
		}
		if (changed) continue;

		for (size_t i = 0; i < ont->loc_any_count; i++) {
			if (match_and_strip_modifier(out_stripped, ont->loc_any[i])) {
				changed = true;
				break;
			}
		}
	}

	if (!out_stripped[0] && clause) {
		snprintf(out_stripped, stripped_sz, "%s", clause);
		trim_punct_ws(out_stripped);
	}
}

/* Parsed constituent state */
#define JARVIS_MAX_PARSED_VERBS    16
#define JARVIS_MAX_PARSED_ENTITIES 16
#define JARVIS_MAX_PARSED_ENT_IDS  4

typedef struct {
	struct {
		char   concept[JARVIS_SHORT_LEN];
		double weight;
	} verbs[JARVIS_MAX_PARSED_VERBS];
	size_t verb_count;

	struct {
		char   type[JARVIS_WORD_LEN];
		char   ids[JARVIS_MAX_PARSED_ENT_IDS][JARVIS_NAME_LEN];
		size_t id_count;
	} entities[JARVIS_MAX_PARSED_ENTITIES];
	size_t entity_count;

	bool   mod_new;
	bool   mod_hidden;
	bool   mod_all;
	bool   mod_silent;
	bool   mod_fullscreen;
	bool   mod_floating;

	char   residual[JARVIS_MAX_RESIDUAL_WORDS][JARVIS_ID_LEN];
	size_t residual_count;

	char   lead_verb_span[JARVIS_ID_LEN];
} parsed_clause_t;

static double get_verb_weight(const parsed_clause_t *p, const char *concept)
{
	for (size_t i = 0; i < p->verb_count; i++) {
		if (strcmp(p->verbs[i].concept, concept) == 0) {
			return p->verbs[i].weight;
		}
	}
	return 0.0;
}

static void set_verb_max(parsed_clause_t *p, const char *concept, double w)
{
	for (size_t i = 0; i < p->verb_count; i++) {
		if (strcmp(p->verbs[i].concept, concept) == 0) {
			if (w > p->verbs[i].weight) p->verbs[i].weight = w;
			return;
		}
	}
	if (p->verb_count < JARVIS_MAX_PARSED_VERBS) {
		snprintf(p->verbs[p->verb_count].concept, sizeof(p->verbs[0].concept), "%s", concept);
		p->verbs[p->verb_count].weight = w;
		p->verb_count++;
	}
}

static bool has_entity_type(const parsed_clause_t *p, const char *etype)
{
	for (size_t i = 0; i < p->entity_count; i++) {
		if (strcmp(p->entities[i].type, etype) == 0 && p->entities[i].id_count > 0) {
			return true;
		}
	}
	return false;
}

static const char *get_first_entity_id(const parsed_clause_t *p, const char *etype, const char *def)
{
	for (size_t i = 0; i < p->entity_count; i++) {
		if (strcmp(p->entities[i].type, etype) == 0 && p->entities[i].id_count > 0) {
			return p->entities[i].ids[0];
		}
	}
	return def;
}

static bool entity_has_id(const parsed_clause_t *p, const char *etype, const char *id)
{
	for (size_t i = 0; i < p->entity_count; i++) {
		if (strcmp(p->entities[i].type, etype) == 0) {
			for (size_t j = 0; j < p->entities[i].id_count; j++) {
				if (strcmp(p->entities[i].ids[j], id) == 0) return true;
			}
		}
	}
	return false;
}

static void add_entity_match(parsed_clause_t *p, const char *etype, const char *eid)
{
	for (size_t i = 0; i < p->entity_count; i++) {
		if (strcmp(p->entities[i].type, etype) == 0) {
			for (size_t j = 0; j < p->entities[i].id_count; j++) {
				if (strcmp(p->entities[i].ids[j], eid) == 0) return;
			}
			if (p->entities[i].id_count < JARVIS_MAX_PARSED_ENT_IDS) {
				char *slot = p->entities[i].ids[p->entities[i].id_count++];
				snprintf(slot, sizeof(p->entities[i].ids[0]), "%s", eid);
			}
			return;
		}
	}
	if (p->entity_count < JARVIS_MAX_PARSED_ENTITIES) {
		size_t idx = p->entity_count++;
		snprintf(p->entities[idx].type, sizeof(p->entities[idx].type), "%s", etype);
		snprintf(p->entities[idx].ids[0], sizeof(p->entities[idx].ids[0]), "%s", eid);
		p->entities[idx].id_count = 1;
	}
}

static void join_words(char words[][JARVIS_ID_LEN], size_t start, size_t len, char *out, size_t out_sz)
{
	out[0] = '\0';
	size_t pos = 0;
	for (size_t i = 0; i < len; i++) {
		pos += (size_t)snprintf(out + pos, pos < out_sz ? out_sz - pos : 0, "%s%s", i ? " " : "", words[start + i]);
	}
}

static void extract_constituents(
	const jarvis_ontology_t   *ont,
	const char                *clause,
	const jarvis_live_index_t *live_idx,
	parsed_clause_t           *out_p
)
{
	memset(out_p, 0, sizeof(*out_p));
	char clean[JARVIS_CLAUSE_LEN];
	jarvis_clean_phrase(clause, clean, sizeof(clean));

	char words[JARVIS_MAX_CLAUSE_WORDS][JARVIS_ID_LEN];
	size_t n = 0;
	char *save = NULL;
	for (char *t = strtok_r(clean, " ", &save); t && n < JARVIS_MAX_CLAUSE_WORDS; t = strtok_r(NULL, " ", &save)) {
		snprintf(words[n++], sizeof(words[0]), "%s", t);
	}
	if (n == 0) return;

	bool consumed[JARVIS_MAX_CLAUSE_WORDS] = {false};

	/* Skip leading polite stopwords configured in ontology */
	size_t v_start = 0;
	while (v_start < n && jarvis_ontology_is_stopword(ont, words[v_start])) {
		bool found_lead_vp = false;
		size_t max_p = (n - v_start < 4) ? (n - v_start) : 4;
		for (size_t pl = max_p; pl >= 1; pl--) {
			char probe[128];
			join_words(words, v_start, pl, probe, sizeof(probe));
			if (jarvis_ontology_find_verb_phrase(ont, probe)) {
				found_lead_vp = true;
				break;
			}
		}
		if (found_lead_vp) break;
		v_start++;
	}

	size_t lead_verb_end = 0;
	/* 1. Tag leading Verb phrase first */
	size_t max_l1 = (n - v_start < 6) ? (n - v_start) : 6;
	for (size_t L = max_l1; L >= 1; L--) {
		char span[128];
		join_words(words, v_start, L, span, sizeof(span));
		const jarvis_verb_entry_t *ve = jarvis_ontology_find_verb_phrase(ont, span);
		if (ve) {
			for (size_t t = 0; t < ve->tag_count; t++) {
				set_verb_max(out_p, ve->tags[t].concept, ve->tags[t].weight);
			}
			snprintf(out_p->lead_verb_span, sizeof(out_p->lead_verb_span), "%.*s", (int)(sizeof(out_p->lead_verb_span) - 1), span);
			lead_verb_end = v_start + L;
			if (!jarvis_live_index_find(live_idx, span)) {
				for (size_t k = v_start; k < v_start + L; k++) consumed[k] = true;
			}
			break;
		}
	}

	/* 2. Tag longest matching Entity spans (up to 6 words) across the clause */
	size_t max_l2 = (n < 6) ? n : 6;
	for (size_t L = max_l2; L >= 1; L--) {
		for (size_t i = 0; i + L <= n; i++) {
			char span[128];
			join_words(words, i, L, span, sizeof(span));
			const jarvis_entity_entry_t *ee = jarvis_live_index_find(live_idx, span);
			if (!ee) continue;

			/* Do not consume a single-word mod.flag-only entity (like "new") so it doesn't shadow multi-word phrases,
			 * unless it also carries non-modifier entity tags (like "fullscreen" -> ui.window_action + mod.flag). */
			if (ee->tag_count == 1 && strcmp(ee->tags[0].type, "mod.flag") == 0) {
				add_entity_match(out_p, ee->tags[0].type, ee->tags[0].id);
				continue;
			}

			bool any_cons = false;
			for (size_t k = i; k < i + L; k++) {
				if (consumed[k]) { any_cons = true; break; }
			}
			if (any_cons) {
				bool allow_subsume = (i == v_start && L > (lead_verb_end - v_start));
				if (allow_subsume) {
					for (size_t k = lead_verb_end; k < i + L; k++) {
						if (consumed[k]) { allow_subsume = false; break; }
					}
				} else if (i >= v_start && i + L <= lead_verb_end) {
					/* Entity phrase is a sub-span inside the leading verb phrase (e.g. "fullscreen" inside "toggle fullscreen") */
					allow_subsume = true;
				}
				if (!allow_subsume) continue;
			}

			for (size_t t = 0; t < ee->tag_count; t++) {
				add_entity_match(out_p, ee->tags[t].type, ee->tags[t].id);
			}
			for (size_t k = i; k < i + L; k++) consumed[k] = true;
		}
	}

	/* 3. Tag secondary / interior Verb spans if not yet tagged */
	size_t max_l3 = (n < 5) ? n : 5;
	for (size_t L = max_l3; L >= 1; L--) {
		for (size_t i = 0; i + L <= n; i++) {
			bool any_cons = false;
			for (size_t k = i; k < i + L; k++) {
				if (consumed[k]) { any_cons = true; break; }
			}
			if (any_cons) continue;

			char span[128];
			join_words(words, i, L, span, sizeof(span));
			const jarvis_verb_entry_t *ve = jarvis_ontology_find_verb_phrase(ont, span);
			if (ve) {
				/* If a verb phrase is also a known web.shortcut host name after a leading verb, keep it as argument */
				if (i > v_start && out_p->verb_count > 0 && ve->tag_count == 1 &&
				    strcmp(ve->tags[0].concept, "SEARCH_WEB") == 0) {
					continue;
				}
				for (size_t t = 0; t < ve->tag_count; t++) {
					set_verb_max(out_p, ve->tags[t].concept, ve->tags[t].weight);
				}
				for (size_t k = i; k < i + L; k++) consumed[k] = true;
			}
		}
	}

	/* 4. Modifiers (from mod.flag ontology entities) and implicit verb fallback */
	out_p->mod_new        = entity_has_id(out_p, "mod.flag", "new");
	out_p->mod_hidden     = entity_has_id(out_p, "mod.flag", "hidden");
	out_p->mod_all        = entity_has_id(out_p, "mod.flag", "all");
	out_p->mod_silent     = entity_has_id(out_p, "mod.flag", "silent");
	out_p->mod_fullscreen = entity_has_id(out_p, "mod.flag", "fullscreen");
	out_p->mod_floating   = entity_has_id(out_p, "mod.flag", "floating");

	for (size_t i = 0; i < n; i++) {
		if (!consumed[i] && out_p->verb_count == 0) {
			const jarvis_verb_entry_t *ve = jarvis_ontology_find_verb_phrase(ont, words[i]);
			if (ve) {
				for (size_t t = 0; t < ve->tag_count; t++) {
					set_verb_max(out_p, ve->tags[t].concept, ve->tags[t].weight);
				}
				consumed[i] = true;
			}
		}
	}

	/* Promote self-contained entity phrases that carry inherent verb semantics */
	if (has_entity_type(out_p, "media.object")) {
		const char *m0 = get_first_entity_id(out_p, "media.object", "");
		if (strcmp(m0, "next_track") == 0 || strcmp(m0, "prev_track") == 0 ||
		    strcmp(m0, "volume_up") == 0 || strcmp(m0, "volume_down") == 0 ||
		    strcmp(m0, "volume_mute") == 0) {
			set_verb_max(out_p, "MEDIA_ADJUST", 0.95);
		}
	}
	if (has_entity_type(out_p, "ui.window_action") && get_verb_weight(out_p, "OPEN_LAUNCH") < 0.90) {
		set_verb_max(out_p, "WINDOW_CONTROL", 0.95);
	}

	if (out_p->verb_count == 0 && out_p->entity_count > 0) {
		if (has_entity_type(out_p, "ui.workspace")) {
			set_verb_max(out_p, "WORKSPACE_SWITCH", 0.92);
		} else if (has_entity_type(out_p, "ui.window_action")) {
			set_verb_max(out_p, "WINDOW_CONTROL", 0.92);
		} else if (has_entity_type(out_p, "shell.program") ||
		           has_entity_type(out_p, "ui.container_kind") ||
		           has_entity_type(out_p, "gui.application") ||
		           has_entity_type(out_p, "web.shortcut")) {
			set_verb_max(out_p, "OPEN_LAUNCH", 0.90);
		} else if (has_entity_type(out_p, "git.object") ||
		           has_entity_type(out_p, "fs.collection") ||
		           has_entity_type(out_p, "fs.metric")) {
			set_verb_max(out_p, "PRESENT", 0.90);
		} else if (has_entity_type(out_p, "ui.relative_window") ||
		           has_entity_type(out_p, "ui.window")) {
			set_verb_max(out_p, "FOCUS_NAV", 0.90);
		} else if (has_entity_type(out_p, "fs.location")) {
			set_verb_max(out_p, "FOCUS_NAV", 0.88);
		}
	}

	for (size_t i = 0; i < n && out_p->residual_count < JARVIS_MAX_RESIDUAL_WORDS; i++) {
		if (!consumed[i] &&
		    !jarvis_ontology_is_stopword(ont, words[i]) &&
		    !jarvis_ontology_is_deictic_pronoun(ont, words[i])) {
			char *slot = out_p->residual[out_p->residual_count++];
			snprintf(slot, sizeof(out_p->residual[0]), "%.*s",
			         (int)(sizeof(out_p->residual[0]) - 1), words[i]);
		}
	}
}

static bool starts_with_folded_phrase(const char *s, const char *phrase, size_t *out_advance)
{
	char p_clean[128];
	jarvis_clean_phrase(phrase, p_clean, sizeof(p_clean));
	if (!p_clean[0]) return false;

	char s_clean[512];
	jarvis_clean_phrase(s, s_clean, sizeof(s_clean));
	size_t plen = strlen(p_clean);
	if (strncmp(s_clean, p_clean, plen) != 0 ||
	    (s_clean[plen] != '\0' && s_clean[plen] != ' ')) {
		return false;
	}

	/* Count words in p_clean and advance that many words in raw string s */
	size_t n_words = 0;
	for (size_t i = 0; p_clean[i]; i++) {
		if (i == 0 || (p_clean[i] != ' ' && p_clean[i - 1] == ' ')) n_words++;
	}
	const char *cur = s;
	while (n_words > 0 && *cur) {
		while (*cur && (isspace((unsigned char)*cur) || strchr(",.!?;:", *cur))) cur++;
		while (*cur && !isspace((unsigned char)*cur)) cur++;
		n_words--;
	}
	while (*cur && isspace((unsigned char)*cur)) cur++;
	*out_advance = (size_t)(cur - s);
	return true;
}

static void extract_after_verb(
	const jarvis_ontology_t *ont,
	const char              *raw_stripped,
	const char              *lead_verb_span,
	bool                     strip_arg_preps,
	char                    *out,
	size_t                   out_sz
)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!raw_stripped) return;

	const char *q = raw_stripped;
	while (*q && isspace((unsigned char)*q)) q++;

	/* Strip polite leading phrases configured in ontology */
	if (ont) {
		bool stripped_p = true;
		while (stripped_p) {
			stripped_p = false;
			for (size_t i = 0; i < ont->polite_prefix_count; i++) {
				size_t adv = 0;
				if (starts_with_folded_phrase(q, ont->polite_prefixes[i], &adv) && adv > 0) {
					q += adv;
					stripped_p = true;
					break;
				}
			}
		}
	}

	/* Strip leading verb phrase (SVO/VSO) or trailing verb phrase (SOV) */
	bool stripped_lead_verb = false;
	if (lead_verb_span && *lead_verb_span) {
		size_t adv = 0;
		if (starts_with_folded_phrase(q, lead_verb_span, &adv)) {
			q += adv;
			stripped_lead_verb = true;
		}
	} else if (ont) {
		size_t best_adv = 0;
		for (size_t i = 0; i < ont->verb_phrase_count; i++) {
			size_t adv = 0;
			if (starts_with_folded_phrase(q, ont->verb_phrases[i].phrase, &adv) && adv > best_adv) {
				best_adv = adv;
			}
		}
		if (best_adv > 0) {
			q += best_adv;
			stripped_lead_verb = true;
		}
	}

	/* Strip optional prepositions from ont->arg_preps */
	if (strip_arg_preps && ont) {
		bool changed = true;
		while (changed) {
			changed = false;
			for (size_t i = 0; i < ont->arg_prep_count; i++) {
				size_t adv = 0;
				if (starts_with_folded_phrase(q, ont->arg_preps[i], &adv) && adv > 0) {
					q += adv;
					changed = true;
					break;
				}
			}
		}
	}

	snprintf(out, out_sz, "%s", q);
	size_t olen = strlen(out);
	while (olen > 0 && isspace((unsigned char)out[olen - 1])) {
		out[--olen] = '\0';
	}

	/* SOV support: if no leading verb was stripped, subtract trailing verb phrase (and trailing polite/adposition particles) */
	if (!stripped_lead_verb && ont && out[0]) {
		bool sov_changed = true;
		while (sov_changed) {
			sov_changed = false;
			for (size_t i = 0; i < ont->polite_prefix_count; i++) {
				if (ends_with_ci_space(out, ont->polite_prefixes[i])) {
					strip_tail_words(out, ont->polite_prefixes[i]);
					sov_changed = true;
					break;
				}
			}
		}
		size_t best_vlen = 0;
		const char *best_vp = NULL;
		for (size_t i = 0; i < ont->verb_phrase_count; i++) {
			const char *vp = ont->verb_phrases[i].phrase;
			size_t vlen = strlen(vp);
			if (vlen > best_vlen && ends_with_ci_space(out, vp)) {
				best_vlen = vlen;
				best_vp = vp;
			}
		}
		if (best_vp) {
			strip_tail_words(out, best_vp);
		}
		if (strip_arg_preps) {
			bool tail_changed = true;
			while (tail_changed) {
				tail_changed = false;
				for (size_t i = 0; i < ont->arg_prep_count; i++) {
					if (ends_with_ci_space(out, ont->arg_preps[i])) {
						strip_tail_words(out, ont->arg_preps[i]);
						tail_changed = true;
						break;
					}
				}
				for (size_t i = 0; i < ont->stopword_count; i++) {
					if (ont->stopwords[i][0] && ends_with_ci_space(out, ont->stopwords[i])) {
						strip_tail_words(out, ont->stopwords[i]);
						tail_changed = true;
						break;
					}
				}
			}
		}
	}
}

static void strip_generic_media_object_words(const jarvis_ontology_t *ont, char *q)
{
	if (!ont || !q || !*q) return;
	bool changed = true;
	while (changed && q[0]) {
		changed = false;
		char q_clean[512];
		jarvis_clean_phrase(q, q_clean, sizeof(q_clean));
		for (size_t i = 0; i < ont->entity_phrase_count; i++) {
			const jarvis_entity_entry_t *ee = &ont->entity_phrases[i];
			for (size_t t = 0; t < ee->tag_count; t++) {
				if (strcmp(ee->tags[t].type, "media.object") == 0 &&
				    strcmp(ee->tags[t].id, "music") == 0) {
					if (strcmp(q_clean, ee->phrase) == 0) {
						q[0] = '\0';
						return;
					}
					if (ends_with_ci_space(q, ee->phrase)) {
						strip_tail_words(q, ee->phrase);
						changed = true;
						break;
					}
					size_t adv = 0;
					if (starts_with_folded_phrase(q, ee->phrase, &adv) && adv > 0) {
						memmove(q, q + adv, strlen(q + adv) + 1);
						changed = true;
						break;
					}
				}
			}
			if (changed) break;
		}
		for (size_t i = 0; i < ont->stopword_count && q[0]; i++) {
			if (ont->stopwords[i][0] && ends_with_ci_space(q, ont->stopwords[i])) {
				strip_tail_words(q, ont->stopwords[i]);
				changed = true;
				break;
			}
		}
	}
}

static int parse_workspace_digit(const jarvis_ontology_t *ont, const char *s)
{
	if (!s || !*s) return 0;
	if (s[0] >= '1' && s[0] <= '9' && s[1] == '\0') return s[0] - '0';
	if (ont) {
		for (size_t i = 0; i < ont->entity_phrase_count; i++) {
			if (strcmp(ont->entity_phrases[i].phrase, s) == 0) {
				for (size_t t = 0; t < ont->entity_phrases[i].tag_count; t++) {
					if (strcmp(ont->entity_phrases[i].tags[t].type, "ui.workspace") == 0 ||
					    strcmp(ont->entity_phrases[i].tags[t].type, "sys.number") == 0) {
						int v = atoi(ont->entity_phrases[i].tags[t].id);
						if (v >= 1 && v <= 9) return v;
					}
				}
			}
		}
	}
	return 0;
}

static bool is_digit_1_to_9(const jarvis_ontology_t *ont, const char *s)
{
	return parse_workspace_digit(ont, s) >= 1;
}

static bool is_all_digits(const char *s)
{
	if (!s || !*s) return false;
	for (const char *p = s; *p; p++) {
		if (!isdigit((unsigned char)*p)) return false;
	}
	return true;
}

static bool detect_url_token(const char *stripped, char *out_url, size_t url_sz)
{
	static const char *TLDS[] = {".com", ".org", ".net", ".io", ".dev", ".ai", ".edu", ".gov", ".pt", NULL};
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", stripped);
	char *save = NULL;
	for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		if (strncmp(tok, "http://", 7) == 0 || strncmp(tok, "https://", 8) == 0) {
			snprintf(out_url, url_sz, "%s", tok);
			return true;
		}
		for (size_t i = 0; TLDS[i]; i++) {
			const char *hit = strstr(tok, TLDS[i]);
			if (hit && hit > tok) {
				char after = hit[strlen(TLDS[i])];
				if (after == '\0' || after == '/') {
					snprintf(out_url, url_sz, "%s", tok);
					return true;
				}
			}
		}
	}
	return false;
}

static void assign_ir_slot(jarvis_ir_t *ir, const char *slot_name, const char *val)
{
	if (!ir || !slot_name || !val) return;
	if (strcmp(slot_name, "action") == 0 || strcmp(slot_name, "state") == 0) {
		snprintf(ir->action, sizeof(ir->action), "%s", val);
	} else if (strcmp(slot_name, "command") == 0 || strcmp(slot_name, "program") == 0) {
		snprintf(ir->program, sizeof(ir->program), "%s", val);
	} else if (strcmp(slot_name, "identifier") == 0) {
		snprintf(ir->identifier, sizeof(ir->identifier), "%s", val);
		snprintf(ir->application, sizeof(ir->application), "%s", val);
	} else if (strcmp(slot_name, "application") == 0) {
		snprintf(ir->application, sizeof(ir->application), "%s", val);
	} else if (strcmp(slot_name, "url") == 0) {
		snprintf(ir->url, sizeof(ir->url), "%s", val);
	} else if (strcmp(slot_name, "query") == 0 || strcmp(slot_name, "target_query") == 0) {
		snprintf(ir->query, sizeof(ir->query), "%s", val);
	} else if (strcmp(slot_name, "text") == 0 || strcmp(slot_name, "key_combo") == 0) {
		snprintf(ir->text, sizeof(ir->text), "%s", val);
	} else if (strcmp(slot_name, "kind") == 0) {
		snprintf(ir->kind, sizeof(ir->kind), "%s", val);
	} else if (strcmp(slot_name, "relation") == 0) {
		snprintf(ir->relation, sizeof(ir->relation), "%s", val);
	} else if (strcmp(slot_name, "direction") == 0) {
		snprintf(ir->direction, sizeof(ir->direction), "%s", val);
	} else if (strcmp(slot_name, "workspace") == 0) {
		ir->workspace = atoi(val);
	} else if (strcmp(slot_name, "amount") == 0 || strcmp(slot_name, "button") == 0 || strcmp(slot_name, "seconds") == 0) {
		ir->amount = atoi(val);
	} else if (strcmp(slot_name, "mode") == 0 || strcmp(slot_name, "record_target") == 0) {
		snprintf(ir->record_target, sizeof(ir->record_target), "%s", val);
	} else if (strcmp(slot_name, "location") == 0) {
		snprintf(ir->location, sizeof(ir->location), "%s", val);
	} else if (strcmp(slot_name, "aspect") == 0) {
		snprintf(ir->aspect, sizeof(ir->aspect), "%s", val);
	} else if (strcmp(slot_name, "metric") == 0) {
		snprintf(ir->metric, sizeof(ir->metric), "%s", val);
	} else if (strcmp(slot_name, "pattern") == 0) {
		snprintf(ir->pattern, sizeof(ir->pattern), "%s", val);
	} else if (strcmp(slot_name, "unit") == 0) {
		snprintf(ir->unit, sizeof(ir->unit), "%s", val);
	} else {
		/* Arbitrary custom slot name -> store in ir->query */
		snprintf(ir->query, sizeof(ir->query), "%s", val);
	}
}

static const jarvis_capability_t *find_ont_capability(const jarvis_ontology_t *ont, const char *cid)
{
	if (!ont || !cid) return NULL;
	for (size_t i = 0; i < ont->capability_count; i++) {
		if (strcmp(ont->capabilities[i].id, cid) == 0) {
			return &ont->capabilities[i];
		}
	}
	return NULL;
}

static bool build_intent_ir(
	const jarvis_ontology_t *ont,
	const char              *cid,
	double                   score,
	const parsed_clause_t   *p,
	const char              *raw_stripped,
	const char              *eff_surface,
	bool                     explicit_term,
	bool                     target_other,
	bool                     submit_enter,
	jarvis_ir_t             *out_ir
)
{
	memset(out_ir, 0, sizeof(*out_ir));
	snprintf(out_ir->cap, sizeof(out_ir->cap), "%s", cid);
	out_ir->score             = score;
	out_ir->explicit_terminal = explicit_term;
	out_ir->target_other      = target_other;
	out_ir->submit_enter      = submit_enter;
	snprintf(out_ir->surface, sizeof(out_ir->surface), "%s", eff_surface);

	/* 1. If capability defines declarative slot rules in ontology.json, evaluate them directly */
	const jarvis_capability_t *cap_def = find_ont_capability(ont, cid);
	if (cap_def && (cap_def->slot_rule_count > 0 || cap_def->compile.op[0])) {
		char open_val[512] = {0};
		extract_after_verb(ont, raw_stripped, p->lead_verb_span, true, open_val, sizeof(open_val));
		if (strcmp(cid, "media.play") == 0) {
			strip_generic_media_object_words(ont, open_val);
		}
		for (size_t i = 0; i < cap_def->slot_rule_count; i++) {
			const jarvis_cap_slot_rule_t *sr = &cap_def->slot_rules[i];
			const char *resolved = sr->default_val;
			if (strcmp(sr->source, "literal") == 0) {
				resolved = sr->default_val;
			} else if (strcmp(sr->source, "open") == 0 || strcmp(sr->source, "after_verb") == 0 || strcmp(sr->source, "arg") == 0) {
				resolved = open_val[0] ? open_val : sr->default_val;
			} else if (strcmp(sr->source, "entity_id") == 0) {
				for (size_t ti = 0; ti < sr->type_count; ti++) {
					if (has_entity_type(p, sr->types[ti])) {
						resolved = get_first_entity_id(p, sr->types[ti], sr->default_val);
						break;
					}
				}
			} else if (strcmp(sr->source, "entity_id_or_open") == 0) {
				bool found_ent = false;
				for (size_t ti = 0; ti < sr->type_count; ti++) {
					if (has_entity_type(p, sr->types[ti])) {
						resolved = get_first_entity_id(p, sr->types[ti], "");
						found_ent = (resolved && resolved[0]);
						if (found_ent) break;
					}
				}
				if (!found_ent) {
					resolved = open_val[0] ? open_val : sr->default_val;
				}
			}
			assign_ir_slot(out_ir, sr->slot_name, resolved ? resolved : "");
		}
		return true;
	}

	const char *loc = get_first_entity_id(p, "fs.location", ".");

	if (strcmp(cid, "fs.list") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "terminal");
		snprintf(out_ir->location, sizeof(out_ir->location), "%s", loc);
		snprintf(out_ir->collection, sizeof(out_ir->collection), "%s", get_first_entity_id(p, "fs.collection", "files"));
		out_ir->hidden = true;
		return true;
	}
	if (strcmp(cid, "fs.metric") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "terminal");
		snprintf(out_ir->metric, sizeof(out_ir->metric), "%s", get_first_entity_id(p, "fs.metric", "cwd"));
		snprintf(out_ir->location, sizeof(out_ir->location), "%s", loc);
		return true;
	}
	if (strcmp(cid, "fs.find") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "terminal");
		snprintf(out_ir->pattern, sizeof(out_ir->pattern), "%s", get_first_entity_id(p, "fs.file_type", "*"));
		snprintf(out_ir->location, sizeof(out_ir->location), "%s", loc);
		return true;
	}
	if (strcmp(cid, "fs.navigate") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "terminal");
		snprintf(out_ir->location, sizeof(out_ir->location), "%s", get_first_entity_id(p, "fs.location", "~"));
		return true;
	}
	if (strcmp(cid, "git.inspect") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "terminal");
		snprintf(out_ir->aspect, sizeof(out_ir->aspect), "%s", get_first_entity_id(p, "git.object", "status"));
		snprintf(out_ir->location, sizeof(out_ir->location), "%s", loc);
		return true;
	}
	if (strcmp(cid, "shell.run") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "terminal");
		snprintf(out_ir->program, sizeof(out_ir->program), "%s", get_first_entity_id(p, "shell.program", ""));
		return true;
	}
	if (strcmp(cid, "edit.clear") == 0) {
		snprintf(out_ir->unit, sizeof(out_ir->unit), "%s", get_first_entity_id(p, "edit.unit", "line"));
		return true;
	}
	if (strcmp(cid, "window.new") == 0) {
		const char *kind = get_first_entity_id(p, "ui.container_kind", "");
		if ((strcmp(kind, "window") == 0 || strcmp(kind, "tab") == 0) &&
		    !p->mod_new && get_verb_weight(p, "OPEN_LAUNCH") <= 0.0) {
			return false;
		}
		snprintf(out_ir->kind, sizeof(out_ir->kind), "%s", kind);
		return true;
	}
	if (strcmp(cid, "window.focus") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		snprintf(out_ir->identifier, sizeof(out_ir->identifier), "%s", get_first_entity_id(p, "ui.window", ""));
		return true;
	}
	if (strcmp(cid, "window.focus_relative") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		snprintf(out_ir->relation, sizeof(out_ir->relation), "%s", get_first_entity_id(p, "ui.relative_window", "other"));
		return true;
	}
	if (strcmp(cid, "workspace.switch") == 0 || strcmp(cid, "workspace.move_window") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		int ws = 1;
		if (has_entity_type(p, "ui.workspace")) {
			ws = atoi(get_first_entity_id(p, "ui.workspace", "1"));
		} else {
			for (size_t i = 0; i < p->residual_count; i++) {
				int v = parse_workspace_digit(ont, p->residual[i]);
				if (v >= 1 && v <= 9) {
					ws = v;
					break;
				}
			}
		}
		if (ws < 1 || ws > 9) ws = 1;
		out_ir->workspace = ws;
		return true;
	}
	if (strcmp(cid, "window.control") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		const char *action = get_first_entity_id(p, "ui.window_action", "unknown");
		if (strcmp(action, "unknown") == 0) {
			if (p->mod_fullscreen) action = "fullscreen";
			else if (p->mod_floating) action = "float";
		}
		if (strcmp(action, "unknown") == 0) return false;
		snprintf(out_ir->action, sizeof(out_ir->action), "%s", action);
		bool is_tab = entity_has_id(p, "ui.container_kind", "tab");
		snprintf(out_ir->kind, sizeof(out_ir->kind), "%s", is_tab ? "tab" : "window");
		return true;
	}
	if (strcmp(cid, "ui.scroll") == 0) {
		out_ir->explicit_terminal = false;
		const char *dir = get_first_entity_id(p, "ui.scroll_direction", "down");
		snprintf(out_ir->direction, sizeof(out_ir->direction), "%s", dir);
		int amt = 5;
		bool found_amt = false;
		for (size_t i = 0; i < p->residual_count; i++) {
			if (is_all_digits(p->residual[i])) {
				amt = atoi(p->residual[i]);
				found_amt = true;
			}
		}
		if (!found_amt && has_entity_type(p, "ui.workspace")) {
			int wv = atoi(get_first_entity_id(p, "ui.workspace", "5"));
			if (wv >= 1) amt = wv;
		}
		if (amt < 1) amt = 1;
		if (amt > 20) amt = 20;
		out_ir->amount = amt;
		return true;
	}
	if (strcmp(cid, "input.dictate") == 0) {
		char q_clean[512];
		extract_after_verb(ont, raw_stripped, p->lead_verb_span, false, q_clean, sizeof(q_clean));
		if (ont) {
			for (size_t i = 0; i < ont->loc_submit_count; i++) {
				if (ends_with_ci_space(q_clean, ont->loc_submit[i])) {
					strip_tail_words(q_clean, ont->loc_submit[i]);
					out_ir->submit_enter = true;
					break;
				}
			}
		}
		if (!q_clean[0] && !out_ir->submit_enter) return false;
		snprintf(out_ir->text, sizeof(out_ir->text), "%s", q_clean);
		return true;
	}
	if (strcmp(cid, "agent.ask") == 0) {
		char q_clean[512];
		extract_after_verb(ont, raw_stripped, p->lead_verb_span, true, q_clean, sizeof(q_clean));
		if (!q_clean[0]) return false;
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		snprintf(out_ir->query, sizeof(out_ir->query), "%s", q_clean);
		return true;
	}
	if (strcmp(cid, "web.open") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		snprintf(out_ir->url, sizeof(out_ir->url), "%s", get_first_entity_id(p, "web.shortcut", ""));
		return out_ir->url[0] != '\0';
	}
	if (strcmp(cid, "app.launch") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		snprintf(out_ir->application, sizeof(out_ir->application), "%s", get_first_entity_id(p, "gui.application", ""));
		return out_ir->application[0] != '\0';
	}
	if (strcmp(cid, "record.control") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		snprintf(out_ir->record_target, sizeof(out_ir->record_target), "%s", get_first_entity_id(p, "record.object", "both"));
		snprintf(out_ir->action, sizeof(out_ir->action), "%s", get_verb_weight(p, "PAUSE_STOP") > 0.0 ? "stop" : "start");
		return true;
	}
	if (strcmp(cid, "web.search") == 0) {
		char q_clean[512];
		extract_after_verb(ont, raw_stripped, p->lead_verb_span, true, q_clean, sizeof(q_clean));
		if (!q_clean[0]) return false;
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		snprintf(out_ir->query, sizeof(out_ir->query), "%s", q_clean);
		return true;
	}
	if (strcmp(cid, "media.control") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		const char *m0 = get_first_entity_id(p, "media.object", "");
		if (get_verb_weight(p, "PAUSE_STOP") > 0.0 && m0[0] && p->residual_count == 0) {
			snprintf(out_ir->action, sizeof(out_ir->action), "pause");
			return true;
		}
		if (strcmp(m0, "next_track") == 0 || strcmp(m0, "prev_track") == 0 ||
		    strcmp(m0, "volume_up") == 0 || strcmp(m0, "volume_down") == 0 ||
		    strcmp(m0, "volume_mute") == 0) {
			snprintf(out_ir->action, sizeof(out_ir->action), "%s", m0);
			return true;
		}
		return false;
	}
	if (strcmp(cid, "media.play") == 0) {
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		out_ir->explicit_terminal = false;
		const char *m0 = get_first_entity_id(p, "media.object", "");
		if (get_verb_weight(p, "PAUSE_STOP") > 0.0 && strcmp(m0, "music") == 0 && p->residual_count == 0) {
			snprintf(out_ir->cap, sizeof(out_ir->cap), "media.control");
			snprintf(out_ir->action, sizeof(out_ir->action), "pause");
			return true;
		}
		if ((strcmp(m0, "next_track") == 0 || strcmp(m0, "prev_track") == 0 ||
		     strcmp(m0, "volume_up") == 0 || strcmp(m0, "volume_down") == 0 ||
		     strcmp(m0, "volume_mute") == 0) && p->residual_count == 0) {
			snprintf(out_ir->cap, sizeof(out_ir->cap), "media.control");
			snprintf(out_ir->action, sizeof(out_ir->action), "%s", m0);
			return true;
		}
		char q_clean[512];
		extract_after_verb(ont, raw_stripped, p->lead_verb_span, true, q_clean, sizeof(q_clean));
		strip_generic_media_object_words(ont, q_clean);
		if (!q_clean[0]) {
			if (has_entity_type(p, "media.object") || get_verb_weight(p, "PLAY_STREAM") >= 0.90) {
				if (m0[0] && strcmp(m0, "music") != 0) {
					snprintf(q_clean, sizeof(q_clean), "%s", m0);
				} else {
					q_clean[0] = '\0';
				}
			} else {
				return false;
			}
		}
		if (out_ir->score > 0.96) out_ir->score = 0.96;
		snprintf(out_ir->query, sizeof(out_ir->query), "%s", q_clean);
		return true;
	}

	return false;
}

static bool padded_contains_phrase(const char *padded_hay, const char *phrase)
{
	if (!phrase || !*phrase) return false;
	char probe[160];
	snprintf(probe, sizeof(probe), " %s ", phrase);
	return strstr(padded_hay, probe) != NULL;
}

bool jarvis_solve_clause(
	const jarvis_ontology_t *ont,
	const char              *raw_clause,
	const char              *active_surface,
	const jarvis_world_t    *world,
	jarvis_ir_t             *out_ir
)
{
	if (!ont || !raw_clause || !out_ir) return false;
	memset(out_ir, 0, sizeof(*out_ir));

	char norm_clause[512];
	jarvis_ontology_normalize_asr(ont, world, raw_clause, norm_clause, sizeof(norm_clause));
	if (!norm_clause[0]) return false;

	char stripped[512], eff_surface[32];
	bool explicit_term = false, target_other = false, submit_enter = false;
	bool new_terminal = false, bg_play = false;
	strip_locative_modifiers(
		ont, norm_clause, active_surface,
		stripped, sizeof(stripped),
		eff_surface, sizeof(eff_surface),
		&explicit_term, &target_other, &submit_enter,
		&new_terminal, &bg_play
	);

	jarvis_live_index_t *live_idx = calloc(1, sizeof(*live_idx));
	if (!live_idx) return false;
	jarvis_ontology_build_live_index(ont, world, live_idx);

	parsed_clause_t p;
	extract_constituents(ont, stripped, live_idx, &p);
	free(live_idx);

	if (get_verb_weight(&p, "HELP_ASK") > 0.0) {
		return false;
	}

	/* Extract raw words without polite prefixes */
	char raw_clean[512];
	jarvis_clean_phrase(raw_clause, raw_clean, sizeof(raw_clean));
	char *raw_words[64];
	size_t n_raw = 0;
	char *save = NULL;
	for (char *t = strtok_r(raw_clean, " ", &save); t && n_raw < 64; t = strtok_r(NULL, " ", &save)) {
		raw_words[n_raw++] = t;
	}
	size_t rw_idx = 0;
	while (rw_idx < n_raw && jarvis_ontology_is_polite_prefix(ont, raw_words[rw_idx])) {
		rw_idx++;
	}

	/* Fast-path Tier-0 voice interruption */
	char raw_core[JARVIS_CLAUSE_LEN] = {0};
	size_t rc_pos = 0;
	for (size_t i = rw_idx; i < n_raw; i++) {
		rc_pos += (size_t)snprintf(raw_core + rc_pos, rc_pos < sizeof(raw_core) ? sizeof(raw_core) - rc_pos : 0, "%s%s", i > rw_idx ? " " : "", raw_words[i]);
	}
	if (jarvis_ontology_is_tts_interrupt(ont, raw_core)) {
		snprintf(out_ir->cap, sizeof(out_ir->cap), "tts.interrupt");
		out_ir->score = 0.99;
		snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
		return true;
	}

	/* Declarative Tier-0 Casual, Ergonomic & Slang Idiom Fast-Path (< 0.1ms deterministic resolution) */
	if (!(rw_idx < n_raw && jarvis_ontology_is_interrogative(ont, raw_words[rw_idx]))) {
		char padded_raw[528];
		snprintf(padded_raw, sizeof(padded_raw), " %s ", raw_core);
		for (size_t ii = 0; ii < ont->idiom_count; ii++) {
			const jarvis_idiom_t *idm = &ont->idioms[ii];
			bool phrase_ok = false;
			for (size_t pi = 0; pi < idm->phrase_count; pi++) {
				if (padded_contains_phrase(padded_raw, idm->phrases[pi])) {
					phrase_ok = true;
					break;
				}
			}
			if (!phrase_ok) continue;

			if (idm->require_any_count > 0) {
				bool req_ok = false;
				for (size_t ri = 0; ri < idm->require_any_count; ri++) {
					if (padded_contains_phrase(padded_raw, idm->require_any[ri])) {
						req_ok = true;
						break;
					}
				}
				if (!req_ok) continue;
			}

			if (idm->forbid_any_count > 0) {
				bool fbd_hit = false;
				for (size_t fi = 0; fi < idm->forbid_any_count; fi++) {
					if (padded_contains_phrase(padded_raw, idm->forbid_any[fi])) {
						fbd_hit = true;
						break;
					}
				}
				if (fbd_hit) continue;
			}

			snprintf(out_ir->cap, sizeof(out_ir->cap), "%s", idm->cap);
			if (idm->action[0]) snprintf(out_ir->action, sizeof(out_ir->action), "%s", idm->action);
			if (idm->text[0]) {
				snprintf(out_ir->text, sizeof(out_ir->text), "%s", idm->text);
				if (strcmp(idm->cap, "media.play") == 0 || strcmp(idm->cap, "web.search") == 0 || strcmp(idm->cap, "agent.ask") == 0) {
					snprintf(out_ir->query, sizeof(out_ir->query), "%s", idm->text);
				} else if (strcmp(idm->cap, "app.launch") == 0) {
					snprintf(out_ir->application, sizeof(out_ir->application), "%s", idm->text);
				} else if (strcmp(idm->cap, "shell.run") == 0) {
					snprintf(out_ir->program, sizeof(out_ir->program), "%s", idm->text);
				}
			}
			if (idm->relation[0]) snprintf(out_ir->relation, sizeof(out_ir->relation), "%s", idm->relation);
			if (strcmp(idm->kind, "auto_tab_window") == 0) {
				bool is_tab = entity_has_id(&p, "ui.container_kind", "tab");
				snprintf(out_ir->kind, sizeof(out_ir->kind), "%s", is_tab ? "tab" : "window");
			} else if (idm->kind[0]) {
				snprintf(out_ir->kind, sizeof(out_ir->kind), "%s", idm->kind);
			}
			if (strcmp(idm->cap, "workspace.move_window") == 0 || strcmp(idm->cap, "workspace.switch") == 0) {
				int ws = 1;
				if (has_entity_type(&p, "ui.workspace")) {
					ws = atoi(get_first_entity_id(&p, "ui.workspace", "1"));
				} else {
					for (size_t k = rw_idx; k < n_raw; k++) {
						int v = parse_workspace_digit(ont, raw_words[k]);
						if (v >= 1 && v <= 9) { ws = v; break; }
					}
				}
				out_ir->workspace = ws;
			}
			out_ir->target_other = target_other || entity_has_id(&p, "ui.relative_window", "other");
			out_ir->score = idm->score > 0.0 ? idm->score : 0.96;
			if (strcmp(idm->surface, "inherit") == 0) {
				snprintf(out_ir->surface, sizeof(out_ir->surface), "%s", eff_surface);
			} else {
				snprintf(out_ir->surface, sizeof(out_ir->surface), "%s", idm->surface[0] ? idm->surface : "any");
			}
			return true;
		}
	}

	/* Guard against interrogative questions unless fully covered fs/git inspection */
	if (rw_idx < n_raw && jarvis_ontology_is_interrogative(ont, raw_words[rw_idx])) {
		if (!(p.residual_count == 0 &&
		      (has_entity_type(&p, "fs.metric") ||
		       has_entity_type(&p, "fs.collection") ||
		       has_entity_type(&p, "git.object")))) {
			return false;
		}
	}

	/* Direct URL pattern in clause ("open https://...", "open github.com/torvalds") */
	char url_val[512] = {0};
	if (p.lead_verb_span[0] &&
	    (get_verb_weight(&p, "OPEN_LAUNCH") > 0.0 || get_verb_weight(&p, "FOCUS_NAV") > 0.0) &&
	    detect_url_token(stripped, url_val, sizeof(url_val))) {
		char url_clean[512];
		jarvis_clean_phrase(url_val, url_clean, sizeof(url_clean));
		char padded_url[520];
		snprintf(padded_url, sizeof(padded_url), " %s ", url_clean);

		bool has_non_url_res = false;
		for (size_t i = 0; i < p.residual_count; i++) {
			char probe[80];
			snprintf(probe, sizeof(probe), " %s ", p.residual[i]);
			if (!strstr(padded_url, probe)) {
				has_non_url_res = true;
				break;
			}
		}
		if (!has_non_url_res &&
		    get_verb_weight(&p, "PRESENT") <= 0.0 &&
		    !has_entity_type(&p, "shell.program") &&
		    get_verb_weight(&p, "SEARCH_WEB") <= 0.0) {
			snprintf(out_ir->cap, sizeof(out_ir->cap), "web.open");
			out_ir->score = 0.96;
			snprintf(out_ir->surface, sizeof(out_ir->surface), "any");
			out_ir->target_other = target_other;
			out_ir->submit_enter = submit_enter;
			snprintf(out_ir->url, sizeof(out_ir->url), "%s", url_val);
			return true;
		}
	}

	/* Explicit dictation when clause begins with a dictation verb */
	bool is_explicit_dictation = (rw_idx < n_raw && jarvis_ontology_is_dictation_verb(ont, raw_words[rw_idx]));
	if (is_explicit_dictation && !has_entity_type(&p, "edit.unit") && get_verb_weight(&p, "AGENT_ASK") <= 0.0) {
		if (build_intent_ir(ont, "input.dictate", 0.96, &p, stripped, eff_surface, explicit_term, target_other, submit_enter, out_ir)) {
			out_ir->new_terminal = new_terminal;
			return true;
		}
	}

	/* Explicit coding agent delegation ("ask pi to ...", "tell the agent to ...") */
	if (get_verb_weight(&p, "AGENT_ASK") > 0.0) {
		if (build_intent_ir(ont, "agent.ask", 0.96, &p, stripped, eff_surface, explicit_term, target_other, submit_enter, out_ir)) {
			return true;
		}
	}

	const jarvis_capability_t *best_cap = NULL;
	double best_score = 0.0;

	for (size_t ci = 0; ci < ont->capability_count; ci++) {
		const jarvis_capability_t *cap = &ont->capabilities[ci];
		const char *cid = cap->id;

		double v_score = 0.0;
		for (size_t vi = 0; vi < cap->verb_count; vi++) {
			double w = get_verb_weight(&p, cap->verbs[vi]);
			if (w > v_score) v_score = w;
		}
		if (v_score <= 0.0) continue;

		/* Forbidden entities check */
		bool blocked = false;
		for (size_t fi = 0; fi < cap->forbid_ent_count; fi++) {
			const char *fe = cap->forbid_entities[fi];
			bool in_req = false;
			for (size_t ri = 0; ri < cap->req_ent_count; ri++) {
				if (strcmp(cap->required_entities[ri], fe) == 0) { in_req = true; break; }
			}
			if (in_req) continue;
			if (has_entity_type(&p, fe)) {
				if (strcmp(fe, "ui.container_kind") == 0 ||
				    strcmp(fe, "gui.application") == 0 ||
				    strcmp(fe, "ui.window") == 0 ||
				    strcmp(fe, "fs.location") == 0) {
					bool overlap_ok = (strcmp(cid, "window.control") == 0 ||
					                   strcmp(cid, "media.play") == 0 ||
					                   strcmp(cid, "media.control") == 0 ||
					                   (strcmp(fe, "ui.container_kind") == 0 &&
					                    entity_has_id(&p, "ui.container_kind", "terminal") &&
					                    (strcmp(cid, "shell.run") == 0 ||
					                     strcmp(cid, "fs.list") == 0 ||
					                     strcmp(cid, "fs.navigate") == 0 ||
					                     strcmp(cid, "fs.find") == 0 ||
					                     strcmp(cid, "fs.metric") == 0 ||
					                     strcmp(cid, "git.inspect") == 0)));
					for (size_t ri = 0; ri < cap->req_ent_count && !overlap_ok; ri++) {
						const char *re_t = cap->required_entities[ri];
						if (strcmp(re_t, "ui.container_kind") == 0 ||
						    strcmp(re_t, "gui.application") == 0 ||
						    strcmp(re_t, "ui.window") == 0 ||
						    strcmp(re_t, "ui.window_action") == 0 ||
						    strcmp(re_t, "media.object") == 0) {
							overlap_ok = true;
						}
					}
					if (overlap_ok) {
						if (strcmp(fe, "fs.location") == 0 && !has_entity_type(&p, "media.object")) {
							blocked = true;
							break;
						}
						continue;
					}
				}
				blocked = true;
				break;
			}
		}
		if (blocked) continue;

		/* Required entities check */
		bool all_req = true;
		for (size_t ri = 0; ri < cap->req_ent_count; ri++) {
			if (!has_entity_type(&p, cap->required_entities[ri])) {
				all_req = false;
				break;
			}
		}
		if (cap->req_ent_count > 0 && !all_req) {
			bool has_digit_res = false;
			for (size_t i = 0; i < p.residual_count; i++) {
				if (is_digit_1_to_9(ont, p.residual[i])) { has_digit_res = true; break; }
			}
			if (strcmp(cid, "fs.list") == 0 && has_entity_type(&p, "fs.location") &&
			    get_verb_weight(&p, "PRESENT") > 0.0 && get_verb_weight(&p, "FOCUS_NAV") <= 0.0) {
				/* allowed */
			} else if (strcmp(cid, "workspace.switch") == 0 &&
			           get_verb_weight(&p, "WORKSPACE_SWITCH") > 0.0 && has_digit_res) {
				/* allowed */
			} else if (strcmp(cid, "workspace.move_window") == 0 &&
			           get_verb_weight(&p, "WORKSPACE_MOVE") > 0.0 && has_digit_res) {
				/* allowed */
			} else if (strcmp(cid, "ui.scroll") == 0 && get_verb_weight(&p, "SCROLL_NAV") > 0.0) {
				/* allowed */
			} else {
				continue;
			}
		}

		/* Required_any check */
		if (cap->req_any_count > 0) {
			bool ok_any = false;
			for (size_t ai = 0; ai < cap->req_any_count; ai++) {
				const char *ra = cap->required_any[ai];
				if (strcmp(ra, "media.object") == 0 && has_entity_type(&p, "media.object")) {
					ok_any = true;
				}
				if (strcmp(ra, "residual_with_play_verb") == 0 &&
				    (p.residual_count > 0 || has_entity_type(&p, "media.object") ||
				     get_verb_weight(&p, "PLAY_STREAM") >= 0.90) &&
				    get_verb_weight(&p, "PLAY_STREAM") >= 0.80) {
					ok_any = true;
				}
			}
			if (!ok_any) continue;
		}

		/* Content-token coverage invariant */
		if (cap->forbid_residual && p.residual_count > 0) {
			bool all_d19 = true;
			bool all_d = true;
			for (size_t i = 0; i < p.residual_count; i++) {
				if (!is_digit_1_to_9(ont, p.residual[i])) all_d19 = false;
				if (!is_all_digits(p.residual[i])) all_d = false;
			}
			if ((strcmp(cid, "workspace.switch") == 0 || strcmp(cid, "workspace.move_window") == 0) && all_d19) {
				/* allowed */
			} else if (strcmp(cid, "ui.scroll") == 0 && all_d) {
				/* allowed */
			} else {
				continue;
			}
		}

		if (strcmp(cid, "input.dictate") == 0 && !is_explicit_dictation) {
			continue;
		}

		double ctx_bonus = (strcmp(cap->surface, "any") == 0 || strcmp(cap->surface, eff_surface) == 0 || explicit_term) ? 0.04 : 0.0;
		double req_bonus = (cap->req_ent_count > 0 || cap->req_any_count > 0) ? 0.03 : 0.0;
		bool is_open_arg = (cap->open_arg[0] != '\0' ||
		                    strcmp(cid, "media.play") == 0 ||
		                    strcmp(cid, "web.search") == 0 ||
		                    strcmp(cid, "input.dictate") == 0 ||
		                    strcmp(cid, "agent.ask") == 0);
		size_t eff_res = (is_open_arg ||
		                  (strcmp(cid, "ui.scroll") == 0 && p.residual_count == 1 && is_all_digits(p.residual[0])))
			? 0 : p.residual_count;
		double res_penalty = 0.18 * (double)eff_res;
		double lead_penalty = (p.lead_verb_span[0] || p.verb_count == 0 ||
		                       has_entity_type(&p, "ui.window_action") ||
		                       has_entity_type(&p, "media.object")) ? 0.0 : 0.14;
		double score = 0.55 * v_score + 0.41 + ctx_bonus + req_bonus - res_penalty - lead_penalty;
		if (score > 0.99) score = 0.99;
		if (score < 0.0) score = 0.0;

		/* Disambiguation rules */
		if (strcmp(cid, "app.launch") == 0 && get_verb_weight(&p, "OPEN_LAUNCH") > 0.0 && get_verb_weight(&p, "FOCUS_NAV") <= 0.0) {
			score = 0.99;
		}
		if (has_entity_type(&p, "ui.container_kind") && entity_has_id(&p, "ui.container_kind", "terminal") &&
		    (strcmp(cid, "shell.run") == 0 || strcmp(cid, "fs.list") == 0 ||
		     strcmp(cid, "fs.navigate") == 0 || strcmp(cid, "fs.find") == 0 ||
		     strcmp(cid, "fs.metric") == 0 || strcmp(cid, "git.inspect") == 0)) {
			new_terminal = true;
			explicit_term = true;
			snprintf(eff_surface, sizeof(eff_surface), "terminal");
		}
		if (strcmp(cid, "window.focus") == 0 && get_verb_weight(&p, "OPEN_LAUNCH") > 0.0 && has_entity_type(&p, "gui.application")) {
			continue;
		}
		if (strcmp(cid, "fs.navigate") == 0 && has_entity_type(&p, "fs.location")) {
			if (get_verb_weight(&p, "FOCUS_NAV") >= 0.90 && !has_entity_type(&p, "fs.collection")) {
				score = 0.99;
			} else if (get_verb_weight(&p, "PRESENT") > 0.0 && get_verb_weight(&p, "OPEN_LAUNCH") <= 0.0) {
				continue;
			}
		}
		if (strcmp(cid, "workspace.move_window") == 0 && get_verb_weight(&p, "WORKSPACE_MOVE") >= 0.80) {
			score = 0.99;
		}
		if (strcmp(cid, "workspace.switch") == 0 && get_verb_weight(&p, "WORKSPACE_MOVE") >= 0.80) {
			continue;
		}
		if (strcmp(cid, "workspace.switch") == 0 &&
		    (get_verb_weight(&p, "WORKSPACE_SWITCH") >= 0.80 || has_entity_type(&p, "ui.workspace"))) {
			score = 0.99;
		}
		if (strcmp(cid, "window.control") == 0 &&
		    (has_entity_type(&p, "ui.window_action") || get_verb_weight(&p, "WINDOW_CONTROL") >= 0.80)) {
			score = 0.99;
		}
		if (strcmp(cid, "media.control") == 0) {
			const char *m0 = get_first_entity_id(&p, "media.object", "");
			if (get_verb_weight(&p, "PAUSE_STOP") > 0.0 && p.residual_count == 0) {
				score = 0.99;
			} else if ((strcmp(m0, "next_track") == 0 || strcmp(m0, "prev_track") == 0 ||
			            strcmp(m0, "volume_up") == 0 || strcmp(m0, "volume_down") == 0 ||
			            strcmp(m0, "volume_mute") == 0) && p.residual_count == 0) {
				score = 0.99;
			} else {
				continue;
			}
		}

		if (score > best_score) {
			best_score = score;
			best_cap = cap;
		}
	}

	if (!best_cap || best_score < 0.80) {
		return false;
	}

	if (strcmp(best_cap->id, "window.focus_relative") != 0) {
		if (entity_has_id(&p, "ui.relative_window", "other")) {
			target_other = true;
		}
	}

	if (!build_intent_ir(ont, best_cap->id, best_score, &p, stripped, eff_surface, explicit_term, target_other, submit_enter, out_ir)) {
		return false;
	}
	out_ir->target_other = target_other;
	out_ir->new_terminal = new_terminal;
	if (bg_play && strcmp(out_ir->cap, "media.play") == 0) {
		out_ir->background = true;
	}
	return true;
}

static const char *lookup_ir_slot_str(const jarvis_ir_t *ir, const char *slot_name, char *num_buf, size_t num_sz)
{
	if (!ir || !slot_name) return "";
	if (strcmp(slot_name, "action") == 0 || strcmp(slot_name, "state") == 0) return ir->action;
	if (strcmp(slot_name, "command") == 0 || strcmp(slot_name, "program") == 0) return ir->program;
	if (strcmp(slot_name, "identifier") == 0) return ir->identifier[0] ? ir->identifier : ir->application;
	if (strcmp(slot_name, "application") == 0) return ir->application;
	if (strcmp(slot_name, "url") == 0) return ir->url;
	if (strcmp(slot_name, "query") == 0 || strcmp(slot_name, "target_query") == 0) return ir->query;
	if (strcmp(slot_name, "text") == 0 || strcmp(slot_name, "key_combo") == 0) return ir->text;
	if (strcmp(slot_name, "kind") == 0) return ir->kind;
	if (strcmp(slot_name, "relation") == 0) return ir->relation;
	if (strcmp(slot_name, "direction") == 0) return ir->direction;
	if (strcmp(slot_name, "mode") == 0 || strcmp(slot_name, "record_target") == 0) return ir->record_target;
	if (strcmp(slot_name, "location") == 0) return ir->location;
	if (strcmp(slot_name, "aspect") == 0) return ir->aspect;
	if (strcmp(slot_name, "metric") == 0) return ir->metric;
	if (strcmp(slot_name, "pattern") == 0) return ir->pattern;
	if (strcmp(slot_name, "unit") == 0) return ir->unit;
	if (strcmp(slot_name, "workspace") == 0) {
		snprintf(num_buf, num_sz, "%d", ir->workspace > 0 ? ir->workspace : 1);
		return num_buf;
	}
	if (strcmp(slot_name, "amount") == 0 || strcmp(slot_name, "button") == 0 || strcmp(slot_name, "seconds") == 0) {
		snprintf(num_buf, num_sz, "%d", ir->amount > 0 ? ir->amount : 1);
		return num_buf;
	}
	return ir->query;
}

static void expand_ir_template(const jarvis_ir_t *ir, const char *tpl, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!tpl) return;
	size_t w = 0;
	for (size_t i = 0; tpl[i] && w + 1 < out_sz; ) {
		if (tpl[i] == '$' && tpl[i + 1] == '{') {
			const char *end = strchr(tpl + i + 2, '}');
			if (end) {
				char sname[64] = {0}, num_buf[32] = {0};
				size_t slen = (size_t)(end - (tpl + i + 2));
				if (slen >= sizeof(sname)) slen = sizeof(sname) - 1;
				memcpy(sname, tpl + i + 2, slen);
				sname[slen] = '\0';
				const char *val = lookup_ir_slot_str(ir, sname, num_buf, sizeof(num_buf));
				w += (size_t)snprintf(out + w, out_sz - w, "%s", val ? val : "");
				i = (size_t)(end - tpl) + 1;
				continue;
			}
		}
		out[w++] = tpl[i++];
	}
	out[w] = '\0';
}

static void assign_dsl_field(jarvis_dsl_t *dsl, const char *field, const char *val)
{
	if (!dsl || !field || !val) return;
	if (strcmp(field, "target.command") == 0) snprintf(dsl->target.command, sizeof(dsl->target.command), "%s", val);
	else if (strcmp(field, "target.identifier") == 0) snprintf(dsl->target.identifier, sizeof(dsl->target.identifier), "%s", val);
	else if (strcmp(field, "target.kind") == 0) snprintf(dsl->target.kind, sizeof(dsl->target.kind), "%s", val);
	else if (strcmp(field, "target.relation") == 0) snprintf(dsl->target.relation, sizeof(dsl->target.relation), "%s", val);
	else if (strcmp(field, "target.url") == 0) snprintf(dsl->target.url, sizeof(dsl->target.url), "%s", val);
	else if (strcmp(field, "target.query") == 0) snprintf(dsl->target.query, sizeof(dsl->target.query), "%s", val);
	else if (strcmp(field, "args.action") == 0) snprintf(dsl->args.action, sizeof(dsl->args.action), "%s", val);
	else if (strcmp(field, "args.text") == 0) snprintf(dsl->args.text, sizeof(dsl->args.text), "%s", val);
	else if (strcmp(field, "args.key_combo") == 0) snprintf(dsl->args.key_combo, sizeof(dsl->args.key_combo), "%s", val);
	else if (strcmp(field, "args.direction") == 0) snprintf(dsl->args.direction, sizeof(dsl->args.direction), "%s", val);
	else if (strcmp(field, "args.mode") == 0) snprintf(dsl->args.mode, sizeof(dsl->args.mode), "%s", val);
	else if (strcmp(field, "args.workspace") == 0) dsl->args.workspace = atoi(val);
	else if (strcmp(field, "args.amount") == 0) dsl->args.amount = atoi(val);
	else if (strcmp(field, "args.button") == 0) dsl->args.button = atoi(val);
	else if (strcmp(field, "args.seconds") == 0) dsl->args.seconds = atof(val);
}

bool jarvis_compile_ir_to_dsl_ex(
	const jarvis_ontology_t *ont,
	const jarvis_ir_t       *ir,
	const char              *active_surface,
	const jarvis_context_t  *ctx,
	const jarvis_world_t    *world,
	jarvis_dsl_t            *out_dsl
)
{
	if (!ir || !out_dsl) return false;
	if (!ont) ont = jarvis_ontology_get_active();

	const jarvis_capability_t *cap_def = find_ont_capability(ont, ir->cap);
	if (cap_def && cap_def->compile.op[0]) {
		memset(out_dsl, 0, sizeof(*out_dsl));
		const char *op = cap_def->compile.op;
		if (cap_def->compile.op_if_slot[0]) {
			char nbuf[32] = {0};
			const char *sval = lookup_ir_slot_str(ir, cap_def->compile.op_if_slot, nbuf, sizeof(nbuf));
			if (sval && strcmp(sval, cap_def->compile.op_if_equals) == 0) {
				if (cap_def->compile.op_then[0]) op = cap_def->compile.op_then;
			} else if (cap_def->compile.op_else[0]) {
				op = cap_def->compile.op_else;
			}
		}
		snprintf(out_dsl->op, sizeof(out_dsl->op), "%s", op);
		for (size_t i = 0; i < cap_def->compile.field_count; i++) {
			char expanded[512];
			expand_ir_template(ir, cap_def->compile.fields[i].tpl, expanded, sizeof(expanded));
			assign_dsl_field(out_dsl, cap_def->compile.fields[i].field, expanded);
		}
		if (ir->target_other) snprintf(out_dsl->target.relation, sizeof(out_dsl->target.relation), "other");
		if (ir->new_terminal) out_dsl->target.new_terminal = true;
		if (ir->background) out_dsl->args.background = true;
		return true;
	}

	return jarvis_compile_ir_to_dsl(ir, active_surface, ctx, world, out_dsl);
}

bool jarvis_compile_ir_to_dsl(
	const jarvis_ir_t      *ir,
	const char             *active_surface,
	const jarvis_context_t *ctx,
	const jarvis_world_t   *world,
	jarvis_dsl_t           *out_dsl
)
{
	if (!ir || !out_dsl) return false;
	const jarvis_ontology_t *ont = jarvis_ontology_get_active();
	const jarvis_capability_t *cap_def = find_ont_capability(ont, ir->cap);
	if (cap_def && cap_def->compile.op[0]) {
		return jarvis_compile_ir_to_dsl_ex(ont, ir, active_surface, ctx, world, out_dsl);
	}
	memset(out_dsl, 0, sizeof(*out_dsl));

	const char *cap = ir->cap;
	const char *surf = (active_surface && *active_surface) ? active_surface : "any";

	#define APPLY_OTHER_AND_NEW_TERM() do { \
		if (ir->target_other) snprintf(out_dsl->target.relation, sizeof(out_dsl->target.relation), "other"); \
		if (ir->new_terminal) out_dsl->target.new_terminal = true; \
	} while (0)

	if (strcmp(cap, "fs.list") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "spawn_shell_command");
		const char *loc = ir->location[0] ? ir->location : ".";
		if (strcmp(loc, ".") == 0) snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "ls -la");
		else snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "ls -la %s", loc);
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "fs.navigate") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "spawn_shell_command");
		snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "cd %s", ir->location[0] ? ir->location : "~");
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "fs.find") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "spawn_shell_command");
		const char *loc = ir->location[0] ? ir->location : ".";
		const char *pat = ir->pattern[0] ? ir->pattern : "*";
		snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "find %s -name '%s'", loc, pat);
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "fs.metric") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "spawn_shell_command");
		const char *m = ir->metric;
		const char *cmd = "pwd";
		if (strcmp(m, "disk_space") == 0) cmd = "df -h";
		else if (strcmp(m, "largest") == 0) cmd = "du -sh * 2>/dev/null | sort -h";
		else if (strcmp(m, "recent") == 0) cmd = "ls -lat | head -n 20";
		snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "%s", cmd);
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "git.inspect") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "spawn_shell_command");
		const char *a = ir->aspect;
		const char *cmd = "git status";
		if (strcmp(a, "diff") == 0) cmd = "git diff";
		else if (strcmp(a, "log") == 0) cmd = "git log -n 10 --oneline";
		else if (strcmp(a, "branch") == 0) cmd = "git branch -a";
		snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "%s", cmd);
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "shell.run") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "spawn_shell_command");
		const char *prog = ir->program;
		if (strcmp(prog, "tmux_attach") == 0) snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "tmux a || tmux");
		else snprintf(out_dsl->target.command, sizeof(out_dsl->target.command), "%s", prog);
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "app.launch") == 0) {
		if (strcmp(ir->application, "terminal") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "open_terminal");
		} else {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "open_application");
			snprintf(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), "%s", ir->application);
		}
		return true;
	}
	if (strcmp(cap, "web.open") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "open_url");
		snprintf(out_dsl->target.url, sizeof(out_dsl->target.url), "%s", ir->url);
		return true;
	}
	if (strcmp(cap, "window.new") == 0) {
		const char *k = ir->kind;
		if (strcmp(k, "terminal") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "open_terminal");
			return true;
		}
		if (strcmp(k, "tab") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "press_key");
			snprintf(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), "ctrl+t");
			APPLY_OTHER_AND_NEW_TERM();
			return true;
		}
		if (strcmp(k, "window") == 0) {
			if (strcmp(surf, "browser") == 0) {
				snprintf(out_dsl->op, sizeof(out_dsl->op), "press_key");
				snprintf(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), "ctrl+n");
				APPLY_OTHER_AND_NEW_TERM();
			} else {
				snprintf(out_dsl->op, sizeof(out_dsl->op), "open_terminal");
			}
			return true;
		}
		if (strcmp(k, "devtools") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "press_key");
			snprintf(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), "ctrl+shift+i");
			APPLY_OTHER_AND_NEW_TERM();
			return true;
		}
		if (strcmp(k, "address_bar") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "press_key");
			snprintf(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), "ctrl+l");
			APPLY_OTHER_AND_NEW_TERM();
			return true;
		}
		if (k[0]) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "open_application");
			snprintf(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), "%s", k);
			return true;
		}
		return false;
	}
	if (strcmp(cap, "window.focus") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "focus_window");
		snprintf(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), "%s", ir->identifier);
		return true;
	}
	if (strcmp(cap, "window.focus_relative") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "focus_relative_window");
		snprintf(out_dsl->target.kind, sizeof(out_dsl->target.kind), "relative");
		snprintf(out_dsl->target.relation, sizeof(out_dsl->target.relation), "%s", ir->relation[0] ? ir->relation : "other");
		return true;
	}
	if (strcmp(cap, "workspace.switch") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "workspace_switch");
		out_dsl->args.workspace = ir->workspace > 0 ? ir->workspace : 1;
		return true;
	}
	if (strcmp(cap, "workspace.move_window") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "workspace_move_window");
		out_dsl->args.workspace = ir->workspace > 0 ? ir->workspace : 1;
		if (ir->target_other) {
			snprintf(out_dsl->target.kind, sizeof(out_dsl->target.kind), "relative");
			snprintf(out_dsl->target.relation, sizeof(out_dsl->target.relation), "other");
		} else {
			const char *tid = (ctx && ctx->last_window_id[0]) ? ctx->last_window_id :
			                  ((world && world->focused.id[0]) ? world->focused.id : "");
			if (tid[0]) snprintf(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), "%s", tid);
		}
		return true;
	}
	if (strcmp(cap, "window.control") == 0) {
		const char *act = ir->action;
		if (strcmp(act, "float") == 0 || strcmp(act, "tile") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "window_float");
		} else if (strcmp(act, "close") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "window_close");
			snprintf(out_dsl->target.kind, sizeof(out_dsl->target.kind), "%.*s", (int)(sizeof(out_dsl->target.kind) - 1), ir->kind[0] ? ir->kind : "window");
		} else {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "window_fullscreen");
		}
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "ui.scroll") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "scroll");
		snprintf(out_dsl->args.direction, sizeof(out_dsl->args.direction), "%s", ir->direction[0] ? ir->direction : "down");
		out_dsl->args.amount = ir->amount > 0 ? ir->amount : 5;
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "edit.clear") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "press_key");
		const char *u = ir->unit;
		const char *kc = "ctrl+u";
		if (strcmp(surf, "browser") == 0) {
			if (strcmp(u, "word") == 0) kc = "ctrl+BackSpace";
			else kc = "ctrl+a BackSpace";
		} else {
			if (strcmp(u, "word") == 0) kc = "ctrl+w";
			else if (strcmp(u, "screen") == 0) kc = "ctrl+l";
			else if (strcmp(u, "process") == 0) kc = "ctrl+c";
		}
		snprintf(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), "%s", kc);
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "input.key") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "press_key");
		snprintf(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), "%.*s",
		         (int)(sizeof(out_dsl->args.key_combo) - 1),
		         ir->text[0] ? ir->text : "Return");
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "input.dictate") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "%s", ir->submit_enter ? "type_and_enter" : "type_text");
		snprintf(out_dsl->args.text, sizeof(out_dsl->args.text), "%s", ir->text);
		APPLY_OTHER_AND_NEW_TERM();
		return true;
	}
	if (strcmp(cap, "agent.ask") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "ask_agent");
		snprintf(out_dsl->target.query, sizeof(out_dsl->target.query), "%s", ir->query);
		return true;
	}
	if (strcmp(cap, "media.play") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "media_play");
		snprintf(out_dsl->target.query, sizeof(out_dsl->target.query), "%s", ir->query);
		out_dsl->verify.has_audio_playing = true;
		out_dsl->verify.audio_playing = true;
		out_dsl->args.background = ir->background;
		return true;
	}
	if (strcmp(cap, "tts.interrupt") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "tts_interrupt");
		return true;
	}
	if (strcmp(cap, "tts.control") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "tts_control");
		snprintf(out_dsl->args.action, sizeof(out_dsl->args.action), "%s", ir->action[0] ? ir->action : "on");
		return true;
	}
	if (strcmp(cap, "media.control") == 0) {
		const char *act = ir->action;
		if (strcmp(act, "next_track") == 0) snprintf(out_dsl->op, sizeof(out_dsl->op), "media_next");
		else if (strcmp(act, "prev_track") == 0) snprintf(out_dsl->op, sizeof(out_dsl->op), "media_prev");
		else if (strcmp(act, "volume_up") == 0 || strcmp(act, "volume_down") == 0 || strcmp(act, "volume_mute") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "media_volume");
			snprintf(out_dsl->args.action, sizeof(out_dsl->args.action), "%s", act);
		} else {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "media_pause");
		}
		return true;
	}
	if (strcmp(cap, "record.control") == 0) {
		if (strcmp(ir->action, "stop") == 0) {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "record_stop");
		} else {
			snprintf(out_dsl->op, sizeof(out_dsl->op), "record_start");
			snprintf(out_dsl->args.mode, sizeof(out_dsl->args.mode), "%s", ir->record_target[0] ? ir->record_target : "both");
		}
		return true;
	}
	if (strcmp(cap, "web.search") == 0) {
		snprintf(out_dsl->op, sizeof(out_dsl->op), "search_web");
		snprintf(out_dsl->target.query, sizeof(out_dsl->target.query), "%s", ir->query);
		return true;
	}

	#undef APPLY_OTHER_AND_NEW_TERM
	return false;
}
