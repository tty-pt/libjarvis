#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <ttypt/jarvis.h>

typedef struct {
	jarvis_engine_t *eng;
	bool             is_open;
} JarvisEngineWrapper;

static void JarvisEngineFinalizer(napi_env env, void *finalize_data, void *finalize_hint)
{
	(void)env;
	(void)finalize_hint;
	JarvisEngineWrapper *wrap = (JarvisEngineWrapper *)finalize_data;
	if (wrap) {
		if (wrap->is_open && wrap->eng) {
			jarvis_engine_destroy(wrap->eng);
			wrap->eng = NULL;
			wrap->is_open = false;
		}
		free(wrap);
	}
}

static void get_opt_string_prop(napi_env env, napi_value obj, const char *prop, char *out, size_t out_sz)
{
	out[0] = '\0';
	bool has = false;
	if (napi_has_named_property(env, obj, prop, &has) == napi_ok && has) {
		napi_value v;
		if (napi_get_named_property(env, obj, prop, &v) == napi_ok) {
			napi_valuetype vt;
			if (napi_typeof(env, v, &vt) == napi_ok && vt == napi_string) {
				size_t len = 0;
				napi_get_value_string_utf8(env, v, out, out_sz - 1, &len);
				out[len] = '\0';
			}
		}
	}
}

/* create(configObj?) -> handle */
static napi_value JarvisCreate(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	jarvis_config_t cfg;
	jarvis_config_init(&cfg);

	char wm[64] = {0};
	char ontology_path[512] = {0};
	char few_shot_path[512] = {0};
	char context_path[512] = {0};
	char memory_path[512] = {0};
	char session_path[512] = {0};
	char embed_cache_path[512] = {0};
	char auren_url[256] = {0};
	char llm_model[128] = {0};

	if (argc >= 1) {
		napi_valuetype vt;
		if (napi_typeof(env, args[0], &vt) == napi_ok && vt == napi_object) {
			get_opt_string_prop(env, args[0], "wm", wm, sizeof(wm));
			get_opt_string_prop(env, args[0], "ontologyPath", ontology_path, sizeof(ontology_path));
			get_opt_string_prop(env, args[0], "fewShotPath", few_shot_path, sizeof(few_shot_path));
			get_opt_string_prop(env, args[0], "contextPath", context_path, sizeof(context_path));
			get_opt_string_prop(env, args[0], "memoryPath", memory_path, sizeof(memory_path));
			get_opt_string_prop(env, args[0], "sessionPath", session_path, sizeof(session_path));
			get_opt_string_prop(env, args[0], "embedCachePath", embed_cache_path, sizeof(embed_cache_path));
			get_opt_string_prop(env, args[0], "aurenUrl", auren_url, sizeof(auren_url));
			get_opt_string_prop(env, args[0], "llmModel", llm_model, sizeof(llm_model));

			if (wm[0]) cfg.wm = wm;
			if (ontology_path[0]) cfg.ontology_path = ontology_path;
			if (few_shot_path[0]) cfg.few_shot_path = few_shot_path;
			if (context_path[0]) cfg.context_path = context_path;
			if (memory_path[0]) cfg.memory_path = memory_path;
			if (session_path[0]) cfg.session_path = session_path;
			if (embed_cache_path[0]) cfg.embed_cache_path = embed_cache_path;
			if (auren_url[0]) cfg.auren_url = auren_url;
			if (llm_model[0]) cfg.llm_model = llm_model;

			bool has_dry = false;
			if (napi_has_named_property(env, args[0], "dryRun", &has_dry) == napi_ok && has_dry) {
				napi_value dv;
				napi_get_named_property(env, args[0], "dryRun", &dv);
				bool dry = false;
				if (napi_get_value_bool(env, dv, &dry) == napi_ok) {
					cfg.dry_run = dry;
				}
			}

			char infer_str[32] = {0};
			get_opt_string_prop(env, args[0], "inferMode", infer_str, sizeof(infer_str));
			if (strcmp(infer_str, "none") == 0) cfg.infer_mode = JARVIS_INFER_NONE;
			else if (strcmp(infer_str, "llm") == 0) cfg.infer_mode = JARVIS_INFER_LLM;
			else if (strcmp(infer_str, "embedding") == 0) cfg.infer_mode = JARVIS_INFER_EMBEDDING;
			else if (strcmp(infer_str, "auto") == 0) cfg.infer_mode = JARVIS_INFER_AUTO;
		}
	}

	JarvisEngineWrapper *wrap = (JarvisEngineWrapper *)calloc(1, sizeof(JarvisEngineWrapper));
	if (!wrap) {
		napi_throw_error(env, NULL, "Out of memory");
		return NULL;
	}

	wrap->eng = jarvis_engine_create(&cfg);
	if (!wrap->eng) {
		free(wrap);
		napi_throw_error(env, NULL, "Failed to create jarvis engine");
		return NULL;
	}
	wrap->is_open = true;

	napi_value res;
	napi_create_object(env, &res);
	napi_wrap(env, res, wrap, JarvisEngineFinalizer, NULL, NULL);
	return res;
}

