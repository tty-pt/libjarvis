#ifndef JARVIS_FIREFOX_CORE_H
#define JARVIS_FIREFOX_CORE_H

/**
 * @file firefox_core.h
 * @brief Self-contained Firefox MozLZ4 session parser + AT-SPI2 browser inspector/driver.
 *
 * ZERO libjarvis, libren, libX11, or WM link dependencies!
 * Platform/WM operations are injected via firefox_core_ops_t callbacks.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
static inline void ff_sleep_ms(int ms) { if (ms > 0) Sleep((DWORD)ms); }
#else
#include <dlfcn.h>
#include <glob.h>
#include <strings.h>
#include <unistd.h>
static inline void ff_sleep_ms(int ms) { if (ms > 0) usleep((useconds_t)ms * 1000U); }
#endif

#define FIREFOX_CORE_MAX_TABS 16
#define FF_INSPECT_BUF_SZ     8192

#define FF_ATSPI_STATE_BUSY        3
#define FF_ATSPI_STATE_DEFUNCT     6
#define FF_ATSPI_STATE_EDITABLE    7
#define FF_ATSPI_STATE_ENABLED     8
#define FF_ATSPI_STATE_FOCUSABLE  11
#define FF_ATSPI_STATE_FOCUSED    12
#define FF_ATSPI_STATE_MULTI_LINE 17
#define FF_ATSPI_STATE_SELECTED   23
#define FF_ATSPI_STATE_SENSITIVE  24
#define FF_ATSPI_STATE_SHOWING    25
#define FF_ATSPI_STATE_SINGLE_LINE 26
#define FF_ATSPI_STATE_VISIBLE    30
#define FF_ATSPI_SCROLL_ANYWHERE   6

typedef struct {
	int  index;
	char title[256];
	char url[512];
} firefox_tab_t;

typedef struct {
	bool found;
	bool showing;
	bool defunct;
	bool busy;
	int  child_count;
	char url[512];
	char title[256];
} ff_doc_state_t;

typedef struct {
	bool found;
	bool editable;
	char role[64];
	char label[128];
	int  cx;
	int  cy;
} ff_focused_input_t;

typedef struct {
	bool (*focus_browser)(void *ud);
	bool (*is_browser_focused)(void *ud);
	bool (*get_focused_geometry)(void *ud, int *x, int *y, int *w, int *h);
	bool (*get_screen_bounds)(void *ud, int *sw, int *sh);
	void (*click_xy)(void *ud, int x, int y, int btn);
	int  (*should_abort)(void *ud);
	void *ud;
} firefox_core_ops_t;

#if defined(__APPLE__) || defined(_WIN32)
#include "../desktop/a11y_core.h"

static bool ff_bridge_focus_win(void *ud)
{
	const firefox_core_ops_t *f = (const firefox_core_ops_t *)ud;
	return (f && f->focus_browser) ? f->focus_browser(f->ud) : true;
}

static bool ff_bridge_is_focused(void *ud)
{
	const firefox_core_ops_t *f = (const firefox_core_ops_t *)ud;
	return (f && f->is_browser_focused) ? f->is_browser_focused(f->ud) : true;
}

static bool ff_bridge_click_xy(void *ud, int x, int y, int btn)
{
	const firefox_core_ops_t *f = (const firefox_core_ops_t *)ud;
	if (!f || !f->click_xy) return false;
	f->click_xy(f->ud, x, y, btn);
	return true;
}

static void ff_to_a11y_ops(const firefox_core_ops_t *f, a11y_ops_t *ao)
{
	memset(ao, 0, sizeof(*ao));
	if (!f) return;
	ao->focus_win      = f->focus_browser ? ff_bridge_focus_win : NULL;
	ao->is_win_focused = f->is_browser_focused ? ff_bridge_is_focused : NULL;
	ao->click_xy       = f->click_xy ? ff_bridge_click_xy : NULL;
	ao->ud             = (void *)f;
}
#endif

static firefox_tab_t g_ff_tabs_cache[FIREFOX_CORE_MAX_TABS];
static size_t        g_ff_tabs_count = 0;
static time_t        g_ff_tabs_mtime = 0;

static bool ff_str_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle) return false;
	size_t nlen = strlen(needle);
	for (const char *h = haystack; *h; h++) {
		size_t i = 0;
		while (i < nlen && h[i] &&
		       tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == nlen) return true;
	}
	return false;
}

static bool ff_is_browser_name(const char *name)
{
	if (!name || !*name) return false;
	return ff_str_ci(name, "Firefox") ||
	       ff_str_ci(name, "Chromium") ||
	       ff_str_ci(name, "Chrome") ||
	       ff_str_ci(name, "Brave") ||
	       ff_str_ci(name, "Safari") ||
	       ff_str_ci(name, "Edge") ||
	       ff_str_ci(name, "msedge") ||
	       ff_str_ci(name, "qutebrowser");
}

static __attribute__((unused)) void ff_url_encode(const char *src, char *dst, size_t dst_sz)
{
	static const char hex[] = "0123456789ABCDEF";
	if (!dst || dst_sz == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && w + 4 < dst_sz; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			dst[w++] = (char)*p;
		} else if (*p == ' ') {
			dst[w++] = '+';
		} else {
			dst[w++] = '%';
			dst[w++] = hex[*p >> 4];
			dst[w++] = hex[*p & 0x0F];
		}
	}
	dst[w] = '\0';
}

/* Native C MozLZ4 block decompressor for Firefox sessionstore recovery.jsonlz4 */
static char *firefox_core_decompress_mozlz4(const char *path, size_t *out_len)
{
	*out_len = 0;
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long fsz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (fsz <= 12 || fsz > 16 * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}
	uint8_t *data = (uint8_t *)malloc((size_t)fsz);
	if (!data) {
		fclose(fp);
		return NULL;
	}
	if (fread(data, 1, (size_t)fsz, fp) != (size_t)fsz || memcmp(data, "mozLz40\0", 8) != 0) {
		free(data);
		fclose(fp);
		return NULL;
	}
	fclose(fp);

	uint32_t uncomp_sz = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
	                     ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
	if (uncomp_sz == 0 || uncomp_sz > 64 * 1024 * 1024) {
		free(data);
		return NULL;
	}

	uint8_t *out = (uint8_t *)malloc(uncomp_sz + 1);
	if (!out) {
		free(data);
		return NULL;
	}

	size_t ip = 12, n = (size_t)fsz, op = 0;
	while (ip < n && op < uncomp_sz) {
		uint8_t token = data[ip++];
		size_t lit_len = token >> 4;
		if (lit_len == 15) {
			while (ip < n) {
				uint8_t s = data[ip++];
				lit_len += s;
				if (s != 255) break;
			}
		}
		if (ip + lit_len > n || op + lit_len > uncomp_sz) break;
		memcpy(out + op, data + ip, lit_len);
		ip += lit_len;
		op += lit_len;
		if (ip >= n || op >= uncomp_sz) break;
		if (ip + 2 > n) break;
		size_t offset = (size_t)data[ip] | ((size_t)data[ip + 1] << 8);
		ip += 2;
		if (offset == 0 || offset > op) break;
		size_t match_len = (token & 0x0F) + 4;
		if (match_len == 19) {
			while (ip < n) {
				uint8_t s = data[ip++];
				match_len += s;
				if (s != 255) break;
			}
		}
		size_t start = op - offset;
		for (size_t i = 0; i < match_len && op < uncomp_sz; i++) {
			out[op++] = out[start + i];
		}
	}
	out[op] = '\0';
	free(data);
	*out_len = op;
	return (char *)out;
}

static __attribute__((unused)) size_t firefox_core_populate_tabs(firefox_tab_t *out_tabs, size_t max_tabs)
{
	if (!out_tabs || max_tabs == 0) return 0;

	char best_path[512] = {0};
	time_t best_mtime = 0;

#ifdef _WIN32
	const char *appdata = getenv("APPDATA");
	if (appdata && *appdata) {
		char search_pat[512];
		snprintf(search_pat, sizeof(search_pat), "%s\\Mozilla\\Firefox\\Profiles\\*", appdata);
		WIN32_FIND_DATAA ffd;
		HANDLE hFind = FindFirstFileA(search_pat, &ffd);
		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
				    strcmp(ffd.cFileName, ".") != 0 && strcmp(ffd.cFileName, "..") != 0) {
					char cand[512];
					struct stat st;
					snprintf(cand, sizeof(cand), "%s\\Mozilla\\Firefox\\Profiles\\%s\\sessionstore-backups\\recovery.jsonlz4",
					         appdata, ffd.cFileName);
					if (stat(cand, &st) == 0 && st.st_mtime > best_mtime) {
						best_mtime = st.st_mtime;
						snprintf(best_path, sizeof(best_path), "%s", cand);
					}
				}
			} while (FindNextFileA(hFind, &ffd));
			FindClose(hFind);
		}
	}
#else
	const char *home = getenv("HOME");
	if (!home || !*home) return 0;

#ifdef __APPLE__
	const char *pats[1] = {
		"%s/Library/Application Support/Firefox/Profiles/*/sessionstore-backups/recovery.jsonlz4"
	};
	int n_pats = 1;
#else
	const char *pats[2] = {
		"%s/snap/firefox/common/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4",
		"%s/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4"
	};
	int n_pats = 2;
