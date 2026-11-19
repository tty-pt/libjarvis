#ifndef JARVIS_INTERNAL_H
#define JARVIS_INTERNAL_H

#include "../include/ttypt/jarvis.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(strtok_r)
#define strtok_r strtok_s
#endif

/* ---------------------------------------------------------------------------
 * JSON AST wrapper backed by json-c (host) or libbud/src/jsmn.h (WASM)
 * ------------------------------------------------------------------------- */

typedef enum {
	JJSON_NULL   = 0,
	JJSON_BOOL   = 1,
	JJSON_NUMBER = 2,
	JJSON_STRING = 3,
	JJSON_ARRAY  = 4,
	JJSON_OBJECT = 5
} jjson_type_t;

typedef struct jjson_val jjson_val_t;

typedef struct {
	char        *key;
	jjson_val_t *val;
} jjson_kv_t;

struct jjson_val {
	jjson_type_t type;
	union {
		bool   b;
		double num;
		char  *str;
		struct {
			jjson_val_t **items;
			size_t        count;
			size_t        cap;
		} arr;
		struct {
			jjson_kv_t *pairs;
			size_t      count;
			size_t      cap;
		} obj;
	} u;
};

jjson_val_t       *jjson_parse(const char *json_str);
void               jjson_free(jjson_val_t *v);
const jjson_val_t *jjson_obj_get(const jjson_val_t *obj, const char *key);
const char        *jjson_obj_get_str(const jjson_val_t *obj, const char *key, const char *def);
double             jjson_obj_get_num(const jjson_val_t *obj, const char *key, double def);
int                jjson_obj_get_int(const jjson_val_t *obj, const char *key, int def);
bool               jjson_obj_get_bool(const jjson_val_t *obj, const char *key, bool def);
size_t             jjson_escape_str(const char *src, char *dst, size_t dst_sz);
bool               jarvis_normalize_dsl_val(const jjson_val_t *raw, jarvis_dsl_t *out_dsl);

/* ---------------------------------------------------------------------------
 * Ontology Data Structures & Named Capacities
 * ------------------------------------------------------------------------- */

#define JARVIS_MAX_STOPWORDS          256
#define JARVIS_MAX_CLAUSE_SEPS        16
#define JARVIS_MAX_VERB_MAP           512
#define JARVIS_MAX_VERB_CONCEPTS      6
#define JARVIS_MAX_VERB_HEADS         256
#define JARVIS_MAX_ENTITY_MAP         1024
#define JARVIS_MAX_ENTITY_TAGS        6
#define JARVIS_MAX_VOCAB_WORDS        2048
#define JARVIS_MAX_LOCATIVES          32
#define JARVIS_MAX_CAPABILITIES       48
#define JARVIS_MAX_CAP_VERBS          6
#define JARVIS_MAX_CAP_REQ_ENTS       4
#define JARVIS_MAX_CAP_FORBID_ENTS    8
#define JARVIS_MAX_CAP_REQ_ANY        4
#define JARVIS_MAX_WAKE_VARIANTS      16
#define JARVIS_MAX_POLITE_PREFIXES    24
#define JARVIS_MAX_DICTATION_VERBS    16
#define JARVIS_MAX_DEICTIC_PRONOUNS   16
#define JARVIS_MAX_TTS_INTERRUPTS     32
#define JARVIS_MAX_LOCKDOWN_PHRASES   24
#define JARVIS_MAX_HALLUCINATIONS     48
#define JARVIS_MAX_LIVE_ENTITIES      1024
#define JARVIS_MAX_CLAUSE_WORDS       64
#define JARVIS_MAX_SPAN_WORDS         6
#define JARVIS_MAX_RESIDUAL_WORDS     32
#define JARVIS_SOLVER_TIER0_THRESH    0.85

typedef struct {
	char   concept[JARVIS_SHORT_LEN];
	double weight;
} jarvis_verb_tag_t;

typedef struct {
	char              phrase[JARVIS_ID_LEN];
	jarvis_verb_tag_t tags[JARVIS_MAX_VERB_CONCEPTS];
	size_t            tag_count;
} jarvis_verb_entry_t;

