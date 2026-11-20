#include "jarvis-internal.h"

/* ---------------------------------------------------------------------------
 * JSON AST Loader backed by json-c (host) or libbud/src/jsmn.h (WASM)
 * ------------------------------------------------------------------------- */

static jjson_val_t *alloc_val(jjson_type_t t)
{
	jjson_val_t *v = (jjson_val_t *)calloc(1, sizeof(jjson_val_t));
	if (v) v->type = t;
	return v;
}

#define JSMN_PARENT_LINKS
#define JSMN_STATIC
#include "jsmn.h"

static void unescape_jsmn_str(const char *src, size_t slen, char *out, size_t out_sz)
{
	size_t o = 0;
	if (!out || out_sz == 0) return;
	for (size_t i = 0; i < slen && o + 1 < out_sz; i++) {
		char c = src[i];
		if (c == '\\' && i + 1 < slen) {
			char nxt = src[++i];
			switch (nxt) {
			case '"':  out[o++] = '"';  break;
			case '\\': out[o++] = '\\'; break;
			case '/':  out[o++] = '/';  break;
			case 'b':  out[o++] = '\b'; break;
			case 'f':  out[o++] = '\f'; break;
			case 'n':  out[o++] = '\n'; break;
			case 'r':  out[o++] = '\r'; break;
			case 't':  out[o++] = '\t'; break;
			default:   out[o++] = nxt;  break;
			}
		} else {
			out[o++] = c;
		}
	}
	out[o] = '\0';
}

static jjson_val_t *from_jsmn_tokens(const char *js, const jsmntok_t *toks, int ntoks, int *idx)
{
	if (*idx >= ntoks) return NULL;
	const jsmntok_t *t = &toks[(*idx)++];
	size_t slen = (size_t)(t->end - t->start);
	const char *s = js + t->start;

	if (t->type == JSMN_OBJECT) {
		jjson_val_t *v = alloc_val(JJSON_OBJECT);
		if (!v) return NULL;
		if (t->size > 0) {
			v->u.obj.pairs = (jjson_kv_t *)calloc((size_t)t->size, sizeof(jjson_kv_t));
			v->u.obj.cap = (size_t)t->size;
			for (int i = 0; i < t->size && *idx < ntoks; i++) {
				const jsmntok_t *kt = &toks[(*idx)++];
				size_t klen = (size_t)(kt->end - kt->start);
				char *kstr = (char *)malloc(klen + 1);
				if (kstr) unescape_jsmn_str(js + kt->start, klen, kstr, klen + 1);
				jjson_val_t * child = from_jsmn_tokens(js, toks, ntoks, idx);
				size_t pi = v->u.obj.count++;
				v->u.obj.pairs[pi].key = kstr;
				v->u.obj.pairs[pi].val = child;
			}
		}
		return v;
	}
	if (t->type == JSMN_ARRAY) {
		jjson_val_t *v = alloc_val(JJSON_ARRAY);
		if (!v) return NULL;
		if (t->size > 0) {
			v->u.arr.items = (jjson_val_t **)calloc((size_t)t->size, sizeof(jjson_val_t *));
			v->u.arr.cap = (size_t)t->size;
			for (int i = 0; i < t->size && *idx < ntoks; i++) {
				v->u.arr.items[v->u.arr.count++] = from_jsmn_tokens(js, toks, ntoks, idx);
			}
		}
		return v;
	}
	if (t->type == JSMN_STRING) {
		jjson_val_t *v = alloc_val(JJSON_STRING);
		if (v) {
			v->u.str = (char *)malloc(slen + 1);
			if (v->u.str) unescape_jsmn_str(s, slen, v->u.str, slen + 1);
		}
		return v;
	}
	if (t->type == JSMN_PRIMITIVE) {
		if (slen == 4 && strncmp(s, "true", 4) == 0) {
			jjson_val_t *v = alloc_val(JJSON_BOOL);
			if (v) v->u.b = true;
			return v;
		}
		if (slen == 5 && strncmp(s, "false", 5) == 0) {
			jjson_val_t *v = alloc_val(JJSON_BOOL);
			if (v) v->u.b = false;
			return v;
		}
		if (slen == 4 && strncmp(s, "null", 4) == 0) {
			return alloc_val(JJSON_NULL);
		}
		jjson_val_t *v = alloc_val(JJSON_NUMBER);
		if (v) {
			char nbuf[JARVIS_ID_LEN];
			size_t cp = slen < sizeof(nbuf) - 1 ? slen : sizeof(nbuf) - 1;
			memcpy(nbuf, s, cp);
			nbuf[cp] = '\0';
			v->u.num = strtod(nbuf, NULL);
		}
		return v;
	}
	return NULL;
}