#endif

	for (int i = 0; i < n_pats; i++) {
		char pat[512];
		snprintf(pat, sizeof(pat), pats[i], home);
		glob_t gl;
		if (glob(pat, 0, NULL, &gl) == 0) {
			for (size_t j = 0; j < gl.gl_pathc; j++) {
				struct stat st;
				if (stat(gl.gl_pathv[j], &st) == 0 && st.st_mtime > best_mtime) {
					best_mtime = st.st_mtime;
					snprintf(best_path, sizeof(best_path), "%s", gl.gl_pathv[j]);
				}
			}
			globfree(&gl);
		}
	}
#endif

	if (!best_path[0]) return 0;
	if (best_mtime == g_ff_tabs_mtime && g_ff_tabs_count > 0) {
		size_t c = 0;
		for (size_t i = 0; i < g_ff_tabs_count && c < max_tabs; i++) {
			out_tabs[c++] = g_ff_tabs_cache[i];
		}
		return c;
	}

	size_t raw_len = 0;
	char *raw = firefox_core_decompress_mozlz4(best_path, &raw_len);
	if (!raw) return 0;

	g_ff_tabs_count = 0;
	const char *p = raw;
	int tab_idx = 1;
	while ((p = strstr(p, "\"entries\":[")) != NULL && g_ff_tabs_count < FIREFOX_CORE_MAX_TABS) {
		p += 11;
		const char *end_arr = strchr(p, ']');
		if (!end_arr) break;
		const char *url_key = NULL;
		const char *title_key = NULL;
		const char *scan = p;
		while ((scan = strstr(scan, "\"url\":\"")) != NULL && scan < end_arr) {
			url_key = scan;
			const char *tk = strstr(scan, "\"title\":\"");
			if (tk && tk < end_arr) title_key = tk;
			scan += 7;
		}
		if (url_key && url_key < end_arr) {
			url_key += 7;
			const char *url_end = strchr(url_key, '"');
			if (url_end && url_end > url_key) {
				firefox_tab_t *t = &g_ff_tabs_cache[g_ff_tabs_count++];
				memset(t, 0, sizeof(*t));
				t->index = tab_idx++;
				size_t ulen = (size_t)(url_end - url_key);
				if (ulen >= sizeof(t->url)) ulen = sizeof(t->url) - 1;
				memcpy(t->url, url_key, ulen);
				t->url[ulen] = '\0';

				if (title_key && title_key < end_arr) {
					title_key += 9;
					const char *title_end = strchr(title_key, '"');
					if (title_end && title_end > title_key) {
						size_t tlen = (size_t)(title_end - title_key);
						if (tlen >= sizeof(t->title)) tlen = sizeof(t->title) - 1;
						memcpy(t->title, title_key, tlen);
						t->title[tlen] = '\0';
					}
				}
			}
		}
		p = end_arr + 1;
	}
	free(raw);

	g_ff_tabs_mtime = best_mtime;
	size_t c = 0;
	for (size_t i = 0; i < g_ff_tabs_count && c < max_tabs; i++) {
		out_tabs[c++] = g_ff_tabs_cache[i];
	}
	return c;
}

static __attribute__((unused)) bool ff_point_in_viewport(
	int x, int y,
	bool has_win, int wx, int wy, int ww, int wh,
	bool has_scr, int sw, int sh)
{
	if (x < 0 || y < 0) return false;
	if (has_win && ww > 0 && wh > 0) {
		if (x < wx || x >= wx + ww || y < wy || y >= wy + wh) return false;
	}
	if (has_scr && sw > 0 && sh > 0) {
		if (x >= sw || y >= sh) return false;
	}
	if (!has_win && !has_scr) {
		if (x >= 16384 || y >= 16384) return false;
	}
	return true;
}

typedef struct {
	const firefox_core_ops_t *ops;
	const char               *filter_query;
	bool                      read_mode;
	size_t                    skip_chars;
	size_t                    seen_chars;
	bool                      truncated;
	char                     *out;
	size_t                    out_sz;
	size_t                    off;
	int                       visited;
} ff_walk_ctx_t;

static void ff_emit_line(ff_walk_ctx_t *ctx, const char *line)
{
	if (!ctx || !line || !*line || ctx->truncated || ctx->out_sz < 96) return;
	size_t len = strlen(line);
	if (ctx->seen_chars + len <= ctx->skip_chars) {
		ctx->seen_chars += len;
		return;
	}
	size_t start = (ctx->skip_chars > ctx->seen_chars) ? (ctx->skip_chars - ctx->seen_chars) : 0;
	ctx->seen_chars += len;
	size_t rem = len - start;
	size_t limit = ctx->out_sz - 80;
	if (ctx->off >= limit) {
		ctx->truncated = true;
		return;
	}
	size_t avail = limit - ctx->off;
	if (rem > avail) {
		memcpy(ctx->out + ctx->off, line + start, avail);
		ctx->off += avail;
		ctx->out[ctx->off] = '\0';
		ctx->truncated = true;
		return;
	}
	memcpy(ctx->out + ctx->off, line + start, rem);
	ctx->off += rem;
	ctx->out[ctx->off] = '\0';
}

#if !defined(__APPLE__) && !defined(_WIN32)
typedef struct {
	int x;
	int y;
	int width;
	int height;
} ff_atspi_rect_t;

typedef struct {
	int              (*init)(void);
	void            *(*get_desktop)(int);
	int              (*get_child_count)(void *, void **);
	void            *(*get_child_at_index)(void *, int, void **);
	char            *(*get_name)(void *, void **);
	char            *(*get_role_name)(void *, void **);
	char            *(*text_get_text)(void *, int, int, void **);
	void            *(*get_hyperlink)(void *);
	char            *(*hyperlink_get_uri)(void *, int, void **);
	void            *(*get_document_iface)(void *);
	char            *(*doc_get_attr)(void *, const char *, void **);
	void            *(*get_component_iface)(void *);
	ff_atspi_rect_t *(*comp_get_extents)(void *, int, void **);
	int              (*comp_grab_focus)(void *, void **);
	int              (*comp_scroll_to)(void *, int, void **);
	void            *(*get_action_iface)(void *);
	int              (*action_get_n)(void *, void **);
	int              (*action_do)(void *, int, void **);
	void            *(*get_state_set)(void *);
	int              (*state_set_contains)(void *, int);
	void            *(*g_ref)(void *);
	void             (*g_unref)(void *);
	void             (*g_free)(void *);
	bool             ready;
	bool             tried;
} ff_atspi_syms_t;

static ff_atspi_syms_t g_ff_atspi = {0};