/* destroy(handle) -> boolean */
static napi_value JarvisDestroy(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (argc >= 1) {
		JarvisEngineWrapper *wrap = NULL;
		napi_unwrap(env, args[0], (void **)&wrap);
		if (wrap && wrap->is_open && wrap->eng) {
			jarvis_engine_destroy(wrap->eng);
			wrap->eng = NULL;
			wrap->is_open = false;
		}
	}
	napi_value res;
	napi_get_boolean(env, true, &res);
	return res;
}

/* process(handle, utterance, surface?) -> jsonString */
static napi_value JarvisProcess(napi_env env, napi_callback_info info)
{
	size_t argc = 3;
	napi_value args[3];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (argc < 2) {
		napi_throw_type_error(env, NULL, "Expected (handle, utterance[, surface])");
		return NULL;
	}

	JarvisEngineWrapper *wrap = NULL;
	napi_unwrap(env, args[0], (void **)&wrap);
	if (!wrap || !wrap->is_open || !wrap->eng) {
		napi_throw_error(env, NULL, "Jarvis engine is destroyed or invalid");
		return NULL;
	}

	char utterance[1024] = {0};
	char surface[64] = "any";
	size_t len = 0;
	napi_get_value_string_utf8(env, args[1], utterance, sizeof(utterance) - 1, &len);
	if (argc >= 3) {
		napi_valuetype vt;
		if (napi_typeof(env, args[2], &vt) == napi_ok && vt == napi_string) {
			napi_get_value_string_utf8(env, args[2], surface, sizeof(surface) - 1, &len);
		}
	}

	jarvis_process_result_t proc_res;
	jarvis_process(wrap->eng, utterance, surface, &proc_res);

	char *json_buf = (char *)malloc(65536);
	if (!json_buf) {
		napi_throw_error(env, NULL, "Out of memory");
		return NULL;
	}
	size_t n = jarvis_process_result_to_json(&proc_res, json_buf, 65536);
	napi_value res;
	napi_create_string_utf8(env, json_buf, n, &res);
	free(json_buf);
	return res;
}

/* remember(handle, key, value) -> boolean */
static napi_value JarvisRemember(napi_env env, napi_callback_info info)
{
	size_t argc = 3;
	napi_value args[3];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (argc < 3) {
		napi_throw_type_error(env, NULL, "Expected (handle, key, value)");
		return NULL;
	}

	JarvisEngineWrapper *wrap = NULL;
	napi_unwrap(env, args[0], (void **)&wrap);
	if (!wrap || !wrap->is_open || !wrap->eng) {
		napi_throw_error(env, NULL, "Jarvis engine is destroyed or invalid");
		return NULL;
	}

	char key[256] = {0};
	char val[1024] = {0};
	size_t len = 0;
	napi_get_value_string_utf8(env, args[1], key, sizeof(key) - 1, &len);
	napi_get_value_string_utf8(env, args[2], val, sizeof(val) - 1, &len);

	bool ok = jarvis_memory_remember(wrap->eng, key, val);
	napi_value res;
	napi_get_boolean(env, ok, &res);
	return res;
}

/* recall(handle, query?) -> jsonString */
static napi_value JarvisRecall(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (argc < 1) {
		napi_throw_type_error(env, NULL, "Expected (handle[, query])");
		return NULL;
	}

	JarvisEngineWrapper *wrap = NULL;
	napi_unwrap(env, args[0], (void **)&wrap);
	if (!wrap || !wrap->is_open || !wrap->eng) {
		napi_throw_error(env, NULL, "Jarvis engine is destroyed or invalid");
		return NULL;
	}

	char query[256] = {0};
	if (argc >= 2) {
		napi_valuetype vt;
		if (napi_typeof(env, args[1], &vt) == napi_ok && vt == napi_string) {
			size_t len = 0;
			napi_get_value_string_utf8(env, args[1], query, sizeof(query) - 1, &len);
		}
	}

	char json_buf[8192];
	size_t n = jarvis_memory_recall_json(wrap->eng, query, json_buf, sizeof(json_buf));
	napi_value res;
	napi_create_string_utf8(env, json_buf, n, &res);
	return res;
}