jjson_val_t *jjson_parse(const char *json_str)
{
	if (!json_str || !*json_str) return NULL;
	size_t len = strlen(json_str);
	jsmn_parser p;
	jsmn_init(&p);
	int needed = jsmn_parse(&p, json_str, len, NULL, 0);
	if (needed <= 0) return NULL;
	jsmntok_t *toks = (jsmntok_t *)malloc((size_t)needed * sizeof(jsmntok_t));
	if (!toks) return NULL;
	jsmn_init(&p);
	int r = jsmn_parse(&p, json_str, len, toks, (unsigned int)needed);
	if (r <= 0) {
		free(toks);
		return NULL;
	}
	int idx = 0;
	jjson_val_t *root = from_jsmn_tokens(json_str, toks, r, &idx);
	free(toks);
	return root;
}

void jjson_free(jjson_val_t *v)
{
	if (!v) return;
	switch (v->type) {
	case JJSON_STRING:
		free(v->u.str);
		break;
	case JJSON_ARRAY:
		for (size_t i = 0; i < v->u.arr.count; i++) {
			jjson_free(v->u.arr.items[i]);
		}
		free(v->u.arr.items);
		break;
	case JJSON_OBJECT:
		for (size_t i = 0; i < v->u.obj.count; i++) {
			free(v->u.obj.pairs[i].key);
			jjson_free(v->u.obj.pairs[i].val);
		}
		free(v->u.obj.pairs);
		break;
	default:
		break;
	}
	free(v);
}

const jjson_val_t *jjson_obj_get(const jjson_val_t *obj, const char *key)
{
	if (!obj || obj->type != JJSON_OBJECT || !key) return NULL;
	for (size_t i = 0; i < obj->u.obj.count; i++) {
		if (strcmp(obj->u.obj.pairs[i].key, key) == 0) {
			return obj->u.obj.pairs[i].val;
		}
	}
	return NULL;
}

const char *jjson_obj_get_str(const jjson_val_t *obj, const char *key, const char *def)
{
	const jjson_val_t *v = jjson_obj_get(obj, key);
	if (v && v->type == JJSON_STRING && v->u.str) {
		return v->u.str;
	}
	return def;
}

double jjson_obj_get_num(const jjson_val_t *obj, const char *key, double def)
{
	const jjson_val_t *v = jjson_obj_get(obj, key);
	if (v && v->type == JJSON_NUMBER) {
		return v->u.num;
	}
	return def;
}

int jjson_obj_get_int(const jjson_val_t *obj, const char *key, int def)
{
	const jjson_val_t *v = jjson_obj_get(obj, key);
	if (!v) return def;
	if (v->type == JJSON_NUMBER) return (int)v->u.num;
	if (v->type == JJSON_STRING && v->u.str) {
		/* Extract first integer sequence if present (e.g. "Workspace 6" -> 6) */
		const char *s = v->u.str;
		while (*s && !(*s >= '0' && *s <= '9') && *s != '-') s++;
		if (*s) return atoi(s);
	}
	return def;
}

bool jjson_obj_get_bool(const jjson_val_t *obj, const char *key, bool def)
{
	const jjson_val_t *v = jjson_obj_get(obj, key);
	if (v && v->type == JJSON_BOOL) {
		return v->u.b;
	}
	return def;
}

size_t jjson_escape_str(const char *src, char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0) return 0;
	if (!src) {
		dst[0] = '\0';
		return 0;
	}
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && w + 2 < dst_sz; p++) {
		unsigned char c = *p;
		if (c == '"' || c == '\\') {
			if (w + 3 >= dst_sz) break;
			dst[w++] = '\\';
			dst[w++] = (char)c;
		} else if (c == '\n') {
			if (w + 3 >= dst_sz) break;
			dst[w++] = '\\';
			dst[w++] = 'n';
		} else if (c == '\r') {
			if (w + 3 >= dst_sz) break;
			dst[w++] = '\\';
			dst[w++] = 'r';
		} else if (c == '\t') {
			if (w + 3 >= dst_sz) break;
			dst[w++] = '\\';
			dst[w++] = 't';
		} else if (c >= 0x20) {
			dst[w++] = (char)c;
		}
	}
	dst[w] = '\0';
	return w;
}

/* ---------------------------------------------------------------------------
 * LLM JSON Payload Extractor & DSL Normalizer / Healer
 * ------------------------------------------------------------------------- */