static bool ff_ensure_atspi_loaded(void)
{
	if (g_ff_atspi.tried) return g_ff_atspi.ready;
	g_ff_atspi.tried = true;

#ifdef __linux__
	void *h_atspi = dlopen("libatspi.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_gobj  = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_glib  = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	if (!h_atspi || !h_gobj || !h_glib) return false;

	*(void **)&g_ff_atspi.init                = dlsym(h_atspi, "atspi_init");
	*(void **)&g_ff_atspi.get_desktop         = dlsym(h_atspi, "atspi_get_desktop");
	*(void **)&g_ff_atspi.get_child_count     = dlsym(h_atspi, "atspi_accessible_get_child_count");
	*(void **)&g_ff_atspi.get_child_at_index  = dlsym(h_atspi, "atspi_accessible_get_child_at_index");
	*(void **)&g_ff_atspi.get_name            = dlsym(h_atspi, "atspi_accessible_get_name");
	*(void **)&g_ff_atspi.get_role_name       = dlsym(h_atspi, "atspi_accessible_get_role_name");
	*(void **)&g_ff_atspi.text_get_text       = dlsym(h_atspi, "atspi_text_get_text");
	*(void **)&g_ff_atspi.get_hyperlink       = dlsym(h_atspi, "atspi_accessible_get_hyperlink");
	*(void **)&g_ff_atspi.hyperlink_get_uri   = dlsym(h_atspi, "atspi_hyperlink_get_uri");
	*(void **)&g_ff_atspi.get_document_iface  = dlsym(h_atspi, "atspi_accessible_get_document_iface");
	*(void **)&g_ff_atspi.doc_get_attr        = dlsym(h_atspi, "atspi_document_get_document_attribute_value");
	*(void **)&g_ff_atspi.get_component_iface = dlsym(h_atspi, "atspi_accessible_get_component_iface");
	*(void **)&g_ff_atspi.comp_get_extents    = dlsym(h_atspi, "atspi_component_get_extents");
	*(void **)&g_ff_atspi.comp_grab_focus     = dlsym(h_atspi, "atspi_component_grab_focus");
	*(void **)&g_ff_atspi.comp_scroll_to      = dlsym(h_atspi, "atspi_component_scroll_to");
	*(void **)&g_ff_atspi.get_action_iface    = dlsym(h_atspi, "atspi_accessible_get_action_iface");
	*(void **)&g_ff_atspi.action_get_n        = dlsym(h_atspi, "atspi_action_get_n_actions");
	*(void **)&g_ff_atspi.action_do           = dlsym(h_atspi, "atspi_action_do_action");
	*(void **)&g_ff_atspi.get_state_set       = dlsym(h_atspi, "atspi_accessible_get_state_set");
	*(void **)&g_ff_atspi.state_set_contains  = dlsym(h_atspi, "atspi_state_set_contains");
	*(void **)&g_ff_atspi.g_ref               = dlsym(h_gobj, "g_object_ref");
	*(void **)&g_ff_atspi.g_unref             = dlsym(h_gobj, "g_object_unref");
	*(void **)&g_ff_atspi.g_free              = dlsym(h_glib, "g_free");

	if (!g_ff_atspi.init || !g_ff_atspi.get_desktop || !g_ff_atspi.get_child_count ||
	    !g_ff_atspi.get_child_at_index || !g_ff_atspi.get_name || !g_ff_atspi.get_role_name ||
	    !g_ff_atspi.g_unref || !g_ff_atspi.g_free) {
		return false;
	}
	g_ff_atspi.init();
	g_ff_atspi.ready = true;
	return true;
#else
	return false;
#endif
}

static bool ff_node_has_state(void *node, int state_id)
{
	if (!node || !g_ff_atspi.get_state_set || !g_ff_atspi.state_set_contains) return true;
	void *ss = g_ff_atspi.get_state_set(node);
	if (!ss) return false;
	bool has = (g_ff_atspi.state_set_contains(ss, state_id) != 0);
	g_ff_atspi.g_unref(ss);
	return has;
}

static bool ff_is_interactive_role(const char *role)
{
	if (!role) return false;
	return strcmp(role, "entry") == 0 ||
	       strcmp(role, "combo box") == 0 ||
	       strcmp(role, "button") == 0 ||
	       strcmp(role, "push button") == 0 ||
	       strcmp(role, "toggle button") == 0 ||
	       strcmp(role, "link") == 0 ||
	       strcmp(role, "list item") == 0 ||
	       strcmp(role, "page tab") == 0 ||
	       strcmp(role, "menu item") == 0 ||
	       strcmp(role, "check box") == 0 ||
	       strcmp(role, "radio button") == 0 ||
	       strcmp(role, "table row") == 0;
}

static bool ff_get_node_center_checked(
	void *node, const firefox_core_ops_t *ops,
	int *out_cx, int *out_cy, bool *out_in_vp)
{
	if (out_in_vp) *out_in_vp = false;
	if (!node || !g_ff_atspi.get_component_iface || !g_ff_atspi.comp_get_extents) return false;
	void *comp = g_ff_atspi.get_component_iface(node);
	if (!comp) return false;
	void *cerr = NULL;
	ff_atspi_rect_t *r = NULL;
	bool is_win_rel = false;
	int wx = 0, wy = 0, ww = 0, wh = 0;
	bool has_win = (ops && ops->get_focused_geometry &&
	                ops->get_focused_geometry(ops->ud, &wx, &wy, &ww, &wh) &&
	                ww > 0 && wh > 0);
	if (has_win) {
		r = g_ff_atspi.comp_get_extents(comp, 1, &cerr);
		is_win_rel = (r != NULL && r->width > 0 && r->height > 0);
	}
	if (!is_win_rel) {
		if (r) g_ff_atspi.g_free(r);
		cerr = NULL;
		r = g_ff_atspi.comp_get_extents(comp, 0, &cerr);
	}
	g_ff_atspi.g_unref(comp);
	if (!r) return false;

	int rx = r->x;
	int ry = r->y;
	int rw = r->width;
	int rh = r->height;
	g_ff_atspi.g_free(r);

	if (rw <= 0 || rh <= 0) return false;
	while (rx < -1600) rx += 3200;

	if (is_win_rel) {
		if (wx < -1600) wx += 3200;
		rx += wx;
		ry += wy;
	}

	int cx = rx + rw / 2;
	int cy = ry + rh / 2;
	int sw = 0, sh = 0;
	bool has_scr = (ops && ops->get_screen_bounds &&
	                ops->get_screen_bounds(ops->ud, &sw, &sh) &&
	                sw > 0 && sh > 0);
	bool in_vp = ff_point_in_viewport(cx, cy, has_win, wx, wy, ww, wh, has_scr, sw, sh);
	if (out_cx) *out_cx = cx;
	if (out_cy) *out_cy = cy;
	if (out_in_vp) *out_in_vp = in_vp;
	return true;
}

static bool ff_get_node_center(void *node, const firefox_core_ops_t *ops, int *out_cx, int *out_cy)
{
	bool in_vp = false;
	return ff_get_node_center_checked(node, ops, out_cx, out_cy, &in_vp) && in_vp;
}

static void ff_clean_atspi_text(const char *raw, char *clean_txt, size_t clean_sz)
{
	if (!clean_txt || clean_sz == 0) return;
	size_t cw = 0;
	if (raw) {
		for (const unsigned char *p = (const unsigned char *)raw; *p && cw + 2 < clean_sz; ) {
			if (p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBC) {
				if (cw > 0 && clean_txt[cw - 1] != ' ') clean_txt[cw++] = ' ';
				p += 3;
				continue;
			}
			if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] >= 0xAA && p[2] <= 0xAE)) {
				p += 3;
				continue;
			}
			if (p[0] == 0xC2 && p[1] == 0xA0) {
				if (cw > 0 && clean_txt[cw - 1] != ' ') clean_txt[cw++] = ' ';
				p += 2;
				continue;
			}
			if (*p == '\n' || *p == '\r' || *p == '\t') {
				if (cw > 0 && clean_txt[cw - 1] != ' ') clean_txt[cw++] = ' ';
				p++;
				continue;
			}
			clean_txt[cw++] = (char)*p++;
		}
	}
	while (cw > 0 && clean_txt[cw - 1] == ' ') cw--;
	clean_txt[cw] = '\0';
}

static bool ff_is_readable_text_role(const char *role)
{
	if (!role) return false;
	return strcmp(role, "heading") == 0 ||
	       strcmp(role, "paragraph") == 0 ||
	       strcmp(role, "link") == 0 ||
	       strcmp(role, "alert") == 0 ||
	       strcmp(role, "status") == 0 ||
	       strcmp(role, "blockquote") == 0 ||
	       strcmp(role, "caption") == 0 ||
	       strcmp(role, "pre") == 0 ||
	       strcmp(role, "text") == 0 ||
	       strcmp(role, "static") == 0 ||
	       strcmp(role, "label") == 0 ||
	       strcmp(role, "description term") == 0 ||
	       strcmp(role, "description value") == 0 ||
	       strcmp(role, "list item") == 0 ||
	       strcmp(role, "table cell") == 0 ||
	       strcmp(role, "section") == 0 ||
	       strcmp(role, "article") == 0;
}