/* solve(utterance, surface?) -> jsonString | null */
static napi_value JarvisSolve(napi_env env, napi_callback_info info)
{
	static jarvis_ontology_t *g_ont = NULL;
	if (!g_ont) {
		g_ont = jarvis_ontology_create(NULL);
	}

	size_t argc = 2;
	napi_value args[2];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (argc < 1) {
		napi_value nv;
		napi_get_null(env, &nv);
		return nv;
	}

	char utterance[1024] = {0};
	char surface[64] = "any";
	size_t len = 0;
	napi_get_value_string_utf8(env, args[0], utterance, sizeof(utterance) - 1, &len);
	if (argc >= 2) {
		napi_valuetype vt;
		if (napi_typeof(env, args[1], &vt) == napi_ok && vt == napi_string) {
			napi_get_value_string_utf8(env, args[1], surface, sizeof(surface) - 1, &len);
		}
	}

	jarvis_ir_t ir;
	if (!jarvis_solve_clause(g_ont, utterance, surface[0] ? surface : "any", NULL, &ir)) {
		napi_value nv;
		napi_get_null(env, &nv);
		return nv;
	}
	jarvis_dsl_t dsl;
	if (!jarvis_compile_ir_to_dsl(&ir, surface[0] ? surface : "any", NULL, NULL, &dsl)) {
		napi_value nv;
		napi_get_null(env, &nv);
		return nv;
	}

	char json_buf[4096];
	size_t n = jarvis_dsl_to_json(&dsl, json_buf, sizeof(json_buf));
	napi_value res;
	napi_create_string_utf8(env, json_buf, n, &res);
	return res;
}

/* normalizeDsl(rawJsonString) -> jsonString | null */
static napi_value JarvisNormalizeDsl(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (argc < 1) {
		napi_value nv;
		napi_get_null(env, &nv);
		return nv;
	}

	char raw_json[8192] = {0};
	size_t len = 0;
	napi_get_value_string_utf8(env, args[0], raw_json, sizeof(raw_json) - 1, &len);

	jarvis_dsl_t dsl;
	if (!jarvis_normalize_dsl_json(raw_json, &dsl)) {
		napi_value nv;
		napi_get_null(env, &nv);
		return nv;
	}

	char json_buf[4096];
	size_t n = jarvis_dsl_to_json(&dsl, json_buf, sizeof(json_buf));
	napi_value res;
	napi_create_string_utf8(env, json_buf, n, &res);
	return res;
}

/* splitClauses(utterance) -> string[] */
static napi_value JarvisSplitClauses(napi_env env, napi_callback_info info)
{
	static jarvis_ontology_t *g_ont = NULL;
	if (!g_ont) {
		g_ont = jarvis_ontology_create(NULL);
	}

	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char utterance[1024] = {0};
	if (argc >= 1) {
		size_t len = 0;
		napi_get_value_string_utf8(env, args[0], utterance, sizeof(utterance) - 1, &len);
	}

	char clauses[8][512];
	size_t n = jarvis_split_clauses(g_ont, utterance, clauses, 8);

	napi_value arr;
	napi_create_array_with_length(env, n, &arr);
	for (size_t i = 0; i < n; i++) {
		napi_value item;
		napi_create_string_utf8(env, clauses[i], NAPI_AUTO_LENGTH, &item);
		napi_set_element(env, arr, (uint32_t)i, item);
	}
	return arr;
}

/* extractStreamingAnswer(rawJsonFragment) -> string */
static napi_value JarvisExtractStreamingAnswer(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char raw[8192] = {0};
	if (argc >= 1) {
		size_t len = 0;
		napi_get_value_string_utf8(env, args[0], raw, sizeof(raw) - 1, &len);
	}

	char ans[4096] = {0};
	jarvis_extract_streaming_answer_prefix(raw, ans, sizeof(ans));

	napi_value res;
	napi_create_string_utf8(env, ans, NAPI_AUTO_LENGTH, &res);
	return res;
}

/* popTtsClause(unspoken, isFirst?) -> { ready: boolean, clause: string, remainder: string } */
static napi_value JarvisPopTtsClause(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char unspoken[4096] = {0};
	bool is_first = false;
	if (argc >= 1) {
		size_t len = 0;
		napi_get_value_string_utf8(env, args[0], unspoken, sizeof(unspoken) - 1, &len);
	}
	if (argc >= 2) {
		napi_get_value_bool(env, args[1], &is_first);
	}

	char clause[2048] = {0};
	char rem[4096] = {0};
	bool ready = jarvis_pop_ready_tts_clause(unspoken, is_first, clause, sizeof(clause), rem, sizeof(rem));

	napi_value obj, v_ready, v_clause, v_rem;
	napi_create_object(env, &obj);
	napi_get_boolean(env, ready, &v_ready);
	napi_create_string_utf8(env, clause, NAPI_AUTO_LENGTH, &v_clause);
	napi_create_string_utf8(env, ready ? rem : unspoken, NAPI_AUTO_LENGTH, &v_rem);
	napi_set_named_property(env, obj, "ready", v_ready);
	napi_set_named_property(env, obj, "clause", v_clause);
	napi_set_named_property(env, obj, "remainder", v_rem);
	return obj;
}