bool jarvis_extract_json_payload(const char *raw_text, char *out_json, size_t out_sz)
{
	if (!raw_text || !out_json || out_sz == 0) return false;
	out_json[0] = '\0';

	const char *s = raw_text;
	while (*s && isspace((unsigned char)*s)) s++;
	if (!*s) return false;

	/* 1. Check markdown ```json ... ``` blocks first */
	const char *fence1 = strstr(s, "```");
	if (fence1) {
		const char *body = fence1 + 3;
		if (strncmp(body, "json", 4) == 0 || strncmp(body, "JSON", 4) == 0) {
			body += 4;
		}
		while (*body && isspace((unsigned char)*body)) body++;
		const char *fence2 = strstr(body, "```");
		if (fence2 && fence2 > body) {
			size_t len = (size_t)(fence2 - body);
			while (len > 0 && isspace((unsigned char)body[len - 1])) len--;
			if (len > 0 && len < out_sz) {
				memcpy(out_json, body, len);
				out_json[len] = '\0';
				jjson_val_t *test = jjson_parse(out_json);
				if (test) {
					jjson_free(test);
					return true;
				}
			}
		}
	}

	/* 2. Find outermost { ... } or [ ... ] */
	const char *first_brace = strchr(s, '{');
	const char *first_bracket = strchr(s, '[');
	const char *start = NULL;
	char close_ch = '}';

	if (first_brace && (!first_bracket || first_brace < first_bracket)) {
		start = first_brace;
		close_ch = '}';
	} else if (first_bracket) {
		start = first_bracket;
		close_ch = ']';
	}

	if (!start) return false;
	const char *end = strrchr(start, close_ch);
	if (!end || end <= start) return false;

	size_t len = (size_t)(end - start + 1);
	if (len >= out_sz) len = out_sz - 1;
	memcpy(out_json, start, len);
	out_json[len] = '\0';

	jjson_val_t *test = jjson_parse(out_json);
	if (!test) {
		out_json[0] = '\0';
		return false;
	}
	jjson_free(test);
	return true;
}

static const struct {
	const char *alias;
	const char *canonical;
} OP_ALIASES[] = {
	{"shell", "spawn_shell_command"},
	{"run_command", "spawn_shell_command"},
	{"exec_command", "spawn_shell_command"},
	{"run_shell", "spawn_shell_command"},
	{"terminal_command", "spawn_shell_command"},
	{"launch_app", "open_application"},
	{"open_app", "open_application"},
	{"start_application", "open_application"},
	{"switch_workspace", "workspace_switch"},
	{"goto_workspace", "workspace_switch"},
	{"move_to_workspace", "workspace_move_window"},
	{"move_window_to_workspace", "workspace_move_window"},
	{"close_window", "window_close"},
	{"kill_window", "window_close"},
	{"fullscreen", "window_fullscreen"},
	{"toggle_fullscreen", "window_fullscreen"},
	{"float", "window_float"},
	{"toggle_float", "window_float"},
	{"play_media", "media_play"},
	{"play_music", "media_play"},
	{"pause_media", "media_pause"},
	{"next_track", "media_next"},
	{"prev_track", "media_prev"},
	{"previous_track", "media_prev"},
	{"volume", "media_volume"},
	{"set_volume", "media_volume"},
	{"web_search", "search_web"},
	{"google_search", "search_web"},
	{"browse_url", "open_url"},
	{"type", "type_text"},
	{"dictate", "type_text"},
	{"key", "press_key"},
	{"key_press", "press_key"},
	{"hotkey", "press_key"},
	{"answer", "answer_question"},
	{"reply", "answer_question"},
	{"help", "wm_help"},
	{"read_terminal", "inspect_terminal"},
	{"capture_terminal", "inspect_terminal"},
	{"read_tmux", "inspect_terminal"},
	{"terminal_context", "inspect_terminal"},
	{"read_screen", "inspect_terminal"},
	{"read_browser", "inspect_browser"},
	{"inspect_page", "inspect_browser"},
	{"read_page", "inspect_browser"},
	{"browser_context", "inspect_browser"},
	{"inspect_a11y", "inspect_browser"},
	{"read_accessibility", "inspect_browser"},
	{"read_clipboard", "inspect_clipboard"},
	{"get_clipboard", "inspect_clipboard"},
	{"read_selection", "inspect_clipboard"},
	{"system_health", "inspect_system"},
	{"check_system", "inspect_system"},
	{"inspect_system_health", "inspect_system"},
	{"probe_command", "run_background_command"},
	{"exec_background", "run_background_command"},
	{"check_command", "run_background_command"},
	{"shell_probe", "run_background_command"},
	{"remember", "remember_fact"},
	{"save_memory", "remember_fact"},
	{"save_preference", "remember_fact"},
	{"learn_preference", "remember_fact"},
	{"recall", "recall_facts"},
	{"search_memory", "recall_facts"},
	{"get_preferences", "recall_facts"},
	{"interrupt", "tts_interrupt"},
	{"stop_speaking", "tts_interrupt"},
	{"stop_talking", "tts_interrupt"},
	{"be_quiet", "tts_interrupt"},
	{"silence_tts", "tts_interrupt"},
	{"conversation", "conversation_control"},
	{"conv_control", "conversation_control"},
	{"end_conversation", "conversation_control"},
	{NULL, NULL}
};

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
	if (!dst || dst_sz == 0) return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	while (*src && isspace((unsigned char)*src)) src++;
	size_t len = strlen(src);
	while (len > 0 && isspace((unsigned char)src[len - 1])) len--;
	if (len >= dst_sz) len = dst_sz - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