static void ff_walk_browser_node(
	void *node, int depth, bool in_doc, const char *active_filter,
	ff_walk_ctx_t *ctx)
{
	if (!node || !ctx || depth > 24 || ctx->visited > 4000 || ctx->truncated) return;
	ctx->visited++;

	void *err = NULL;
	char *role = g_ff_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_ff_atspi.get_name(node, &err);

	bool now_in_doc = in_doc;
	const char *child_filter = active_filter;
	bool skip_children = false;

	err = NULL;
	int n_children = g_ff_atspi.get_child_count(node, &err);

	if (role) {
		if (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0) {
			if (!ff_node_has_state(node, FF_ATSPI_STATE_SHOWING) ||
			    ff_node_has_state(node, FF_ATSPI_STATE_DEFUNCT)) {
				g_ff_atspi.g_free(role);
				if (name) g_ff_atspi.g_free(name);
				return;
			}
			now_in_doc = true;
			char *doc_url = NULL;
			if (g_ff_atspi.get_document_iface && g_ff_atspi.doc_get_attr) {
				void *doc_if = g_ff_atspi.get_document_iface(node);
				if (doc_if) {
					void *derr = NULL;
					doc_url = g_ff_atspi.doc_get_attr(doc_if, "DocURL", &derr);
					g_ff_atspi.g_unref(doc_if);
				}
			}
			if (child_filter && *child_filter) {
				if ((name && ff_str_ci(name, child_filter)) ||
				    (doc_url && ff_str_ci(doc_url, child_filter))) {
					child_filter = NULL;
				}
			}
			char line[768];
			snprintf(line, sizeof(line), "[%s] %s%s%s\n",
			         role,
			         (name && *name) ? name : "",
			         (doc_url && *doc_url) ? " url=" : "",
			         (doc_url && *doc_url) ? doc_url : "");
			ff_emit_line(ctx, line);
			if (doc_url) g_ff_atspi.g_free(doc_url);
		} else if (!in_doc && !ctx->read_mode && strcmp(role, "frame") == 0 && name && *name) {
			char line[320];
			snprintf(line, sizeof(line), "[frame] %s\n", name);
			ff_emit_line(ctx, line);
		} else if (!in_doc && !ctx->read_mode && strcmp(role, "page tab") == 0 && name && *name) {
			int cx = 0, cy = 0;
			bool in_vp = false;
			char coord_buf[32] = {0};
			if (ff_get_node_center_checked(node, ctx->ops, &cx, &cy, &in_vp)) {
				if (in_vp) {
					snprintf(coord_buf, sizeof(coord_buf), " @(%d,%d)", cx, cy);
				} else {
					snprintf(coord_buf, sizeof(coord_buf), " *OFF-SCREEN*");
				}
			}
			bool is_sel = ff_node_has_state(node, FF_ATSPI_STATE_SELECTED);
			char line[320];
			snprintf(line, sizeof(line), "[page tab%s%s] %s\n",
			         coord_buf, is_sel ? " *ACTIVE*" : "", name);
			ff_emit_line(ctx, line);
			skip_children = true;
		} else if (now_in_doc && !ctx->read_mode &&
		           (ff_is_interactive_role(role) || strcmp(role, "heading") == 0 ||
		            ff_node_has_state(node, FF_ATSPI_STATE_EDITABLE))) {
			char *txt = NULL;
			if ((!name || !*name) && g_ff_atspi.text_get_text) {
				void *terr = NULL;
				txt = g_ff_atspi.text_get_text(node, 0, 200, &terr);
			}
			const char *best_txt = (name && *name) ? name : ((txt && *txt) ? txt : "");
			char clean_txt[180] = {0};
			ff_clean_atspi_text(best_txt, clean_txt, sizeof(clean_txt));

			bool is_editable = ff_node_has_state(node, FF_ATSPI_STATE_EDITABLE);
			bool is_input = (strcmp(role, "entry") == 0 || strcmp(role, "combo box") == 0 || is_editable);
			bool matches = true;
			if (child_filter && *child_filter && !is_input) {
				matches = ff_str_ci(clean_txt, child_filter) ||
				          (name && ff_str_ci(name, child_filter)) ||
				          ff_str_ci(role, child_filter);
			}

			if (matches && (clean_txt[0] || is_input)) {
				int cx = 0, cy = 0;
				bool in_vp = false;
				if (ff_get_node_center_checked(node, ctx->ops, &cx, &cy, &in_vp)) {
					bool is_focused = ff_node_has_state(node, FF_ATSPI_STATE_FOCUSED);
					if (!is_focused && is_input && n_children > 0) {
						for (int ci = 0; ci < n_children && ci < 4; ci++) {
							void *cerr = NULL;
							void *ch = g_ff_atspi.get_child_at_index(node, ci, &cerr);
							if (ch) {
								if (ff_node_has_state(ch, FF_ATSPI_STATE_FOCUSED)) is_focused = true;
								g_ff_atspi.g_unref(ch);
								if (is_focused) break;
							}
						}
					}
					bool is_disabled = !ff_node_has_state(node, FF_ATSPI_STATE_ENABLED) ||
					                   !ff_node_has_state(node, FF_ATSPI_STATE_SENSITIVE);
					const char *disp_txt = clean_txt[0] ? clean_txt : "(unlabeled)";
					char line[280];
					if (in_vp) {
						snprintf(line, sizeof(line), "[%s @(%d,%d)%s%s] %.120s\n",
						         role, cx, cy,
						         is_focused ? " *FOCUSED*" : "",
						         is_disabled ? " *DISABLED*" : "",
						         disp_txt);
					} else {
						snprintf(line, sizeof(line), "[%s *OFF-SCREEN*%s%s] %.120s\n",
						         role,
						         is_focused ? " *FOCUSED*" : "",
						         is_disabled ? " *DISABLED*" : "",
						         disp_txt);
					}
					ff_emit_line(ctx, line);
					if (strcmp(role, "table row") == 0 ||
					    strcmp(role, "button") == 0 ||
					    strcmp(role, "push button") == 0 ||
					    strcmp(role, "toggle button") == 0 ||
					    strcmp(role, "entry") == 0 ||
					    strcmp(role, "heading") == 0 ||
					    strcmp(role, "link") == 0 ||
					    is_editable) {
						skip_children = true;
					}
				}
			}
			if (txt) g_ff_atspi.g_free(txt);
		} else if (now_in_doc && ctx->read_mode &&
		           (ff_is_readable_text_role(role) || ff_is_interactive_role(role))) {
			bool is_leaf = (n_children <= 0);
			bool is_atomic = (strcmp(role, "heading") == 0 ||
			                  strcmp(role, "link") == 0 ||
			                  strcmp(role, "button") == 0 ||
			                  strcmp(role, "push button") == 0 ||
			                  strcmp(role, "toggle button") == 0 ||
			                  strcmp(role, "entry") == 0 ||
			                  strcmp(role, "combo box") == 0 ||
			                  strcmp(role, "alert") == 0 ||
			                  strcmp(role, "status") == 0);
			if (is_leaf || (is_atomic && name && *name)) {
				char *txt = NULL;
				if ((!name || !*name) && g_ff_atspi.text_get_text) {
					void *terr = NULL;
					txt = g_ff_atspi.text_get_text(node, 0, 400, &terr);
				}
				const char *best_txt = (name && *name) ? name : ((txt && *txt) ? txt : "");
				char clean_txt[360] = {0};
				ff_clean_atspi_text(best_txt, clean_txt, sizeof(clean_txt));

				bool matches = true;
				if (child_filter && *child_filter) {
					matches = ff_str_ci(clean_txt, child_filter) ||
					          (name && ff_str_ci(name, child_filter));
				}

				if (matches && clean_txt[0]) {
					char line[420];
					if (strcmp(role, "static") == 0 || strcmp(role, "text") == 0 ||
					    strcmp(role, "paragraph") == 0 || strcmp(role, "section") == 0 ||
					    strcmp(role, "article") == 0) {
						snprintf(line, sizeof(line), "%s\n", clean_txt);
					} else {
						snprintf(line, sizeof(line), "[%s] %s\n", role, clean_txt);
					}
					ff_emit_line(ctx, line);
					if (is_atomic) skip_children = true;
				}
				if (txt) g_ff_atspi.g_free(txt);
			}
		}
	}

	if (role) g_ff_atspi.g_free(role);
	if (name) g_ff_atspi.g_free(name);
	if (skip_children || ctx->truncated) return;

	int n = n_children;
	if (n > 150) n = 150;
	for (int i = 0; i < n && !ctx->truncated; i++) {
		err = NULL;
		void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			ff_walk_browser_node(ch, depth + 1, now_in_doc, child_filter, ctx);
			g_ff_atspi.g_unref(ch);
		}
	}
}

typedef struct {
	const char               *query;
	const firefox_core_ops_t *ops;
	int                       best_score;
	int                       best_cx;
	int                       best_cy;
	bool                      best_in_vp;
	void                     *best_node;
	char                      best_role[64];
	char                      best_label[128];
	int                       visited;
} ff_atspi_find_ctx_t;

static int ff_score_atspi_match(const char *role, const char *label, bool in_doc, bool in_chat_list, bool in_msg_row, const char *query)
{
	if (!role || !label || !query || !*query) return -1;
	bool label_match = ff_str_ci(label, query);
	bool role_match  = ff_str_ci(role, query);
	if (!label_match && !(role_match && ff_is_interactive_role(role))) return -1;

	int score = 10;
	if (in_doc) score += 20;
	if (ff_is_interactive_role(role)) score += 35;
	if (strcmp(role, "page tab") == 0) score += 30;
	if (in_chat_list && strcmp(role, "table row") == 0) score += 65;
	if (in_msg_row) score -= 75;

	if (strcmp(role, "entry") == 0 || strcmp(role, "combo box") == 0) {
		if (ff_str_ci(query, "escrever") || ff_str_ci(query, "mensagem") ||
		    ff_str_ci(query, "message") || ff_str_ci(query, "procurar") ||
		    ff_str_ci(query, "search") || ff_str_ci(query, "entry") ||
		    ff_str_ci(query, "input") || ff_str_ci(query, "type")) {
			score += 65;
		}
	}

	size_t qlen = strlen(query);
	if (strcasecmp(label, query) == 0) {
		score += 80;
	} else if (strncasecmp(label, query, qlen) == 0) {
		score += 55;
	} else {
		for (size_t pos = 0; label[pos] && pos < 25; pos++) {
			if (strncasecmp(label + pos, query, qlen) == 0) {
				score += 30;
				break;
			}
		}
	}

	if (strncasecmp(label, "Informações de contacto", 23) == 0 &&
	    !ff_str_ci(query, "informa") && !ff_str_ci(query, "contacto")) {
		score -= 60;
	}
	if (!in_chat_list && strcmp(role, "table row") == 0) {
		for (size_t pos = 0; label[pos] && pos < 30; pos++) {
			if (label[pos] == ':' && label[pos + 1] == ' ') {
				score -= 50;
				break;
			}
		}
	}
	return score;
}

static void ff_find_interactive_node(void *node, int depth, bool in_doc, bool in_chat_list, bool in_msg_row, ff_atspi_find_ctx_t *ctx)
{
	if (!node || depth > 24 || ctx->visited > 3000) return;
	ctx->visited++;

	void *err = NULL;
	char *role = g_ff_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_ff_atspi.get_name(node, &err);

	bool now_in_doc = in_doc;
	if (role && (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0)) {
		if (!ff_node_has_state(node, FF_ATSPI_STATE_SHOWING) ||
		    ff_node_has_state(node, FF_ATSPI_STATE_DEFUNCT)) {
			g_ff_atspi.g_free(role);
			if (name) g_ff_atspi.g_free(name);
			return;
		}
		now_in_doc = true;
	}

	bool now_in_chat_list = in_chat_list;
	if (role && strcmp(role, "table") == 0 && name &&
	    (ff_str_ci(name, "conversas") || ff_str_ci(name, "chats") || ff_str_ci(name, "chat list"))) {
		now_in_chat_list = true;
	}
	bool now_in_msg_row = in_msg_row;

	if (role && strcmp(role, "frame") != 0 && strcmp(role, "application") != 0 &&
	    strcmp(role, "document web") != 0 && strcmp(role, "document frame") != 0) {
		char *txt = NULL;
		if ((!name || !*name) && g_ff_atspi.text_get_text) {
			void *terr = NULL;
			txt = g_ff_atspi.text_get_text(node, 0, 200, &terr);
		}
		char clean_name[256] = {0}, clean_txt[256] = {0};
		ff_clean_atspi_text(name, clean_name, sizeof(clean_name));
		ff_clean_atspi_text(txt, clean_txt, sizeof(clean_txt));
		if (txt) g_ff_atspi.g_free(txt);

		const char *label = clean_name[0] ? clean_name : clean_txt;
		int score = ff_score_atspi_match(role, label, now_in_doc, now_in_chat_list, in_msg_row, ctx->query);
		if (score > 0) {
			int cx = 0, cy = 0;
			bool in_vp = false;
			if (ff_get_node_center_checked(node, ctx->ops, &cx, &cy, &in_vp)) {
				if (in_vp) score += 25;
				if (score > ctx->best_score) {
					ctx->best_score = score;
					ctx->best_cx = cx;
					ctx->best_cy = cy;
					ctx->best_in_vp = in_vp;
					if (ctx->best_node && g_ff_atspi.g_unref) {
						g_ff_atspi.g_unref(ctx->best_node);
						ctx->best_node = NULL;
					}
					if (g_ff_atspi.g_ref) {
						ctx->best_node = g_ff_atspi.g_ref(node);
					}
					snprintf(ctx->best_role, sizeof(ctx->best_role), "%.63s", role);
					snprintf(ctx->best_label, sizeof(ctx->best_label), "%.120s", label[0] ? label : role);
				}
			}
		}
		if (strcmp(role, "table row") == 0 && !now_in_chat_list) {
			now_in_msg_row = true;
		}
	}

	if (role) g_ff_atspi.g_free(role);
	if (name) g_ff_atspi.g_free(name);

	err = NULL;
	int n = g_ff_atspi.get_child_count(node, &err);
	if (n > 100) n = 100;
	for (int i = 0; i < n; i++) {
		err = NULL;
		void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			ff_find_interactive_node(ch, depth + 1, now_in_doc, now_in_chat_list, now_in_msg_row, ctx);
			g_ff_atspi.g_unref(ch);
		}
	}
}

