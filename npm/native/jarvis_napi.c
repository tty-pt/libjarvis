#include <node_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ttypt/jarvis.h>

/* cleanPhrase(text) -> string */
static napi_value JarvisCleanPhrase(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	char text[2048] = {0};
	if (argc >= 1) {
		size_t len = 0;
		napi_get_value_string_utf8(env, args[0], text, sizeof(text) - 1, &len);
	}

	char clean[2048] = {0};
	size_t n = jarvis_clean_phrase(text, clean, sizeof(clean));

	napi_value res;
	napi_create_string_utf8(env, clean, n, &res);
	return res;
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
		{"cleanPhrase", NULL, JarvisCleanPhrase, NULL, NULL, NULL, napi_default, NULL},
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
