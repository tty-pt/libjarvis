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
#include <dlfcn.h>
#include <glob.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FIREFOX_CORE_MAX_TABS 16

typedef struct {
	int  index;
	char title[256];
	char url[512];
} firefox_tab_t;

typedef struct {
	bool (*focus_browser)(void *ud);
	bool (*is_browser_focused)(void *ud);
	bool (*get_focused_geometry)(void *ud, int *x, int *y, int *w, int *h);
	void (*click_xy)(void *ud, int x, int y, int btn);
	void *ud;
} firefox_core_ops_t;

static firefox_tab_t g_ff_tabs_cache[FIREFOX_CORE_MAX_TABS];
static size_t        g_ff_tabs_count = 0;
static time_t        g_ff_tabs_mtime = 0;

static bool ff_str_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle) return false;
	return strcasestr(haystack, needle) != NULL;
}

static bool ff_is_browser_name(const char *name)
{
	if (!name || !*name) return false;
	return ff_str_ci(name, "Firefox") ||
	       ff_str_ci(name, "Chromium") ||
	       ff_str_ci(name, "Chrome") ||
	       ff_str_ci(name, "Brave") ||
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

static size_t firefox_core_populate_tabs(firefox_tab_t *out_tabs, size_t max_tabs)
{
	if (!out_tabs || max_tabs == 0) return 0;
	const char *home = getenv("HOME");
	if (!home || !*home) return 0;

	const char *pats[2] = {
		"%s/snap/firefox/common/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4",
		"%s/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4"
	};

	char best_path[512] = {0};
	time_t best_mtime = 0;

	for (int i = 0; i < 2; i++) {
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
	void            *(*get_action_iface)(void *);
	int              (*action_get_n)(void *, void **);
	int              (*action_do)(void *, int, void **);
	void            *(*get_state_set)(void *);
	int              (*state_set_contains)(void *, int);
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
	*(void **)&g_ff_atspi.get_action_iface    = dlsym(h_atspi, "atspi_accessible_get_action_iface");
	*(void **)&g_ff_atspi.action_get_n        = dlsym(h_atspi, "atspi_action_get_n_actions");
	*(void **)&g_ff_atspi.action_do           = dlsym(h_atspi, "atspi_action_do_action");
	*(void **)&g_ff_atspi.get_state_set       = dlsym(h_atspi, "atspi_accessible_get_state_set");
	*(void **)&g_ff_atspi.state_set_contains  = dlsym(h_atspi, "atspi_state_set_contains");
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

static bool ff_get_node_center(void *node, const firefox_core_ops_t *ops, int *out_cx, int *out_cy)
{
	if (!node || !g_ff_atspi.get_component_iface || !g_ff_atspi.comp_get_extents) return false;
	void *comp = g_ff_atspi.get_component_iface(node);
	if (!comp) return false;
	void *cerr = NULL;
	ff_atspi_rect_t *r = NULL;
	bool is_win_rel = false;
	int wx = 0, wy = 33, ww = 1600, wh = 867;
	if (ops && ops->get_focused_geometry && ops->get_focused_geometry(ops->ud, &wx, &wy, &ww, &wh)) {
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

	bool valid = (rx >= 0 && ry >= 0 && rx < 3200 && ry < 1200);
	if (valid) {
		*out_cx = rx + rw / 2;
		*out_cy = ry + rh / 2;
	}
	return valid;
}

static void ff_clean_atspi_text(const char *raw, char *clean_txt, size_t clean_sz)
{
	if (!clean_txt || clean_sz == 0) return;
	size_t cw = 0;
	if (raw) {
		for (const unsigned char *p = (const unsigned char *)raw; *p && cw + 2 < clean_sz; ) {
			if (p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBC) {
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

static void ff_walk_browser_node(
	void *node, int depth, bool in_doc, const char *filter_query,
	const firefox_core_ops_t *ops,
	char *out, size_t out_sz, size_t *off, int *visited)
{
	if (!node || depth > 24 || *visited > 2800 || *off + 110 >= out_sz) return;
	(*visited)++;

	void *err = NULL;
	char *role = g_ff_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_ff_atspi.get_name(node, &err);

	bool now_in_doc = in_doc;
	const char *child_filter = filter_query;
	bool skip_children = false;

	if (role) {
		if (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0) {
			if (!ff_node_has_state(node, 25) || ff_node_has_state(node, 38)) {
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
			*off += (size_t)snprintf(out + *off, out_sz - *off,
			                         "[%s] %s%s%s\n",
			                         role,
			                         (name && *name) ? name : "",
			                         (doc_url && *doc_url) ? " url=" : "",
			                         (doc_url && *doc_url) ? doc_url : "");
			if (doc_url) g_ff_atspi.g_free(doc_url);
		} else if (!in_doc && strcmp(role, "frame") == 0 && name && *name) {
			*off += (size_t)snprintf(out + *off, out_sz - *off, "[frame] %s\n", name);
		} else if (!in_doc && strcmp(role, "page tab") == 0 && name && *name) {
			int cx = 0, cy = 0;
			char coord_buf[32] = {0};
			if (ff_get_node_center(node, ops, &cx, &cy)) {
				snprintf(coord_buf, sizeof(coord_buf), " @(%d,%d)", cx, cy);
			}
			bool is_sel = ff_node_has_state(node, 23);
			*off += (size_t)snprintf(out + *off, out_sz - *off,
			                         "[page tab%s%s] %s\n",
			                         coord_buf, is_sel ? " *ACTIVE*" : "", name);
			skip_children = true;
		} else if (now_in_doc &&
		           (ff_is_interactive_role(role) || strcmp(role, "heading") == 0)) {
			char *txt = NULL;
			if ((!name || !*name) && g_ff_atspi.text_get_text) {
				void *terr = NULL;
				txt = g_ff_atspi.text_get_text(node, 0, 200, &terr);
			}
			const char *best_txt = (name && *name) ? name : ((txt && *txt) ? txt : "");
			char clean_txt[180] = {0};
			ff_clean_atspi_text(best_txt, clean_txt, sizeof(clean_txt));

			bool is_input = (strcmp(role, "entry") == 0 || strcmp(role, "combo box") == 0);
			bool matches = true;
			if (child_filter && *child_filter && !is_input) {
				matches = ff_str_ci(clean_txt, child_filter) ||
				          (name && ff_str_ci(name, child_filter)) ||
				          ff_str_ci(role, child_filter);
			}

			if (matches && clean_txt[0]) {
				int cx = 0, cy = 0;
				if (ff_get_node_center(node, ops, &cx, &cy)) {
					*off += (size_t)snprintf(out + *off, out_sz - *off,
					                         "[%s @(%d,%d)] %.95s\n",
					                         role, cx, cy, clean_txt);
					if (strcmp(role, "table row") == 0 ||
					    strcmp(role, "button") == 0 ||
					    strcmp(role, "push button") == 0 ||
					    strcmp(role, "toggle button") == 0 ||
					    strcmp(role, "entry") == 0 ||
					    strcmp(role, "heading") == 0 ||
					    strcmp(role, "link") == 0) {
						skip_children = true;
					}
				}
			}
			if (txt) g_ff_atspi.g_free(txt);
		}
	}

	if (role) g_ff_atspi.g_free(role);
	if (name) g_ff_atspi.g_free(name);
	if (skip_children) return;

	err = NULL;
	int n = g_ff_atspi.get_child_count(node, &err);
	if (n > 80) n = 80;
	if (now_in_doc && (!child_filter || !*child_filter) && n > 10) {
		for (int i = 0; i < 6 && *off + 110 < out_sz; i++) {
			err = NULL;
			void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
			if (ch) {
				ff_walk_browser_node(ch, depth + 1, now_in_doc, child_filter, ops, out, out_sz, off, visited);
				g_ff_atspi.g_unref(ch);
			}
		}
		for (int i = n - 4; i < n && *off + 110 < out_sz; i++) {
			err = NULL;
			void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
			if (ch) {
				ff_walk_browser_node(ch, depth + 1, now_in_doc, child_filter, ops, out, out_sz, off, visited);
				g_ff_atspi.g_unref(ch);
			}
		}
		return;
	}
	for (int i = 0; i < n && *off + 110 < out_sz; i++) {
		err = NULL;
		void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			ff_walk_browser_node(ch, depth + 1, now_in_doc, child_filter, ops, out, out_sz, off, visited);
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
		if (!ff_node_has_state(node, 25) || ff_node_has_state(node, 38)) {
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
		if (score > ctx->best_score) {
			int cx = 0, cy = 0;
			if (ff_get_node_center(node, ctx->ops, &cx, &cy)) {
				ctx->best_score = score;
				ctx->best_cx = cx;
				ctx->best_cy = cy;
				snprintf(ctx->best_role, sizeof(ctx->best_role), "%.63s", role);
				snprintf(ctx->best_label, sizeof(ctx->best_label), "%.120s", label[0] ? label : role);
				if ((strcmp(role, "entry") == 0 || strcmp(role, "combo box") == 0) &&
				    g_ff_atspi.get_component_iface && g_ff_atspi.comp_grab_focus) {
					void *comp = g_ff_atspi.get_component_iface(node);
					if (comp) {
						void *ferr = NULL;
						g_ff_atspi.comp_grab_focus(comp, &ferr);
						g_ff_atspi.g_unref(comp);
					}
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
	const firefox_core_ops_t *ops;
	bool                      any_entry_focused;
	int                       best_cx;
	int                       best_cy;
	int                       best_score;
	char                      best_label[128];
	int                       visited;
} ff_entry_focus_ctx_t;

static void ff_walk_entry_focus(void *node, int depth, bool in_doc, ff_entry_focus_ctx_t *ctx)
{
	if (!node || depth > 24 || ctx->visited > 2800 || ctx->any_entry_focused) return;
	ctx->visited++;

	void *err = NULL;
	char *role = g_ff_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_ff_atspi.get_name(node, &err);

	bool now_in_doc = in_doc;
	if (role && (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0)) {
		if (!ff_node_has_state(node, 25) || ff_node_has_state(node, 38)) {
			g_ff_atspi.g_free(role);
			if (name) g_ff_atspi.g_free(name);
			return;
		}
		now_in_doc = true;
	}

	if (now_in_doc && role && (strcmp(role, "entry") == 0 || strcmp(role, "combo box") == 0)) {
		if (ff_node_has_state(node, 12)) {
			ctx->any_entry_focused = true;
		} else {
			int nc = g_ff_atspi.get_child_count(node, &err);
			for (int i = 0; i < nc && i < 4; i++) {
				void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
				if (ch) {
					if (ff_node_has_state(ch, 12)) ctx->any_entry_focused = true;
					g_ff_atspi.g_unref(ch);
				}
			}
		}
		int cx = 0, cy = 0;
		if (!ctx->any_entry_focused && ff_get_node_center(node, ctx->ops, &cx, &cy)) {
			char clean_name[128] = {0};
			ff_clean_atspi_text(name, clean_name, sizeof(clean_name));
			int score = 20 + (cy / 20);
			if (ff_str_ci(clean_name, "escrever") ||
			    ff_str_ci(clean_name, "mensagem") ||
			    ff_str_ci(clean_name, "message") ||
			    ff_str_ci(clean_name, "reply") ||
			    ff_str_ci(clean_name, "chat")) {
				score += 100;
			}
			if (score > ctx->best_score) {
				ctx->best_score = score;
				ctx->best_cx = cx;
				ctx->best_cy = cy;
				snprintf(ctx->best_label, sizeof(ctx->best_label), "%s", clean_name);
			}
		}
	}

	if (role) g_ff_atspi.g_free(role);
	if (name) g_ff_atspi.g_free(name);
	if (ctx->any_entry_focused) return;

	err = NULL;
	int n = g_ff_atspi.get_child_count(node, &err);
	if (n > 100) n = 100;
	for (int i = 0; i < n && !ctx->any_entry_focused; i++) {
		err = NULL;
		void *ch = g_ff_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			ff_walk_entry_focus(ch, depth + 1, now_in_doc, ctx);
			g_ff_atspi.g_unref(ch);
		}
	}
}

static void firefox_core_ensure_entry_focused(const firefox_core_ops_t *ops)
{
	if (!ops || !ops->is_browser_focused || !ops->is_browser_focused(ops->ud)) return;
	if (!ff_ensure_atspi_loaded()) return;
	void *desktop = g_ff_atspi.get_desktop(0);
	if (!desktop) return;

	ff_entry_focus_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ops = ops;

	void *err = NULL;
	int n_apps = g_ff_atspi.get_child_count(desktop, &err);
	for (int i = 0; i < n_apps && !ctx.any_entry_focused; i++) {
		err = NULL;
		void *app = g_ff_atspi.get_child_at_index(desktop, i, &err);
		if (!app) continue;
		err = NULL;
		char *aname = g_ff_atspi.get_name(app, &err);
		if (ff_is_browser_name(aname)) {
			ff_walk_entry_focus(app, 0, false, &ctx);
		}
		if (aname) g_ff_atspi.g_free(aname);
		g_ff_atspi.g_unref(app);
	}
	g_ff_atspi.g_unref(desktop);

	if (!ctx.any_entry_focused && ctx.best_score > 0 && ops->click_xy) {
		ops->click_xy(ops->ud, ctx.best_cx, ctx.best_cy, 1);
		usleep(80000);
	}
}

static bool firefox_core_click_element(
	const firefox_core_ops_t *ops,
	const char *query, int btn,
	char *out_role, size_t role_sz,
	char *out_msg, size_t out_sz)
{
	if (out_role && role_sz > 0) out_role[0] = '\0';
	if (out_msg && out_sz > 0) out_msg[0] = '\0';
	if (!query || !*query || !ff_ensure_atspi_loaded()) return false;
	if (ops && ops->focus_browser) ops->focus_browser(ops->ud);
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

	if (ctx.best_score <= 0) return false;

	if (out_role && role_sz > 0) {
		snprintf(out_role, role_sz, "%s", ctx.best_role);
	}
	if (ops && ops->click_xy) {
		ops->click_xy(ops->ud, ctx.best_cx, ctx.best_cy, btn);
	}
	usleep(90000);
	if (out_msg && out_sz > 0) {
		snprintf(out_msg, out_sz, "clicked [%s] '%.64s' at (%d,%d)",
		         ctx.best_role, ctx.best_label, ctx.best_cx, ctx.best_cy);
	}
	return true;
}

static void ff_extract_html_fallback(const char *file_url, char *out, size_t out_sz, size_t *off)
{
	if (!file_url || strncmp(file_url, "file://", 7) != 0 || *off + 128 >= out_sz) return;
	const char *path = file_url + 7;
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char buf[4096];
	size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[rd] = '\0';

	bool in_tag = false, in_script_or_style = false;
	for (size_t i = 0; i < rd && *off + 2 < out_sz; i++) {
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
			if (!in_script_or_style && *off > 0 && out[*off - 1] != '\n' && out[*off - 1] != ' ') {
				out[(*off)++] = ' ';
			}
			continue;
		}
		if (!in_tag && !in_script_or_style) {
			char c = buf[i];
			if (c == '\n' || c == '\r' || c == '\t') c = ' ';
			if (c == ' ' && *off > 0 && out[*off - 1] == ' ') continue;
			out[(*off)++] = c;
		}
	}
	out[*off] = '\0';
}

static bool firefox_core_inspect_a11y(
	const firefox_core_ops_t *ops,
	const firefox_tab_t *tabs, size_t tab_count,
	const char *filter_query,
	char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';
	size_t off = 0;

	if (ff_ensure_atspi_loaded()) {
		void *desktop = g_ff_atspi.get_desktop(0);
		if (desktop) {
			void *err = NULL;
			int n_apps = g_ff_atspi.get_child_count(desktop, &err);
			for (int i = 0; i < n_apps && off + 128 < out_sz; i++) {
				err = NULL;
				void *app = g_ff_atspi.get_child_at_index(desktop, i, &err);
				if (!app) continue;
				err = NULL;
				char *aname = g_ff_atspi.get_name(app, &err);
				if (ff_is_browser_name(aname)) {
					int visited = 0;
					ff_walk_browser_node(app, 0, false, filter_query, ops, out, out_sz, &off, &visited);
				}
				if (aname) g_ff_atspi.g_free(aname);
				g_ff_atspi.g_unref(app);
			}
			g_ff_atspi.g_unref(desktop);
		}
	}

	if (!strstr(out, "[document web]") && tabs) {
		for (size_t i = 0; i < tab_count && off + 128 < out_sz; i++) {
			off += (size_t)snprintf(out + off, out_sz - off,
			                        "[tab %d] %s (%s)\n",
			                        tabs[i].index, tabs[i].title, tabs[i].url);
			if (strncmp(tabs[i].url, "file://", 7) == 0) {
				ff_extract_html_fallback(tabs[i].url, out, out_sz, &off);
			}
		}
	}

	return off > 0;
}

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
			if (ff_node_has_state(node, 23)) {
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
					usleep(160000);
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

static bool firefox_core_switch_tab(const firefox_core_ops_t *ops, const char *query, char *out_tab_title, size_t title_sz)
{
	if (out_tab_title && title_sz > 0) out_tab_title[0] = '\0';
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
			usleep(140000);
			if (out_tab_title && title_sz > 0) {
				snprintf(out_tab_title, title_sz, "%s", ctx.matched_title);
			}
			return true;
		}
		usleep(45000);
	}
	return false;
}

static void firefox_core_extract_domain_kw(const char *url, char *out, size_t out_sz)
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

#endif /* JARVIS_FIREFOX_CORE_H */