typedef struct {
	char role[64];
	char label[128];
	int  cx;
	int  cy;
	bool multi_line;
} ff_edit_cand_t;

typedef struct {
	const firefox_core_ops_t *ops;
	ff_focused_input_t        focused;
	ff_edit_cand_t            cands[16];
	int                       n_cands;
	int                       visited;
} ff_input_probe_ctx_t;

static void ff_walk_editable_inputs(void *node, int depth, bool in_doc, ff_input_probe_ctx_t *ctx)
{
	if (!node || depth > 24 || ctx->visited > 2800 || ctx->focused.found) return;
	ctx->visited++;

	void *err = NULL;
	char *role = g_ff_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_ff_atspi.get_name(node, &err);

	bool now_in_doc = in_doc;
	if (role && (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0)) {
		if (!ff_node_has_state(node, FF_ATSPI_STATE_SHOWING) ||
		    ff_node_has_state(node, FF_ATSPI_STATE_DEFUNCT)) {
			g_ff_atspi.g_free(role);
			if (name) g_ff_atspi.g_free(name);
			return;
		}
		now_in_doc = true;
	}

	bool skip_children = false;
	if (now_in_doc && role &&
	    strcmp(role, "document web") != 0 && strcmp(role, "document frame") != 0 &&
	    strcmp(role, "frame") != 0 && strcmp(role, "application") != 0) {
		bool is_entry_role = (strcmp(role, "entry") == 0 || strcmp(role, "combo box") == 0);
		bool is_editable = ff_node_has_state(node, FF_ATSPI_STATE_EDITABLE);
		if ((is_entry_role || is_editable) &&
		    ff_node_has_state(node, FF_ATSPI_STATE_ENABLED) &&
		    ff_node_has_state(node, FF_ATSPI_STATE_SENSITIVE)) {
			bool has_focus = ff_node_has_state(node, FF_ATSPI_STATE_FOCUSED);
			if (!has_focus) {
				int nc = g_ff_atspi.get_child_count(node, &err);
				for (int i = 0; i < nc && i < 4; i++) {
					void *cerr = NULL;
					void *ch = g_ff_atspi.get_child_at_index(node, i, &cerr);
					if (ch) {
						if (ff_node_has_state(ch, FF_ATSPI_STATE_FOCUSED)) has_focus = true;
						g_ff_atspi.g_unref(ch);
						if (has_focus) break;
					}
				}
			}

			char clean_name[128] = {0};
			ff_clean_atspi_text(name, clean_name, sizeof(clean_name));
			const char *disp = clean_name[0] ? clean_name : "(unlabeled)";
			int cx = 0, cy = 0;
			bool in_vp = false;
			bool have_coords = ff_get_node_center_checked(node, ctx->ops, &cx, &cy, &in_vp);

			if (has_focus) {
				ctx->focused.found = true;
				ctx->focused.editable = true;
				snprintf(ctx->focused.role, sizeof(ctx->focused.role), "%.63s", role);
				snprintf(ctx->focused.label, sizeof(ctx->focused.label), "%.127s", disp);
				ctx->focused.cx = have_coords ? cx : 0;
				ctx->focused.cy = have_coords ? cy : 0;
			} else if (have_coords && in_vp && ctx->n_cands < 16) {
				ff_edit_cand_t *c = &ctx->cands[ctx->n_cands++];
				snprintf(c->role, sizeof(c->role), "%.63s", role);
				snprintf(c->label, sizeof(c->label), "%.127s", disp);
				c->cx = cx;
				c->cy = cy;
				c->multi_line = ff_node_has_state(node, FF_ATSPI_STATE_MULTI_LINE);
			}
			skip_children = true;
		}
	}

	if (role) g_ff_atspi.g_free(role);
	if (name) g_ff_atspi.g_free(name);
	if (skip_children || ctx->focused.found) return;

	err = NULL;
	int n = g_ff_atspi.get_child_count(node, &err);
	if (n > 100) n = 100;
	for (int i = 0; i < n && !ctx->focused.found; i++) {
		err = NULL;
		void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			ff_walk_editable_inputs(ch, depth + 1, now_in_doc, ctx);
			g_ff_atspi.g_unref(ch);
		}
	}
}

static void ff_probe_editable_inputs(const firefox_core_ops_t *ops, ff_input_probe_ctx_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->ops = ops;
	if (!ff_ensure_atspi_loaded()) return;
	void *desktop = g_ff_atspi.get_desktop(0);
	if (!desktop) return;

	void *err = NULL;
	int n_apps = g_ff_atspi.get_child_count(desktop, &err);
	for (int i = 0; i < n_apps && !ctx->focused.found; i++) {
		err = NULL;
		void *app = g_ff_atspi.get_child_at_index(desktop, i, &err);
		if (!app) continue;
		err = NULL;
		char *aname = g_ff_atspi.get_name(app, &err);
		if (ff_is_browser_name(aname)) {
			ff_walk_editable_inputs(app, 0, false, ctx);
		}
		if (aname) g_ff_atspi.g_free(aname);
		g_ff_atspi.g_unref(app);
	}
	g_ff_atspi.g_unref(desktop);
}
#endif /* !__APPLE__ && !_WIN32 */

static __attribute__((unused)) bool firefox_core_get_focused_input(
	const firefox_core_ops_t *ops, ff_focused_input_t *out_info)
{
	if (out_info) memset(out_info, 0, sizeof(*out_info));
#if defined(__APPLE__) || defined(_WIN32)
	a11y_ops_t aops;
	ff_to_a11y_ops(ops, &aops);
	a11y_browser_input_t in;
	if (!a11y_core_browser_ensure_editable(&aops, false, &in, NULL, 0))
		return false;
	if (out_info) {
		out_info->found = in.found;
		out_info->editable = in.editable;
		snprintf(out_info->role, sizeof(out_info->role), "%s", in.role);
		snprintf(out_info->label, sizeof(out_info->label), "%s", in.label);
		out_info->cx = in.cx;
		out_info->cy = in.cy;
	}
	return in.found;
#else
	ff_input_probe_ctx_t ctx;
	ff_probe_editable_inputs(ops, &ctx);
	if (ctx.focused.found && out_info) *out_info = ctx.focused;
	return ctx.focused.found;
#endif
}

static __attribute__((unused)) bool firefox_core_ensure_editable_focused(
	const firefox_core_ops_t *ops,
	ff_focused_input_t *out_info,
	char *out_err, size_t err_sz)
{
	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (out_err && err_sz > 0) out_err[0] = '\0';

#if defined(__APPLE__) || defined(_WIN32)
	a11y_ops_t aops;
	ff_to_a11y_ops(ops, &aops);
	a11y_browser_input_t in;
	if (!a11y_core_browser_ensure_editable(&aops, true, &in, out_err, err_sz))
		return false;
	if (out_info) {
		out_info->found = in.found;
		out_info->editable = in.editable;
		snprintf(out_info->role, sizeof(out_info->role), "%s", in.role);
		snprintf(out_info->label, sizeof(out_info->label), "%s", in.label);
		out_info->cx = in.cx;
		out_info->cy = in.cy;
	}
	return true;
#else
	if (ops && ops->is_browser_focused && !ops->is_browser_focused(ops->ud)) {
		if (ops->focus_browser) ops->focus_browser(ops->ud);
		ff_sleep_ms(40);
		if (!ops->is_browser_focused(ops->ud)) {
			if (out_err && err_sz > 0) {
				snprintf(out_err, err_sz, "Error: browser window is not focused");
			}
			return false;
		}
	}

	ff_input_probe_ctx_t ctx;
	ff_probe_editable_inputs(ops, &ctx);
	if (ctx.focused.found) {
		if (out_info) *out_info = ctx.focused;
		return true;
	}

	if (ctx.n_cands == 0) {
		if (out_err && err_sz > 0) {
			snprintf(out_err, err_sz, "Error: no visible editable input found on page");
		}
		return false;
	}

	int chosen = -1;
	if (ctx.n_cands == 1) {
		chosen = 0;
	} else {
		int multi_idx = -1, n_multi = 0;
		for (int i = 0; i < ctx.n_cands; i++) {
			if (ctx.cands[i].multi_line) {
				multi_idx = i;
				n_multi++;
			}
		}
		if (n_multi == 1) chosen = multi_idx;
	}

	if (chosen >= 0) {
		const ff_edit_cand_t *c = &ctx.cands[chosen];
		if (ops && ops->click_xy) {
			ops->click_xy(ops->ud, c->cx, c->cy, 1);
			ff_sleep_ms(80);
		}
		if (out_info) {
			out_info->found = true;
			out_info->editable = true;
			snprintf(out_info->role, sizeof(out_info->role), "%s", c->role);
			snprintf(out_info->label, sizeof(out_info->label), "%s", c->label);
			out_info->cx = c->cx;
			out_info->cy = c->cy;
		}
		return true;
	}

	if (out_err && err_sz > 0) {
		size_t off = (size_t)snprintf(
			out_err, err_sz,
			"Error: no input focused and %d editable inputs visible (", ctx.n_cands);
		for (int i = 0; i < ctx.n_cands && i < 4 && off + 48 < err_sz; i++) {
			off += (size_t)snprintf(
				out_err + off, err_sz - off,
				"%s[%s '%.32s']", i > 0 ? ", " : "",
				ctx.cands[i].role, ctx.cands[i].label);
		}
		snprintf(out_err + off, err_sz - off, "); click or specify target first");
	}
	return false;
#endif
}