typedef struct {
	char type[JARVIS_WORD_LEN];
	char id[JARVIS_NAME_LEN];
} jarvis_entity_tag_t;

typedef struct {
	char                phrase[JARVIS_NAME_LEN];
	jarvis_entity_tag_t tags[JARVIS_MAX_ENTITY_TAGS];
	size_t              tag_count;
} jarvis_entity_entry_t;

typedef struct {
	char   id[JARVIS_WORD_LEN];
	char   verbs[JARVIS_MAX_CAP_VERBS][JARVIS_SHORT_LEN];
	size_t verb_count;
	char   required_entities[JARVIS_MAX_CAP_REQ_ENTS][JARVIS_WORD_LEN];
	size_t req_ent_count;
	char   forbid_entities[JARVIS_MAX_CAP_FORBID_ENTS][JARVIS_WORD_LEN];
	size_t forbid_ent_count;
	char   required_any[JARVIS_MAX_CAP_REQ_ANY][JARVIS_WORD_LEN];
	size_t req_any_count;
	bool   forbid_residual;
	char   surface[JARVIS_SHORT_LEN];
} jarvis_capability_t;

struct jarvis_ontology {
	char                  wake_word[JARVIS_SHORT_LEN];
	char                  wake_variants[JARVIS_MAX_WAKE_VARIANTS][JARVIS_SHORT_LEN];
	size_t                wake_variant_count;

	char                  polite_prefixes[JARVIS_MAX_POLITE_PREFIXES][JARVIS_SHORT_LEN];
	size_t                polite_prefix_count;

	char                  dictation_verbs[JARVIS_MAX_DICTATION_VERBS][JARVIS_SHORT_LEN];
	size_t                dictation_verb_count;

	char                  deictic_pronouns[JARVIS_MAX_DEICTIC_PRONOUNS][JARVIS_SHORT_LEN];
	size_t                deictic_pronoun_count;

	char                  tts_interrupts[JARVIS_MAX_TTS_INTERRUPTS][JARVIS_ID_LEN];
	size_t                tts_interrupt_count;

	char                  lockdown_phrases[JARVIS_MAX_LOCKDOWN_PHRASES][JARVIS_ID_LEN];
	size_t                lockdown_phrase_count;

	char                  hallucinations[JARVIS_MAX_HALLUCINATIONS][JARVIS_ID_LEN];
	size_t                hallucination_count;

	char                  stopwords[JARVIS_MAX_STOPWORDS][JARVIS_SHORT_LEN];
	size_t                stopword_count;

	char                  clause_seps[JARVIS_MAX_CLAUSE_SEPS][JARVIS_SHORT_LEN];
	size_t                clause_sep_count;

	jarvis_verb_entry_t   verb_phrases[JARVIS_MAX_VERB_MAP];
	size_t                verb_phrase_count;

	char                  verb_heads[JARVIS_MAX_VERB_HEADS][JARVIS_SHORT_LEN];
	size_t                verb_head_count;

	jarvis_entity_entry_t entity_phrases[JARVIS_MAX_ENTITY_MAP];
	size_t                entity_phrase_count;

	char                  vocab_words[JARVIS_MAX_VOCAB_WORDS][JARVIS_WORD_LEN];
	size_t                vocab_word_count;

	char                  loc_submit[JARVIS_MAX_LOCATIVES][JARVIS_WORD_LEN];
	size_t                loc_submit_count;
	char                  loc_other[JARVIS_MAX_LOCATIVES][JARVIS_WORD_LEN];
	size_t                loc_other_count;
	char                  loc_terminal[JARVIS_MAX_LOCATIVES][JARVIS_WORD_LEN];
	size_t                loc_terminal_count;
	char                  loc_any[JARVIS_MAX_LOCATIVES][JARVIS_WORD_LEN];
	size_t                loc_any_count;

	jarvis_capability_t   capabilities[JARVIS_MAX_CAPABILITIES];
	size_t                capability_count;
};

