#include <ttypt/iso.h>

#include "../../src/jarvis-json.c"
#include "../../src/jarvis-ontology.c"
#include "../../src/jarvis-solver.c"
#include "../../src/jarvis-voice.c"
#include "../../src/libjarvis.c"

ISO_EXPORT(size_t, jarvis_wasm_solve_json, const char *utterance, const char *surface, char *out_json, size_t out_sz)
{
	static jarvis_ontology_t *g_ont = NULL;
	if (!g_ont) {
		g_ont = jarvis_ontology_create(NULL);
	}
	if (!g_ont || !utterance || !out_json || out_sz == 0) {
		return 0;
	}
	jarvis_ir_t ir;
	if (!jarvis_solve_clause(g_ont, utterance, (surface && *surface) ? surface : "any", NULL, &ir)) {
		return 0;
	}
	jarvis_dsl_t dsl;
	if (!jarvis_compile_ir_to_dsl(&ir, (surface && *surface) ? surface : "any", NULL, NULL, &dsl)) {
		return 0;
	}
	return jarvis_dsl_to_json(&dsl, out_json, out_sz);
}

ISO_EXPORT(size_t, jarvis_wasm_normalize_dsl_json, const char *raw_json, char *out_json, size_t out_sz)
{
	if (!raw_json || !out_json || out_sz == 0) {
		return 0;
	}
	jarvis_dsl_t dsl;
	if (!jarvis_normalize_dsl_json(raw_json, &dsl)) {
		return 0;
	}
	return jarvis_dsl_to_json(&dsl, out_json, out_sz);
}

ISO_EXPORT(size_t, jarvis_wasm_split_clauses_json, const char *utterance, char *out_json, size_t out_sz)
{
	static jarvis_ontology_t *g_ont = NULL;
	if (!g_ont) {
		g_ont = jarvis_ontology_create(NULL);
	}
	if (!utterance || !out_json || out_sz == 0) {
		return 0;
	}
	char clauses[8][512];
	size_t n = jarvis_split_clauses(g_ont, utterance, clauses, 8);
	size_t pos = 0;
	out_json[pos++] = '[';
	for (size_t i = 0; i < n; i++) {
		if (i > 0 && pos + 2 < out_sz) {
			out_json[pos++] = ',';
		}
		if (pos + 2 < out_sz) {
			out_json[pos++] = '"';
		}
		for (const char *p = clauses[i]; *p && pos + 4 < out_sz; p++) {
			if (*p == '"' || *p == '\\') {
				out_json[pos++] = '\\';
			}
			out_json[pos++] = *p;
		}
		if (pos + 2 < out_sz) {
			out_json[pos++] = '"';
		}
	}
	if (pos + 2 <= out_sz) {
		out_json[pos++] = ']';
		out_json[pos] = '\0';
	}
	return pos;
}

ISO_EXPORT(size_t, jarvis_wasm_extract_streaming_answer, const char *raw_fragment, char *out_ans, size_t out_sz)
{
	if (!raw_fragment || !out_ans || out_sz == 0) {
		return 0;
	}
	if (!jarvis_extract_streaming_answer_prefix(raw_fragment, out_ans, out_sz)) {
		return 0;
	}
	return strlen(out_ans);
}

ISO_EXPORT(int, jarvis_wasm_pop_tts_clause, const char *unspoken, int is_first, char *out_clause, size_t clause_sz, char *out_rem, size_t rem_sz)
{
	return jarvis_pop_ready_tts_clause(unspoken, is_first != 0, out_clause, clause_sz, out_rem, rem_sz) ? 1 : 0;
}

ISO_EXPORT(int, jarvis_wasm_is_destructive, const char *cmd)
{
	return jarvis_is_destructive_command(cmd) ? 1 : 0;
}

ISO_EXPORT(int, jarvis_wasm_is_hallucination, const char *text)
{
	return jarvis_is_non_speech_hallucination(text) ? 1 : 0;
}

ISO_EXPORT(int, jarvis_wasm_extract_wake_command, const char *transcript, const char *wake_word, char *out_cmd, size_t out_sz)
{
	return jarvis_voice_extract_wake_command(transcript, wake_word, out_cmd, out_sz) ? 1 : 0;
}