static __attribute__((unused)) void firefox_core_ensure_entry_focused(const firefox_core_ops_t *ops)
{
	firefox_core_ensure_editable_focused(ops, NULL, NULL, 0);
}

static __attribute__((unused)) bool ff_role_supports_unfocused_action(const char *role)
{
	if (!role) return false;
	return strcmp(role, "button") == 0 ||
	       strcmp(role, "push button") == 0 ||
	       strcmp(role, "toggle button") == 0 ||
	       strcmp(role, "link") == 0 ||
	       strcmp(role, "page tab") == 0 ||
	       strcmp(role, "menu item") == 0 ||
	       strcmp(role, "check box") == 0 ||
	       strcmp(role, "radio button") == 0;
}

#if !defined(__APPLE__) && !defined(_WIN32)
static bool ff_try_native_action(
	void *node, const char *role, const char *label,
	char *out_msg, size_t out_sz)
{
	if (!node || !g_ff_atspi.get_action_iface || !g_ff_atspi.action_do) return false;
	void *act = g_ff_atspi.get_action_iface(node);
	if (!act) return false;
	void *nerr = NULL;
	int na = g_ff_atspi.action_get_n ? g_ff_atspi.action_get_n(act, &nerr) : 1;
	bool did = false;
	if (na > 0) {
		void *aerr = NULL;
		did = (g_ff_atspi.action_do(act, 0, &aerr) != 0);
	}
	g_ff_atspi.g_unref(act);
	if (did) {
		ff_sleep_ms(90);
		if (out_msg && out_sz > 0) {
			snprintf(out_msg, out_sz, "activated [%s] '%.64s' via native action",
			         role ? role : "", label ? label : "");
		}
	}
	return did;
}
#endif

static __attribute__((unused)) bool firefox_core_click_element(
	const firefox_core_ops_t *ops,
	const char *query, int btn,
	char *out_role, size_t role_sz,
	char *out_msg, size_t out_sz)
{
	if (out_role && role_sz > 0) out_role[0] = '\0';
	if (out_msg && out_sz > 0) out_msg[0] = '\0';
#if defined(__APPLE__) || defined(_WIN32)
	a11y_ops_t aops;
	ff_to_a11y_ops(ops, &aops);
	return a11y_core_browser_click(&aops, query, btn, out_role, role_sz, out_msg, out_sz);
#else
	if (!query || !*query || !ff_ensure_atspi_loaded()) return false;
	void *desktop = g_ff_atspi.get_desktop(0);
	if (!desktop) return false;

	ff_atspi_find_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ctx.ops = ops;

	void *err = NULL;
	int n_apps = g_ff_atspi.get_child_count(desktop, &err);
	for (int i = 0; i < n_apps; i++) {
		err = NULL;
		void *app = g_ff_atspi.get_child_at_index(desktop, i, &err);
		if (!app) continue;
		err = NULL;
		char *aname = g_ff_atspi.get_name(app, &err);
		if (ff_is_browser_name(aname)) {
			ff_find_interactive_node(app, 0, false, false, false, &ctx);
		}
		if (aname) g_ff_atspi.g_free(aname);
		g_ff_atspi.g_unref(app);
	}
	g_ff_atspi.g_unref(desktop);

	if (ctx.best_score <= 0) {
		if (ctx.best_node && g_ff_atspi.g_unref) g_ff_atspi.g_unref(ctx.best_node);
		return false;
	}

	if (out_role && role_sz > 0) {
		snprintf(out_role, role_sz, "%s", ctx.best_role);
	}

	bool focused = !(ops && ops->is_browser_focused) || ops->is_browser_focused(ops->ud);
	if (!focused && btn == 1 && ff_role_supports_unfocused_action(ctx.best_role)) {
		if (ff_try_native_action(ctx.best_node, ctx.best_role, ctx.best_label, out_msg, out_sz)) {
			if (ctx.best_node && g_ff_atspi.g_unref) g_ff_atspi.g_unref(ctx.best_node);
			return true;
		}
	}

	if (!focused && ops && ops->focus_browser) {
		if (ops->focus_browser(ops->ud)) {
			ff_sleep_ms(40);
			focused = !(ops->is_browser_focused) || ops->is_browser_focused(ops->ud);
			if (focused && ctx.best_node) {
				int ncx = 0, ncy = 0;
				bool nin_vp = false;
				if (ff_get_node_center_checked(ctx.best_node, ops, &ncx, &ncy, &nin_vp)) {
					ctx.best_cx = ncx;
					ctx.best_cy = ncy;
					ctx.best_in_vp = nin_vp;
				}
			}
		}
	}

	if (!ctx.best_in_vp) {
		if (ctx.best_node && g_ff_atspi.get_component_iface && g_ff_atspi.comp_scroll_to) {
			void *comp = g_ff_atspi.get_component_iface(ctx.best_node);
			if (comp) {
				void *serr = NULL;
				g_ff_atspi.comp_scroll_to(comp, FF_ATSPI_SCROLL_ANYWHERE, &serr);
				g_ff_atspi.g_unref(comp);
				ff_sleep_ms(180);
				int ncx = 0, ncy = 0;
				bool nin_vp = false;
				if (ff_get_node_center_checked(ctx.best_node, ops, &ncx, &ncy, &nin_vp) && nin_vp) {
					ctx.best_cx = ncx;
					ctx.best_cy = ncy;
					ctx.best_in_vp = true;
				}
			}
		}
		if (!ctx.best_in_vp && btn == 1 &&
		    ff_try_native_action(ctx.best_node, ctx.best_role, ctx.best_label, out_msg, out_sz)) {
			if (ctx.best_node && g_ff_atspi.g_unref) g_ff_atspi.g_unref(ctx.best_node);
			return true;
		}
		if (!ctx.best_in_vp) {
			if (out_msg && out_sz > 0) {
				snprintf(out_msg, out_sz,
				         "Error: element [%s] '%.64s' is off-screen (outside viewport); scroll or filter first",
				         ctx.best_role, ctx.best_label);
			}
			if (ctx.best_node && g_ff_atspi.g_unref) g_ff_atspi.g_unref(ctx.best_node);
			return false;
		}
	}

	if (ops && ops->is_browser_focused && !ops->is_browser_focused(ops->ud)) {
		if (ops->focus_browser) ops->focus_browser(ops->ud);
		ff_sleep_ms(40);
		if (!ops->is_browser_focused(ops->ud)) {
			if (btn == 1 &&
			    ff_try_native_action(ctx.best_node, ctx.best_role, ctx.best_label, out_msg, out_sz)) {
				if (ctx.best_node && g_ff_atspi.g_unref) g_ff_atspi.g_unref(ctx.best_node);
				return true;
			}
			if (out_msg && out_sz > 0) {
				snprintf(out_msg, out_sz, "Error: browser window lost focus before click");
			}
			if (ctx.best_node && g_ff_atspi.g_unref) g_ff_atspi.g_unref(ctx.best_node);
			return false;
		}
	}

	if ((strcmp(ctx.best_role, "entry") == 0 || strcmp(ctx.best_role, "combo box") == 0) &&
	    ctx.best_node && g_ff_atspi.get_component_iface && g_ff_atspi.comp_grab_focus) {
		void *comp = g_ff_atspi.get_component_iface(ctx.best_node);
		if (comp) {
			void *ferr = NULL;
			g_ff_atspi.comp_grab_focus(comp, &ferr);
			g_ff_atspi.g_unref(comp);
		}
	}

	if (ops && ops->click_xy) {
		ops->click_xy(ops->ud, ctx.best_cx, ctx.best_cy, btn);
	}
	ff_sleep_ms(90);
	if (out_msg && out_sz > 0) {
		snprintf(out_msg, out_sz, "clicked [%s] '%.64s' at (%d,%d)",
		         ctx.best_role, ctx.best_label, ctx.best_cx, ctx.best_cy);
	}
	if (ctx.best_node && g_ff_atspi.g_unref) g_ff_atspi.g_unref(ctx.best_node);
	return true;
#endif
}

