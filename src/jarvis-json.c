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