/* isDestructiveCommand(cmd) -> boolean */
static napi_value JarvisIsDestructive(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char cmd[2048] = {0};
	if (argc >= 1) {
		size_t len = 0;
		napi_get_value_string_utf8(env, args[0], cmd, sizeof(cmd) - 1, &len);
	}

	napi_value res;
	napi_get_boolean(env, jarvis_is_destructive_command(cmd), &res);
	return res;
}

/* isHallucination(text) -> boolean */
static napi_value JarvisIsHallucination(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char text[2048] = {0};
	if (argc >= 1) {
		size_t len = 0;
		napi_get_value_string_utf8(env, args[0], text, sizeof(text) - 1, &len);
	}

	napi_value res;
	napi_get_boolean(env, jarvis_is_non_speech_hallucination(text), &res);
	return res;
}

/* extractWakeCommand(transcript, wakeWord?) -> { detected: boolean, command: string } */
static napi_value JarvisExtractWakeCommand(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char transcript[2048] = {0};
	char wake_word[64] = "jarvis";
	size_t len = 0;
	if (argc >= 1) {
		napi_get_value_string_utf8(env, args[0], transcript, sizeof(transcript) - 1, &len);
	}
	if (argc >= 2) {
		napi_valuetype vt;
		if (napi_typeof(env, args[1], &vt) == napi_ok && vt == napi_string) {
			napi_get_value_string_utf8(env, args[1], wake_word, sizeof(wake_word) - 1, &len);
		}
	}

	char cmd[2048] = {0};
	bool det = jarvis_voice_extract_wake_command(transcript, wake_word[0] ? wake_word : "jarvis", cmd, sizeof(cmd));

	napi_value obj, v_det, v_cmd;
	napi_create_object(env, &obj);
	napi_get_boolean(env, det, &v_det);
	napi_create_string_utf8(env, cmd, NAPI_AUTO_LENGTH, &v_cmd);
	napi_set_named_property(env, obj, "detected", v_det);
	napi_set_named_property(env, obj, "command", v_cmd);
	return obj;
}

/* writeEarconWav(wavPath, isDone?) -> boolean */
static napi_value JarvisWriteEarconWav(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char wav_path[1024] = {0};
	bool is_done = false;
	if (argc >= 1) {
		size_t len = 0;
		napi_get_value_string_utf8(env, args[0], wav_path, sizeof(wav_path) - 1, &len);
	}
	if (argc >= 2) {
		napi_get_value_bool(env, args[1], &is_done);
	}

	bool ok = jarvis_voice_write_earcon_wav(wav_path, is_done);
	napi_value res;
	napi_get_boolean(env, ok, &res);
	return res;
}

static napi_value Init(napi_env env, napi_value exports)
{
	napi_property_descriptor desc[] = {
		{"create", NULL, JarvisCreate, NULL, NULL, NULL, napi_default, NULL},
		{"destroy", NULL, JarvisDestroy, NULL, NULL, NULL, napi_default, NULL},
		{"process", NULL, JarvisProcess, NULL, NULL, NULL, napi_default, NULL},
		{"remember", NULL, JarvisRemember, NULL, NULL, NULL, napi_default, NULL},
		{"recall", NULL, JarvisRecall, NULL, NULL, NULL, napi_default, NULL},
		{"solve", NULL, JarvisSolve, NULL, NULL, NULL, napi_default, NULL},
		{"normalizeDsl", NULL, JarvisNormalizeDsl, NULL, NULL, NULL, napi_default, NULL},
		{"splitClauses", NULL, JarvisSplitClauses, NULL, NULL, NULL, napi_default, NULL},
		{"extractStreamingAnswer", NULL, JarvisExtractStreamingAnswer, NULL, NULL, NULL, napi_default, NULL},
		{"popTtsClause", NULL, JarvisPopTtsClause, NULL, NULL, NULL, napi_default, NULL},
		{"isDestructiveCommand", NULL, JarvisIsDestructive, NULL, NULL, NULL, napi_default, NULL},
		{"isHallucination", NULL, JarvisIsHallucination, NULL, NULL, NULL, napi_default, NULL},
		{"extractWakeCommand", NULL, JarvisExtractWakeCommand, NULL, NULL, NULL, napi_default, NULL},
		{"writeEarconWav", NULL, JarvisWriteEarconWav, NULL, NULL, NULL, napi_default, NULL}
	};
	napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
	return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