static void ff_extract_html_fallback(
	const char *file_url, size_t skip_chars,
	char *out, size_t out_sz, size_t *off, bool *out_truncated)
{
	if (!file_url || strncmp(file_url, "file://", 7) != 0 || *off + 128 >= out_sz) return;
	const char *path = file_url + 7;
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char buf[16384];
	size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[rd] = '\0';

	bool in_tag = false, in_script_or_style = false;
	size_t seen = 0;
	char prev_c = ' ';
	size_t limit = (out_sz > 80) ? (out_sz - 80) : out_sz;
	for (size_t i = 0; i < rd; i++) {
		if (buf[i] == '<') {
			if (strncasecmp(buf + i, "<script", 7) == 0 || strncasecmp(buf + i, "<style", 6) == 0) {
				in_script_or_style = true;
			} else if (strncasecmp(buf + i, "</script>", 9) == 0 || strncasecmp(buf + i, "</style>", 8) == 0) {
				in_script_or_style = false;
			}
			in_tag = true;
			continue;
		}
		if (buf[i] == '>') {
			in_tag = false;
			if (!in_script_or_style && seen > 0 && prev_c != '\n' && prev_c != ' ') {
				prev_c = ' ';
				if (seen >= skip_chars) {
					if (*off >= limit) {
						if (out_truncated) *out_truncated = true;
						break;
					}
					out[(*off)++] = ' ';
				}
				seen++;
			}
			continue;
		}
		if (!in_tag && !in_script_or_style) {
			char c = buf[i];
			if (c == '\n' || c == '\r' || c == '\t') c = ' ';
			if (c == ' ' && prev_c == ' ') continue;
			prev_c = c;
			if (seen >= skip_chars) {
				if (*off >= limit) {
					if (out_truncated) *out_truncated = true;
					break;
				}
				out[(*off)++] = c;
			}
			seen++;
		}
	}
	out[*off] = '\0';
}

static bool ff_collect_a11y(
	const firefox_core_ops_t *ops,
	const firefox_tab_t *tabs, size_t tab_count,
	const char *filter_query,
	bool read_mode, size_t skip_chars,
	char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';

	ff_walk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ops = ops;
	ctx.filter_query = filter_query;
	ctx.read_mode = read_mode;
	ctx.skip_chars = skip_chars;
	ctx.out = out;
	ctx.out_sz = out_sz;

#if defined(__APPLE__) || defined(_WIN32)
	a11y_ops_t aops;
	ff_to_a11y_ops(ops, &aops);
	bool live_ok = a11y_core_browser_collect(&aops, filter_query, read_mode, skip_chars, out, out_sz);
	ctx.off = strlen(out);
	if (live_ok && (strstr(out, "[document web]") != NULL || ctx.off + 128 >= out_sz))
		return true;
#else
	if (ff_ensure_atspi_loaded()) {
		void *desktop = g_ff_atspi.get_desktop(0);
		if (desktop) {
			void *err = NULL;
			int n_apps = g_ff_atspi.get_child_count(desktop, &err);
			for (int i = 0; i < n_apps && !ctx.truncated; i++) {
				err = NULL;
				void *app = g_ff_atspi.get_child_at_index(desktop, i, &err);
				if (!app) continue;
				err = NULL;
				char *aname = g_ff_atspi.get_name(app, &err);
				if (ff_is_browser_name(aname)) {
					ff_walk_browser_node(app, 0, false, filter_query, &ctx);
				}
				if (aname) g_ff_atspi.g_free(aname);
				g_ff_atspi.g_unref(app);
			}
			g_ff_atspi.g_unref(desktop);
		}
	}
#endif

	if (!strstr(out, "[document web]") && tabs) {
		for (size_t i = 0; i < tab_count && !ctx.truncated && ctx.off + 128 < out_sz; i++) {
			char hdr[800];
			snprintf(hdr, sizeof(hdr), "[tab %d] %s (%s)\n",
			         tabs[i].index, tabs[i].title, tabs[i].url);
			ff_emit_line(&ctx, hdr);
			if (strncmp(tabs[i].url, "file://", 7) == 0) {
				size_t rem_skip = (ctx.skip_chars > ctx.seen_chars) ? (ctx.skip_chars - ctx.seen_chars) : 0;
				ff_extract_html_fallback(tabs[i].url, rem_skip, out, out_sz, &ctx.off, &ctx.truncated);
			}
		}
	}

	if (ctx.truncated && ctx.off + 64 < out_sz) {
		snprintf(out + ctx.off, out_sz - ctx.off,
		         "\n[truncated; use 'browser read %zu' for more]",
		         skip_chars + ctx.off);
	}

	return ctx.off > 0;
}

static __attribute__((unused)) bool firefox_core_inspect_a11y(
	const firefox_core_ops_t *ops,
	const firefox_tab_t *tabs, size_t tab_count,
	const char *filter_query,
	char *out, size_t out_sz)
{
	return ff_collect_a11y(ops, tabs, tab_count, filter_query, false, 0, out, out_sz);
}

static __attribute__((unused)) bool firefox_core_read_a11y(
	const firefox_core_ops_t *ops,
	const firefox_tab_t *tabs, size_t tab_count,
	const char *filter_query, size_t skip_chars,
	char *out, size_t out_sz)
{
	return ff_collect_a11y(ops, tabs, tab_count, filter_query, true, skip_chars, out, out_sz);
}

#if !defined(__APPLE__) && !defined(_WIN32)
typedef struct {
	const char               *query;
	const firefox_core_ops_t *ops;
	bool                      active_matches;
	bool                      found_other;
	int                       other_cx;
	int                       other_cy;
	char                      matched_title[128];
} ff_tab_switch_ctx_t;

static void ff_walk_page_tab(void *node, int depth, ff_tab_switch_ctx_t *ctx)
{
	if (!node || depth > 10 || ctx->active_matches) return;
	void *err = NULL;
	char *role = g_ff_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_ff_atspi.get_name(node, &err);

	if (role && (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0)) {
		g_ff_atspi.g_free(role);
		if (name) g_ff_atspi.g_free(name);
		return;
	}
	if (role && strcmp(role, "page tab") == 0 && name && *name) {
		if (ff_str_ci(name, ctx->query)) {
			if (ff_node_has_state(node, FF_ATSPI_STATE_SELECTED)) {
				ctx->active_matches = true;
				snprintf(ctx->matched_title, sizeof(ctx->matched_title), "%.120s", name);
			} else if (!ctx->found_other) {
				bool switched = false;
				if (g_ff_atspi.get_action_iface && g_ff_atspi.action_do) {
					void *act = g_ff_atspi.get_action_iface(node);
					if (act) {
						void *aerr = NULL;
						if (g_ff_atspi.action_do(act, 0, &aerr) != 0) {
							switched = true;
						}
						g_ff_atspi.g_unref(act);
					}
				}
				if (switched) {
					ctx->active_matches = true;
					snprintf(ctx->matched_title, sizeof(ctx->matched_title), "%.120s", name);
					ff_sleep_ms(160);
				} else {
					int cx = 0, cy = 0;
					if (ff_get_node_center(node, ctx->ops, &cx, &cy)) {
						ctx->found_other = true;
						ctx->other_cx = cx;
						ctx->other_cy = cy;
						snprintf(ctx->matched_title, sizeof(ctx->matched_title), "%.120s", name);
					}
				}
			}
		}
		g_ff_atspi.g_free(role);
		g_ff_atspi.g_free(name);
		return;
	}
	if (role) g_ff_atspi.g_free(role);
	if (name) g_ff_atspi.g_free(name);

	err = NULL;
	int n = g_ff_atspi.get_child_count(node, &err);
	if (n > 40) n = 40;
	for (int i = 0; i < n && !ctx->active_matches; i++) {
		err = NULL;
		void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			ff_walk_page_tab(ch, depth + 1, ctx);
			g_ff_atspi.g_unref(ch);
		}
	}
}
#endif

static __attribute__((unused)) bool firefox_core_switch_tab(
	const firefox_core_ops_t *ops, const char *query, char *out_tab_title, size_t title_sz)
{
	if (out_tab_title && title_sz > 0) out_tab_title[0] = '\0';
#if defined(__APPLE__) || defined(_WIN32)
	a11y_ops_t aops;
	ff_to_a11y_ops(ops, &aops);
	return a11y_core_browser_switch_tab(&aops, query, out_tab_title, title_sz);
#else
	if (!query || !*query || !ff_ensure_atspi_loaded()) return false;
	for (int attempt = 0; attempt < 2; attempt++) {
		void *desktop = g_ff_atspi.get_desktop(0);
		if (!desktop) return false;

		ff_tab_switch_ctx_t ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.query = query;
		ctx.ops = ops;

		void *err = NULL;
		int n_apps = g_ff_atspi.get_child_count(desktop, &err);
		for (int i = 0; i < n_apps && !ctx.active_matches; i++) {
			err = NULL;
			void *app = g_ff_atspi.get_child_at_index(desktop, i, &err);
			if (!app) continue;
			err = NULL;
			char *aname = g_ff_atspi.get_name(app, &err);
			if (ff_is_browser_name(aname)) {
				ff_walk_page_tab(app, 0, &ctx);
			}
			if (aname) g_ff_atspi.g_free(aname);
			g_ff_atspi.g_unref(app);
		}
		g_ff_atspi.g_unref(desktop);

		if (ctx.active_matches) {
			if (out_tab_title && title_sz > 0) {
				snprintf(out_tab_title, title_sz, "%s", ctx.matched_title);
			}
			return true;
		}
		if (ctx.found_other && ops && ops->click_xy) {
			ops->click_xy(ops->ud, ctx.other_cx, ctx.other_cy, 1);
			ff_sleep_ms(140);
			if (out_tab_title && title_sz > 0) {
				snprintf(out_tab_title, title_sz, "%s", ctx.matched_title);
			}
			return true;
		}
		ff_sleep_ms(45);
	}
	return false;
#endif
}