bool jarvis_ontology_is_stopword(const jarvis_ontology_t *ont, const char *word);
bool jarvis_ontology_is_verb_head(const jarvis_ontology_t *ont, const char *word);
bool jarvis_ontology_is_vocab_word(const jarvis_ontology_t *ont, const char *word);
bool jarvis_ontology_is_polite_prefix(const jarvis_ontology_t *ont, const char *phrase);
bool jarvis_ontology_is_dictation_verb(const jarvis_ontology_t *ont, const char *word);
bool jarvis_ontology_is_deictic_pronoun(const jarvis_ontology_t *ont, const char *word);
bool jarvis_ontology_is_tts_interrupt(const jarvis_ontology_t *ont, const char *clean_clause);
const jarvis_verb_entry_t *jarvis_ontology_find_verb_phrase(const jarvis_ontology_t *ont, const char *phrase);

/* Live entity index merging static ontology entities + dynamic host entities */
typedef struct {
	jarvis_entity_entry_t entries[JARVIS_MAX_ENTITY_MAP + JARVIS_MAX_LIVE_ENTITIES];
	size_t                count;
} jarvis_live_index_t;

void jarvis_ontology_build_live_index(
	const jarvis_ontology_t *ont,
	const jarvis_world_t    *world,
	jarvis_live_index_t     *out_idx
);

const jarvis_entity_entry_t *jarvis_live_index_find(
	const jarvis_live_index_t *idx,
	const char                *phrase
);

/* ---------------------------------------------------------------------------
 * Engine & Inference Internals
 * ------------------------------------------------------------------------- */

typedef struct {
	char         utterance[JARVIS_STR_LEN];
	jarvis_dsl_t dsl;
} jarvis_few_shot_t;

typedef struct {
	char key[JARVIS_NAME_LEN];
	char value[JARVIS_CLAUSE_LEN];
} jarvis_memory_kv_t;

typedef struct {
	double ts;
	char   utterance[JARVIS_STR_LEN];
	char   ops[JARVIS_NAME_LEN];
	char   output[JARVIS_CLAUSE_LEN];
} jarvis_session_turn_t;

#define JARVIS_EMBED_DIM       768
#define JARVIS_MAX_FEW_SHOTS   150
#define JARVIS_MAX_EMBED_CACHE 160

typedef struct {
	char  text[JARVIS_STR_LEN];
	float vec[JARVIS_EMBED_DIM];
} jarvis_embed_entry_t;

struct jarvis_engine {
	char                  wm[JARVIS_SHORT_LEN];
	char                  ontology_path[JARVIS_CLAUSE_LEN];
	char                  few_shot_path[JARVIS_CLAUSE_LEN];
	char                  context_path[JARVIS_CLAUSE_LEN];
	char                  memory_path[JARVIS_CLAUSE_LEN];
	char                  session_path[JARVIS_CLAUSE_LEN];
	char                  embed_cache_path[JARVIS_CLAUSE_LEN];
	char                  auren_url[JARVIS_STR_LEN];
	char                  llm_model[JARVIS_ID_LEN];
	jarvis_infer_mode_t   infer_mode;
	bool                  dry_run;

	jarvis_ontology_t    *ont;

	jarvis_few_shot_t     few_shots[JARVIS_MAX_FEW_SHOTS];
	size_t                few_shot_count;

	jarvis_memory_kv_t    memories[JARVIS_MAX_MEMORIES];
	size_t                memory_count;

	jarvis_session_turn_t turns[JARVIS_MAX_TURNS];
	size_t                turn_count;

	jarvis_embed_entry_t  embed_cache[JARVIS_MAX_EMBED_CACHE];
	size_t                embed_cache_count;

	jarvis_context_t      ctx;
};

/* HTTP / Auren Remote Inference Helpers */
bool jarvis_auren_classify_embedding(
	jarvis_engine_t      *eng,
	const char           *utterance,
	jarvis_dsl_t         *out_dsl
);

bool jarvis_auren_classify_llm(
	jarvis_engine_t      *eng,
	const char           *utterance,
	const jarvis_world_t *world,
	jarvis_dsl_t         *out_steps,
	size_t                max_steps,
	size_t               *out_step_count
);

bool jarvis_auren_run_react_agent(
	jarvis_engine_t         *eng,
	const char              *utterance,
	const jarvis_world_t    *world,
	jarvis_process_result_t *out_res
);

#endif /* JARVIS_INTERNAL_H */