bool jarvis_normalize_dsl_val(const jjson_val_t *raw, jarvis_dsl_t *out_dsl)
{
	if (!raw || raw->type != JJSON_OBJECT || !out_dsl) return false;
	memset(out_dsl, 0, sizeof(*out_dsl));

	const char *op = jjson_obj_get_str(raw, "op", NULL);
	if (!op) op = jjson_obj_get_str(raw, "operation", NULL);
	if (!op) op = jjson_obj_get_str(raw, "action", NULL);
	if (!op || !*op) return false;

	for (size_t i = 0; OP_ALIASES[i].alias; i++) {
		if (strcmp(op, OP_ALIASES[i].alias) == 0) {
			op = OP_ALIASES[i].canonical;
			break;
		}
	}
	copy_str(out_dsl->op, sizeof(out_dsl->op), op);

	const jjson_val_t *t_obj = jjson_obj_get(raw, "target");
	const jjson_val_t *a_obj = jjson_obj_get(raw, "args");
	const jjson_val_t *v_obj = jjson_obj_get(raw, "verify");
	const jjson_val_t *m_obj = jjson_obj_get(raw, "meta");

	#define GET_T_STR(k) (jjson_obj_get_str(t_obj, (k), NULL) ? jjson_obj_get_str(t_obj, (k), NULL) : \
	                     (jjson_obj_get_str(raw, (k), NULL) ? jjson_obj_get_str(raw, (k), NULL) : \
	                      jjson_obj_get_str(a_obj, (k), NULL)))
	#define GET_A_STR(k) (jjson_obj_get_str(a_obj, (k), NULL) ? jjson_obj_get_str(a_obj, (k), NULL) : \
	                     (jjson_obj_get_str(raw, (k), NULL) ? jjson_obj_get_str(raw, (k), NULL) : \
	                      jjson_obj_get_str(t_obj, (k), NULL)))

	copy_str(out_dsl->target.kind, sizeof(out_dsl->target.kind), GET_T_STR("kind"));
	copy_str(out_dsl->target.relation, sizeof(out_dsl->target.relation), GET_T_STR("relation"));
	copy_str(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), GET_T_STR("identifier"));
	copy_str(out_dsl->target.title_contains, sizeof(out_dsl->target.title_contains), GET_T_STR("title_contains"));
	copy_str(out_dsl->target.class_contains, sizeof(out_dsl->target.class_contains), GET_T_STR("class_contains"));
	copy_str(out_dsl->target.url, sizeof(out_dsl->target.url), GET_T_STR("url"));
	copy_str(out_dsl->target.command, sizeof(out_dsl->target.command), GET_T_STR("command"));
	copy_str(out_dsl->target.query, sizeof(out_dsl->target.query), GET_T_STR("query"));
	out_dsl->target.x = jjson_obj_get_int(t_obj, "x", jjson_obj_get_int(raw, "x", 0));
	out_dsl->target.y = jjson_obj_get_int(t_obj, "y", jjson_obj_get_int(raw, "y", 0));
	out_dsl->target.new_terminal = jjson_obj_get_bool(t_obj, "new_terminal", jjson_obj_get_bool(raw, "new_terminal", false));

	copy_str(out_dsl->args.text, sizeof(out_dsl->args.text), GET_A_STR("text"));
	copy_str(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), GET_A_STR("key_combo"));
	copy_str(out_dsl->args.direction, sizeof(out_dsl->args.direction), GET_A_STR("direction"));
	copy_str(out_dsl->args.action, sizeof(out_dsl->args.action), GET_A_STR("action"));
	copy_str(out_dsl->args.mode, sizeof(out_dsl->args.mode), GET_A_STR("mode"));
	copy_str(out_dsl->args.answer, sizeof(out_dsl->args.answer), GET_A_STR("answer"));
	copy_str(out_dsl->args.aspect, sizeof(out_dsl->args.aspect), GET_A_STR("aspect"));
	copy_str(out_dsl->args.key, sizeof(out_dsl->args.key), GET_A_STR("key"));
	copy_str(out_dsl->args.value, sizeof(out_dsl->args.value), GET_A_STR("value"));
	copy_str(out_dsl->args.reply, sizeof(out_dsl->args.reply), GET_A_STR("reply"));

	out_dsl->args.button = jjson_obj_get_int(a_obj, "button", jjson_obj_get_int(raw, "button", 0));
	out_dsl->args.amount = jjson_obj_get_int(a_obj, "amount", jjson_obj_get_int(raw, "amount", 0));
	out_dsl->args.workspace = jjson_obj_get_int(a_obj, "workspace", jjson_obj_get_int(raw, "workspace", jjson_obj_get_int(t_obj, "workspace", 0)));
	out_dsl->args.seconds = jjson_obj_get_num(a_obj, "seconds", jjson_obj_get_num(raw, "seconds", 0.0));
	out_dsl->args.speak = jjson_obj_get_bool(a_obj, "speak", jjson_obj_get_bool(raw, "speak", false));
	out_dsl->args.background = jjson_obj_get_bool(a_obj, "background", jjson_obj_get_bool(raw, "background", false));
	out_dsl->args.streamed_tts = jjson_obj_get_bool(a_obj, "streamed_tts", false);
	out_dsl->args.expect_reply = jjson_obj_get_bool(a_obj, "expect_reply", jjson_obj_get_bool(raw, "expect_reply", false));

	/* Cross-heal fields placed in wrong slots by LLM */
	if (strcmp(out_dsl->op, "spawn_shell_command") == 0 || strcmp(out_dsl->op, "run_background_command") == 0) {
		if (!out_dsl->target.command[0]) {
			if (out_dsl->target.query[0]) copy_str(out_dsl->target.command, sizeof(out_dsl->target.command), out_dsl->target.query);
			else if (out_dsl->args.text[0]) copy_str(out_dsl->target.command, sizeof(out_dsl->target.command), out_dsl->args.text);
		}
	} else if (strcmp(out_dsl->op, "inspect_system") == 0) {
		if (!out_dsl->args.aspect[0]) {
			if (out_dsl->target.query[0]) copy_str(out_dsl->args.aspect, sizeof(out_dsl->args.aspect), out_dsl->target.query);
			else copy_str(out_dsl->args.aspect, sizeof(out_dsl->args.aspect), "all");
		}
	} else if (strcmp(out_dsl->op, "remember_fact") == 0) {
		if (!out_dsl->args.key[0]) {
			if (out_dsl->target.identifier[0]) copy_str(out_dsl->args.key, sizeof(out_dsl->args.key), out_dsl->target.identifier);
			else copy_str(out_dsl->args.key, sizeof(out_dsl->args.key), "note");
		}
		if (!out_dsl->args.value[0]) {
			const char *fact = GET_A_STR("fact");
			if (fact) copy_str(out_dsl->args.value, sizeof(out_dsl->args.value), fact);
			else if (out_dsl->args.text[0]) copy_str(out_dsl->args.value, sizeof(out_dsl->args.value), out_dsl->args.text);
			else if (out_dsl->target.query[0]) copy_str(out_dsl->args.value, sizeof(out_dsl->args.value), out_dsl->target.query);
		}
	} else if (strcmp(out_dsl->op, "recall_facts") == 0) {
		if (!out_dsl->target.query[0] && out_dsl->args.key[0]) {
			copy_str(out_dsl->target.query, sizeof(out_dsl->target.query), out_dsl->args.key);
		}
	} else if (strcmp(out_dsl->op, "open_application") == 0) {
		if (!out_dsl->target.identifier[0]) {
			const char *app = GET_T_STR("app");
			if (app) copy_str(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), app);
			else if (out_dsl->target.command[0]) copy_str(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), out_dsl->target.command);
		}
	} else if (strcmp(out_dsl->op, "open_url") == 0) {
		if (!out_dsl->target.url[0]) {
			if (out_dsl->target.identifier[0]) copy_str(out_dsl->target.url, sizeof(out_dsl->target.url), out_dsl->target.identifier);
			else if (out_dsl->target.query[0]) copy_str(out_dsl->target.url, sizeof(out_dsl->target.url), out_dsl->target.query);
		}
	} else if (strcmp(out_dsl->op, "search_web") == 0 || strcmp(out_dsl->op, "media_play") == 0 ||
	           strcmp(out_dsl->op, "wm_help") == 0) {
		if (!out_dsl->target.query[0]) {
			if (out_dsl->args.text[0]) copy_str(out_dsl->target.query, sizeof(out_dsl->target.query), out_dsl->args.text);
			else if (out_dsl->target.identifier[0]) copy_str(out_dsl->target.query, sizeof(out_dsl->target.query), out_dsl->target.identifier);
			else if (out_dsl->target.command[0]) copy_str(out_dsl->target.query, sizeof(out_dsl->target.query), out_dsl->target.command);
		}
	} else if (strcmp(out_dsl->op, "type_text") == 0 || strcmp(out_dsl->op, "type_and_enter") == 0) {
		if (!out_dsl->args.text[0]) {
			if (out_dsl->target.query[0]) copy_str(out_dsl->args.text, sizeof(out_dsl->args.text), out_dsl->target.query);
			else if (out_dsl->target.command[0]) copy_str(out_dsl->args.text, sizeof(out_dsl->args.text), out_dsl->target.command);
		}
	} else if (strcmp(out_dsl->op, "press_key") == 0) {
		if (!out_dsl->args.key_combo[0]) {
			if (out_dsl->args.key[0]) copy_str(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), out_dsl->args.key);
			else if (out_dsl->target.identifier[0]) copy_str(out_dsl->args.key_combo, sizeof(out_dsl->args.key_combo), out_dsl->target.identifier);
		}
	} else if (strcmp(out_dsl->op, "workspace_switch") == 0 || strcmp(out_dsl->op, "workspace_move_window") == 0) {
		if (out_dsl->args.workspace <= 0) {
			if (out_dsl->target.identifier[0]) {
				const char *s = out_dsl->target.identifier;
				while (*s && !isdigit((unsigned char)*s)) s++;
				out_dsl->args.workspace = *s ? atoi(s) : 1;
			} else {
				out_dsl->args.workspace = 1;
			}
		}
	} else if (strcmp(out_dsl->op, "focus_relative_window") == 0) {
		if (!out_dsl->target.relation[0]) {
			if (out_dsl->target.identifier[0]) copy_str(out_dsl->target.relation, sizeof(out_dsl->target.relation), out_dsl->target.identifier);
			else copy_str(out_dsl->target.relation, sizeof(out_dsl->target.relation), "other");
		}
		if (strcmp(out_dsl->target.relation, "other") != 0 &&
		    strcmp(out_dsl->target.relation, "previous") != 0 &&
		    strcmp(out_dsl->target.relation, "next") != 0 &&
		    strcmp(out_dsl->target.relation, "master") != 0) {
			copy_str(out_dsl->target.relation, sizeof(out_dsl->target.relation), "other");
		}
		copy_str(out_dsl->target.kind, sizeof(out_dsl->target.kind), "relative");
	} else if (strcmp(out_dsl->op, "focus_window") == 0) {
		if (!out_dsl->target.identifier[0]) {
			if (out_dsl->target.title_contains[0]) copy_str(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), out_dsl->target.title_contains);
			else if (out_dsl->target.class_contains[0]) copy_str(out_dsl->target.identifier, sizeof(out_dsl->target.identifier), out_dsl->target.class_contains);
		}
		if (strcmp(out_dsl->target.identifier, "other") == 0 ||
		    strcmp(out_dsl->target.identifier, "previous") == 0 ||
		    strcmp(out_dsl->target.identifier, "next") == 0 ||
		    strcmp(out_dsl->target.identifier, "master") == 0) {
			copy_str(out_dsl->op, sizeof(out_dsl->op), "focus_relative_window");
			copy_str(out_dsl->target.kind, sizeof(out_dsl->target.kind), "relative");
			copy_str(out_dsl->target.relation, sizeof(out_dsl->target.relation), out_dsl->target.identifier);
			out_dsl->target.identifier[0] = '\0';
		}
	} else if (strcmp(out_dsl->op, "answer_question") == 0) {
		if (!out_dsl->args.answer[0]) {
			if (out_dsl->args.text[0]) copy_str(out_dsl->args.answer, sizeof(out_dsl->args.answer), out_dsl->args.text);
			else if (out_dsl->target.query[0]) copy_str(out_dsl->args.answer, sizeof(out_dsl->args.answer), out_dsl->target.query);
		}
	}

	if (v_obj && v_obj->type == JJSON_OBJECT) {
		if (jjson_obj_get(v_obj, "audio_playing")) {
			out_dsl->verify.has_audio_playing = true;
			out_dsl->verify.audio_playing = jjson_obj_get_bool(v_obj, "audio_playing", false);
		}
		if (jjson_obj_get(v_obj, "window_focused")) {
			out_dsl->verify.has_window_focused = true;
			out_dsl->verify.window_focused = jjson_obj_get_bool(v_obj, "window_focused", false);
		}
		copy_str(out_dsl->verify.title_contains, sizeof(out_dsl->verify.title_contains), jjson_obj_get_str(v_obj, "title_contains", NULL));
	}

	if (m_obj && m_obj->type == JJSON_OBJECT) {
		copy_str(out_dsl->meta.source_ir_cap, sizeof(out_dsl->meta.source_ir_cap), jjson_obj_get_str(m_obj, "source_ir_cap", NULL));
		out_dsl->meta.source_ir_score = jjson_obj_get_num(m_obj, "source_ir_score", 0.0);
		copy_str(out_dsl->meta.inference_backend, sizeof(out_dsl->meta.inference_backend), jjson_obj_get_str(m_obj, "inference_backend", NULL));
		out_dsl->meta.latency_ms = jjson_obj_get_int(m_obj, "latency_ms", 0);
	}

	#undef GET_T_STR
	#undef GET_A_STR
	return true;
}