static __attribute__((unused)) void firefox_core_extract_domain_kw(const char *url, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!url || !*url) return;
	const char *host = strstr(url, "://");
	host = host ? (host + 3) : url;
	if (strncasecmp(host, "www.", 4) == 0) host += 4;
	if (strncasecmp(host, "web.", 4) == 0) host += 4;
	size_t dlen = 0;
	while (host[dlen] && host[dlen] != '.' && host[dlen] != '/' && host[dlen] != ':' && dlen + 1 < out_sz) {
		out[dlen] = host[dlen];
		dlen++;
	}
	out[dlen] = '\0';
}

static void ff_normalize_url(const char *url, char *out, size_t out_sz, bool host_only)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!url) return;
	while (*url && isspace((unsigned char)*url)) url++;
	const char *p = strstr(url, "://");
	p = p ? (p + 3) : url;
	if (strncasecmp(p, "www.", 4) == 0) p += 4;

	size_t w = 0;
	bool in_host = true;
	for (; *p && w + 1 < out_sz; p++) {
		char c = *p;
		if (host_only && (c == '/' || c == '?' || c == '#' || c == ':')) break;
		if (c == '/' || c == '?' || c == '#') in_host = false;
		out[w++] = in_host ? (char)tolower((unsigned char)c) : c;
	}
	while (w > 0 && out[w - 1] == '/') w--;
	out[w] = '\0';
}

static __attribute__((unused)) bool firefox_core_url_matches(const char *a, const char *b)
{
	char na[512], nb[512];
	ff_normalize_url(a, na, sizeof(na), false);
	ff_normalize_url(b, nb, sizeof(nb), false);
	return na[0] && nb[0] && strcmp(na, nb) == 0;
}

static __attribute__((unused)) bool firefox_core_url_host_matches(const char *a, const char *b)
{
	char ha[256], hb[256];
	ff_normalize_url(a, ha, sizeof(ha), true);
	ff_normalize_url(b, hb, sizeof(hb), true);
	return ha[0] && hb[0] && strcmp(ha, hb) == 0;
}

#if !defined(__APPLE__) && !defined(_WIN32)
static void ff_probe_doc_walk(void *node, int depth, int *visited, ff_doc_state_t *st)
{
	if (!node || !st || st->found || depth > 14 || *visited > 800) return;
	(*visited)++;

	void *err = NULL;
	char *role = g_ff_atspi.get_role_name(node, &err);
	if (role && (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0)) {
		bool showing = ff_node_has_state(node, FF_ATSPI_STATE_SHOWING);
		bool defunct = ff_node_has_state(node, FF_ATSPI_STATE_DEFUNCT);
		if (showing && !defunct) {
			st->found = true;
			st->showing = true;
			st->defunct = false;
			st->busy = ff_node_has_state(node, FF_ATSPI_STATE_BUSY);
			err = NULL;
			st->child_count = g_ff_atspi.get_child_count(node, &err);
			err = NULL;
			char *name = g_ff_atspi.get_name(node, &err);
			if (name) {
				snprintf(st->title, sizeof(st->title), "%s", name);
				g_ff_atspi.g_free(name);
			}
			if (g_ff_atspi.get_document_iface && g_ff_atspi.doc_get_attr) {
				void *doc_if = g_ff_atspi.get_document_iface(node);
				if (doc_if) {
					void *derr = NULL;
					char *doc_url = g_ff_atspi.doc_get_attr(doc_if, "DocURL", &derr);
					if (doc_url) {
						snprintf(st->url, sizeof(st->url), "%s", doc_url);
						g_ff_atspi.g_free(doc_url);
					}
					g_ff_atspi.g_unref(doc_if);
				}
			}
		}
		g_ff_atspi.g_free(role);
		return;
	}
	if (role) g_ff_atspi.g_free(role);

	err = NULL;
	int n = g_ff_atspi.get_child_count(node, &err);
	if (n > 40) n = 40;
	for (int i = 0; i < n && !st->found; i++) {
		err = NULL;
		void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			ff_probe_doc_walk(ch, depth + 1, visited, st);
			g_ff_atspi.g_unref(ch);
		}
	}
}
#endif

static __attribute__((unused)) bool firefox_core_probe_doc(ff_doc_state_t *out_doc)
{
	if (!out_doc) return false;
	memset(out_doc, 0, sizeof(*out_doc));
#if defined(__APPLE__) || defined(_WIN32)
	a11y_browser_doc_t bd;
	if (!a11y_core_browser_probe_doc(&bd))
		return false;
	out_doc->found = bd.found;
	out_doc->showing = bd.showing;
	out_doc->defunct = bd.defunct;
	out_doc->busy = bd.busy;
	out_doc->child_count = bd.child_count;
	snprintf(out_doc->url, sizeof(out_doc->url), "%s", bd.url);
	snprintf(out_doc->title, sizeof(out_doc->title), "%s", bd.title);
	return out_doc->found;
#else
	if (!ff_ensure_atspi_loaded()) return false;
	void *desktop = g_ff_atspi.get_desktop(0);
	if (!desktop) return false;

	void *err = NULL;
	int n_apps = g_ff_atspi.get_child_count(desktop, &err);
	for (int i = 0; i < n_apps && !out_doc->found; i++) {
		err = NULL;
		void *app = g_ff_atspi.get_child_at_index(desktop, i, &err);
		if (!app) continue;
		err = NULL;
		char *aname = g_ff_atspi.get_name(app, &err);
		if (ff_is_browser_name(aname)) {
			int visited = 0;
			ff_probe_doc_walk(app, 0, &visited, out_doc);
		}
		if (aname) g_ff_atspi.g_free(aname);
		g_ff_atspi.g_unref(app);
	}
	g_ff_atspi.g_unref(desktop);
	return out_doc->found;
#endif
}

/*
 * Wait for active document web readiness (or optional text substring / URL transition).
 * Returns:
 *   1  ready
 *   0  timeout (last probed doc state written to *out_doc)
 *  -2  interrupted via ops->should_abort()
 */
static __attribute__((unused)) int firefox_core_wait_loaded(
	const firefox_core_ops_t *ops,
	const char *prev_url,
	const char *expected_url,
	const char *wait_text,
	int timeout_ms,
	bool click_settle,
	ff_doc_state_t *out_doc,
	char *out_reason, size_t reason_sz)
{
	if (timeout_ms <= 0) timeout_ms = 6000;
	if (timeout_ms > 15000) timeout_ms = 15000;
	if (out_doc) memset(out_doc, 0, sizeof(*out_doc));
	if (out_reason && reason_sz > 0) snprintf(out_reason, reason_sz, "timeout");

	const int step_ms = 80;
	int elapsed = 0;
	bool saw_busy = false;
	bool saw_target_doc = false;
	int idle_same_ticks = 0;

	while (elapsed <= timeout_ms) {
		if (ops && ops->should_abort && ops->should_abort(ops->ud)) {
			if (out_reason && reason_sz > 0) snprintf(out_reason, reason_sz, "interrupted");
			return -2;
		}

		ff_doc_state_t doc;
		firefox_core_probe_doc(&doc);
		if (out_doc) *out_doc = doc;
		if (doc.found && doc.busy) saw_busy = true;

		bool doc_ready = doc.found && doc.showing && !doc.defunct && !doc.busy &&
		                 doc.url[0] != '\0' && strcmp(doc.url, "about:blank") != 0 &&
		                 doc.child_count > 0;

		if (doc_ready) {
			bool target_ok = true;
			if (expected_url && *expected_url) {
				bool exact = firefox_core_url_matches(doc.url, expected_url);
				bool host = firefox_core_url_host_matches(doc.url, expected_url);
				bool navigated = (!prev_url || !*prev_url || strcmp(doc.url, prev_url) != 0 || saw_busy);
				target_ok = exact || (host && navigated);
			} else if (click_settle) {
				bool url_changed = (prev_url && *prev_url && strcmp(doc.url, prev_url) != 0);
				if (!url_changed && !saw_busy) {
					idle_same_ticks++;
					/* For in-page clicks that neither navigate nor trigger BUSY, settle after ~160ms */
					if (idle_same_ticks < 2) target_ok = false;
				}
			}

			if (target_ok && wait_text && *wait_text) {
				char snap[FF_INSPECT_BUF_SZ];
				if (firefox_core_read_a11y(ops, NULL, 0, NULL, 0, snap, sizeof(snap)) &&
				    ff_str_ci(snap, wait_text)) {
					if (out_reason && reason_sz > 0) snprintf(out_reason, reason_sz, "text matched");
					return 1;
				}
				target_ok = false;
			} else if (target_ok) {
				saw_target_doc = true;
				char snap[FF_INSPECT_BUF_SZ] = {0};
				bool has_body = false;
				if (firefox_core_read_a11y(ops, NULL, 0, NULL, 0, snap, sizeof(snap))) {
					const char *nl = strchr(snap, '\n');
					has_body = (nl && nl[1] != '\0');
				}
				if (has_body) {
					if (out_reason && reason_sz > 0) snprintf(out_reason, reason_sz, "loaded");
					return 1;
				}
			}
		}

		if (elapsed >= timeout_ms) break;
		ff_sleep_ms(step_ms);
		elapsed += step_ms;
	}

	if (!wait_text && saw_target_doc) {
		if (out_reason && reason_sz > 0) snprintf(out_reason, reason_sz, "loaded (minimal)");
		return 1;
	}

	return 0;
}

#endif /* JARVIS_FIREFOX_CORE_H */
