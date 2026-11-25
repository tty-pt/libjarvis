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
 * JSON AST wrapper backed by jsmn.h
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

/* ---------------------------------------------------------------------------
 * Ontology Data Structures & Named Capacities
 * ------------------------------------------------------------------------- */

#define JARVIS_MAX_STOPWORDS          256
#define JARVIS_MAX_CONJUNCTIONS       8
#define JARVIS_MAX_UI_STRINGS         96
#define JARVIS_MAX_REPLY_TEMPLATES    48
#define JARVIS_MAX_WAKE_VARIANTS      64
#define JARVIS_MAX_POLITE_PREFIXES    48
#define JARVIS_MAX_TTS_INTERRUPTS     64
#define JARVIS_MAX_OP_ABORTS          96
#define JARVIS_MAX_LOCKDOWN_PHRASES   48
#define JARVIS_MAX_SCREEN_LOCK_KW     16
#define JARVIS_MAX_TIER1_TRIGGERS     64
#define JARVIS_MAX_HALLUCINATIONS     64
#define JARVIS_MAX_LOCALE_ACKS        64
#define JARVIS_MAX_DICTATION_SEEDS    64
#define JARVIS_MAX_CONTINUATION_TOKENS 64

typedef struct {
	char key[JARVIS_WORD_LEN];
	char val[JARVIS_STR_LEN];
} jarvis_kv_str_t;

struct jarvis_ontology {
	char            lang[JARVIS_SHORT_LEN];
	char            lang_name[JARVIS_WORD_LEN];
	char            wake_word[JARVIS_SHORT_LEN];
	char            wake_variants[JARVIS_MAX_WAKE_VARIANTS][JARVIS_SHORT_LEN];
	size_t          wake_variant_count;

	char            whisper_wake_prompt[JARVIS_CLAUSE_LEN];
	char            whisper_cmd_prompt[JARVIS_CLAUSE_LEN];
	char            dictation_seed[JARVIS_CLAUSE_LEN];

	jarvis_kv_str_t ui_strings[JARVIS_MAX_UI_STRINGS];
	size_t          ui_string_count;

	jarvis_kv_str_t reply_templates[JARVIS_MAX_REPLY_TEMPLATES];
	size_t          reply_template_count;

	jarvis_kv_str_t locale_wake_acks[JARVIS_MAX_LOCALE_ACKS];
	size_t          locale_wake_ack_count;

	jarvis_kv_str_t locale_dictation_seeds[JARVIS_MAX_DICTATION_SEEDS];
	size_t          locale_dictation_seed_count;

	char            polite_prefixes[JARVIS_MAX_POLITE_PREFIXES][JARVIS_SHORT_LEN];
	size_t          polite_prefix_count;

	char            tts_interrupts[JARVIS_MAX_TTS_INTERRUPTS][JARVIS_ID_LEN];
	size_t          tts_interrupt_count;

	char            op_abort_phrases[JARVIS_MAX_OP_ABORTS][JARVIS_ID_LEN];
	size_t          op_abort_count;

	char            lockdown_phrases[JARVIS_MAX_LOCKDOWN_PHRASES][JARVIS_ID_LEN];
	size_t          lockdown_phrase_count;

	char            screen_lock_keywords[JARVIS_MAX_SCREEN_LOCK_KW][JARVIS_SHORT_LEN];
	size_t          screen_lock_kw_count;

	char            tier1_triggers[JARVIS_MAX_TIER1_TRIGGERS][JARVIS_SHORT_LEN];
	size_t          tier1_trigger_count;

	char            hallucinations[JARVIS_MAX_HALLUCINATIONS][JARVIS_ID_LEN];
	size_t          hallucination_count;

	char            stopwords[JARVIS_MAX_STOPWORDS][JARVIS_SHORT_LEN];
	size_t          stopword_count;

	char            conjunctions[JARVIS_MAX_CONJUNCTIONS][JARVIS_SHORT_LEN];
	size_t          conjunction_count;

	char            continuation_tokens[JARVIS_MAX_CONTINUATION_TOKENS][JARVIS_SHORT_LEN];
	size_t          continuation_token_count;
};

bool jarvis_ontology_is_stopword(const jarvis_ontology_t *ont, const char *word);
const jarvis_ontology_t *jarvis_ontology_get_active(void);

void   jarvis_phonetic_encode(const char *clean_str, char *out_phon, size_t phon_sz, char *out_skel, size_t skel_sz);
int    jarvis_damerau_levenshtein(const char *a, const char *b);
double jarvis_token_similarity(const char *a_clean, const char *b_clean);

#endif /* JARVIS_INTERNAL_H */