bool jarvis_normalize_dsl_json(const char *json_str, jarvis_dsl_t *out_dsl)
{
	if (!json_str || !out_dsl) return false;
	char extracted[4096];
	const char *to_parse = json_str;
	if (jarvis_extract_json_payload(json_str, extracted, sizeof(extracted))) {
		to_parse = extracted;
	}
	jjson_val_t *v = jjson_parse(to_parse);
	if (!v) return false;

	bool ok = false;
	if (v->type == JJSON_OBJECT) {
		ok = jarvis_normalize_dsl_val(v, out_dsl);
	} else if (v->type == JJSON_ARRAY && v->u.arr.count > 0) {
		ok = jarvis_normalize_dsl_val(v->u.arr.items[v->u.arr.count - 1], out_dsl);
	}
	jjson_free(v);
	return ok;
}

size_t jarvis_dsl_to_json(const jarvis_dsl_t *dsl, char *out_json, size_t out_sz)
{
	if (!dsl || !out_json || out_sz == 0) return 0;
	char esc_op[128], esc_cmd[1024], esc_id[512], esc_url[1024], esc_q[1024], esc_rel[64], esc_kind[64];
	char esc_txt[1024], esc_key[256], esc_dir[64], esc_act[128], esc_mode[64], esc_ans[2048];
	jjson_escape_str(dsl->op, esc_op, sizeof(esc_op));
	jjson_escape_str(dsl->target.command, esc_cmd, sizeof(esc_cmd));
	jjson_escape_str(dsl->target.identifier, esc_id, sizeof(esc_id));
	jjson_escape_str(dsl->target.url, esc_url, sizeof(esc_url));
	jjson_escape_str(dsl->target.query, esc_q, sizeof(esc_q));
	jjson_escape_str(dsl->target.relation, esc_rel, sizeof(esc_rel));
	jjson_escape_str(dsl->target.kind, esc_kind, sizeof(esc_kind));
	jjson_escape_str(dsl->args.text, esc_txt, sizeof(esc_txt));
	jjson_escape_str(dsl->args.key_combo, esc_key, sizeof(esc_key));
	jjson_escape_str(dsl->args.direction, esc_dir, sizeof(esc_dir));
	jjson_escape_str(dsl->args.action, esc_act, sizeof(esc_act));
	jjson_escape_str(dsl->args.mode, esc_mode, sizeof(esc_mode));
	jjson_escape_str(dsl->args.answer, esc_ans, sizeof(esc_ans));

	int n = snprintf(out_json, out_sz, "{\"op\":\"%s\",\"target\":{", esc_op);
	bool first_t = true;
	#define ADD_T_STR(k, val) do { if ((val)[0]) { n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "%s\"%s\":\"%s\"", first_t ? "" : ",", (k), (val)); first_t = false; } } while (0)
	ADD_T_STR("kind", esc_kind);
	ADD_T_STR("relation", esc_rel);
	ADD_T_STR("identifier", esc_id);
	ADD_T_STR("url", esc_url);
	ADD_T_STR("command", esc_cmd);
	ADD_T_STR("query", esc_q);
	if (dsl->target.new_terminal) {
		n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "%s\"new_terminal\":true", first_t ? "" : ",");
		first_t = false;
	}
	#undef ADD_T_STR

	n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "},\"args\":{");
	bool first_a = true;
	#define ADD_A_STR(k, val) do { if ((val)[0]) { n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "%s\"%s\":\"%s\"", first_a ? "" : ",", (k), (val)); first_a = false; } } while (0)
	ADD_A_STR("text", esc_txt);
	ADD_A_STR("key_combo", esc_key);
	ADD_A_STR("direction", esc_dir);
	ADD_A_STR("action", esc_act);
	ADD_A_STR("mode", esc_mode);
	ADD_A_STR("answer", esc_ans);
	if (dsl->args.workspace > 0) {
		n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "%s\"workspace\":%d", first_a ? "" : ",", dsl->args.workspace);
		first_a = false;
	}
	if (dsl->args.amount > 0) {
		n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "%s\"amount\":%d", first_a ? "" : ",", dsl->args.amount);
		first_a = false;
	}
	if (dsl->args.background) {
		n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "%s\"background\":true", first_a ? "" : ",");
		first_a = false;
	}
	if (dsl->args.expect_reply) {
		n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "%s\"expect_reply\":true", first_a ? "" : ",");
		first_a = false;
	}
	#undef ADD_A_STR

	n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "},\"verify\":{");
	if (dsl->verify.has_audio_playing) {
		n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "\"audio_playing\":%s", dsl->verify.audio_playing ? "true" : "false");
	}
	n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "}}");
	return (size_t)n < out_sz ? (size_t)n : out_sz - 1;
}

