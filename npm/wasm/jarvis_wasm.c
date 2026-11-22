#include <ttypt/iso.h>

#include "../../src/jarvis-json.c"
#include "../../src/jarvis-ontology.c"
#include "../../src/jarvis-voice.c"
#include "../../src/libjarvis.c"

static jarvis_ontology_t *g_ont = NULL;

ISO_EXPORT(int, jarvis_wasm_load_ontology_json, const char *json_str)
{
	if (!json_str || !*json_str) return 0;
	jarvis_ontology_t *ont = jarvis_ontology_create_from_json(json_str);
	if (!ont) return 0;
	if (g_ont) jarvis_ontology_destroy(g_ont);
	g_ont = ont;
	return 1;
}

ISO_EXPORT(size_t, jarvis_wasm_clean_phrase, const char *text, char *out, size_t out_sz)
{
	if (!text || !out || out_sz == 0) return 0;
	return jarvis_clean_phrase(text, out, out_sz);
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