size_t jarvis_ir_to_json(const jarvis_ir_t *ir, char *out_json, size_t out_sz)
{
	if (!ir || !out_json || out_sz == 0) return 0;
	char esc_cap[128], esc_surf[64];
	jjson_escape_str(ir->cap, esc_cap, sizeof(esc_cap));
	jjson_escape_str(ir->surface[0] ? ir->surface : "any", esc_surf, sizeof(esc_surf));
	int n = snprintf(
		out_json, out_sz,
		"{\"cap\":\"%s\",\"score\":%.2f,\"surface\":\"%s\",\"explicit_terminal\":%s,\"target_other\":%s,\"submit_enter\":%s}",
		esc_cap, ir->score, esc_surf,
		ir->explicit_terminal ? "true" : "false",
		ir->target_other ? "true" : "false",
		ir->submit_enter ? "true" : "false"
	);
	return (size_t)n < out_sz ? (size_t)n : out_sz - 1;
}

size_t jarvis_process_result_to_json(const jarvis_process_result_t *res, char *out_json, size_t out_sz)
{
	if (!res || !out_json || out_sz == 0) return 0;
	char esc_utt[1024];
	jjson_escape_str(res->utterance, esc_utt, sizeof(esc_utt));
	if (!res->ok) {
		char esc_err[1024];
		jjson_escape_str(res->error, esc_err, sizeof(esc_err));
		int n = snprintf(out_json, out_sz, "{\"ok\":false,\"error\":\"%s\",\"utterance\":\"%s\",\"step_count\":0,\"steps\":[]}", esc_err, esc_utt);
		return (size_t)n < out_sz ? (size_t)n : out_sz - 1;
	}
	char dsl_buf[2048];
	jarvis_dsl_to_json(&res->last_dsl, dsl_buf, sizeof(dsl_buf));
	char esc_src[256], esc_out[2048], esc_err[1024];
	jjson_escape_str(res->source, esc_src, sizeof(esc_src));
	jjson_escape_str(res->combined_result.output, esc_out, sizeof(esc_out));
	jjson_escape_str(res->combined_result.error, esc_err, sizeof(esc_err));
	int n = snprintf(
		out_json, out_sz,
		"{\"ok\":true,\"utterance\":\"%s\",\"dsl\":%s,\"source\":\"%s\",\"result\":{\"handled\":%s,\"success\":%s,\"output\":\"%s\",\"error\":\"%s\"},\"step_count\":%zu,\"steps\":[",
		esc_utt, dsl_buf, esc_src,
		res->combined_result.handled ? "true" : "false",
		res->combined_result.success ? "true" : "false",
		esc_out, esc_err, res->step_count
	);
	for (size_t i = 0; i < res->step_count; i++) {
		const jarvis_step_t *st = &res->steps[i];
		char st_dsl[2048];
		jarvis_dsl_to_json(&st->dsl, st_dsl, sizeof(st_dsl));
		char st_c[1024], st_rc[1024], st_s[128], st_out[2048], st_err[1024];
		jjson_escape_str(st->clause, st_c, sizeof(st_c));
		jjson_escape_str(st->resolved_clause, st_rc, sizeof(st_rc));
		jjson_escape_str(st->source, st_s, sizeof(st_s));
		jjson_escape_str(st->result.output, st_out, sizeof(st_out));
		jjson_escape_str(st->result.error, st_err, sizeof(st_err));
		n += snprintf(
			out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0,
			"%s{\"clause\":\"%s\",\"resolved_clause\":\"%s\",\"source\":\"%s\",\"dsl\":%s,\"result\":{\"handled\":%s,\"success\":%s,\"output\":\"%s\",\"error\":\"%s\"}}",
			i > 0 ? "," : "",
			st_c, st_rc, st_s, st_dsl,
			st->result.handled ? "true" : "false",
			st->result.success ? "true" : "false",
			st_out, st_err
		);
	}
	n += snprintf(out_json + n, (size_t)n < out_sz ? out_sz - (size_t)n : 0, "]}");
	return (size_t)n < out_sz ? (size_t)n : out_sz - 1;
}
