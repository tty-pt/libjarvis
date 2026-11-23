#ifndef JARVIS_DESKTOP_ATSPI_CORE_H
#define JARVIS_DESKTOP_ATSPI_CORE_H

/**
 * @file atspi_core.h
 * @brief Lightweight dynamic AT-SPI2 reader/inspector for focused desktop windows.
 *
 * ZERO link dependencies — resolves libatspi/libgobject/libglib via dlopen().
 */

#include <ctype.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ATSPI_STATE_ACTIVE    1
#define ATSPI_STATE_DEFUNCT   6
#define ATSPI_STATE_EDITABLE  7
#define ATSPI_STATE_ENABLED   8
#define ATSPI_STATE_FOCUSED   12
#define ATSPI_STATE_SELECTED  23
#define ATSPI_STATE_SENSITIVE 24
#define ATSPI_STATE_SHOWING   25

typedef struct {
	int x;
	int y;
	int width;
	int height;
} atspi_rect_t;

typedef struct {
	int           (*init)(void);
	void         *(*get_desktop)(int);
	int           (*get_child_count)(void *, void **);
	void         *(*get_child_at_index)(void *, int, void **);
	char         *(*get_name)(void *, void **);
	char         *(*get_role_name)(void *, void **);
	unsigned int  (*get_pid)(void *, void **);
	void         *(*get_text_iface)(void *);
	char         *(*text_get_text)(void *, int, int, void **);
	void         *(*get_component_iface)(void *);
	atspi_rect_t *(*comp_get_extents)(void *, int, void **);
	void         *(*get_state_set)(void *);
	int           (*state_set_contains)(void *, int);
	void          (*g_unref)(void *);
	void          (*g_free)(void *);
	bool          ready;
	bool          tried;
} atspi_syms_t;

static atspi_syms_t g_atspi = {0};

static __attribute__((unused)) bool atspi_core_ensure_loaded(void)
{
	if (g_atspi.tried) return g_atspi.ready;
	g_atspi.tried = true;

	void *h_atspi = dlopen("libatspi.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_gobj  = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_glib  = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	if (!h_atspi || !h_gobj || !h_glib) return false;

	*(void **)&g_atspi.init                = dlsym(h_atspi, "atspi_init");
	*(void **)&g_atspi.get_desktop         = dlsym(h_atspi, "atspi_get_desktop");
	*(void **)&g_atspi.get_child_count     = dlsym(h_atspi, "atspi_accessible_get_child_count");
	*(void **)&g_atspi.get_child_at_index  = dlsym(h_atspi, "atspi_accessible_get_child_at_index");
	*(void **)&g_atspi.get_name            = dlsym(h_atspi, "atspi_accessible_get_name");
	*(void **)&g_atspi.get_role_name       = dlsym(h_atspi, "atspi_accessible_get_role_name");
	*(void **)&g_atspi.get_pid             = dlsym(h_atspi, "atspi_accessible_get_process_id");
	*(void **)&g_atspi.get_text_iface      = dlsym(h_atspi, "atspi_accessible_get_text_iface");
	*(void **)&g_atspi.text_get_text       = dlsym(h_atspi, "atspi_text_get_text");
	*(void **)&g_atspi.get_component_iface = dlsym(h_atspi, "atspi_accessible_get_component_iface");
	*(void **)&g_atspi.comp_get_extents    = dlsym(h_atspi, "atspi_component_get_extents");
	*(void **)&g_atspi.get_state_set       = dlsym(h_atspi, "atspi_accessible_get_state_set");
	*(void **)&g_atspi.state_set_contains  = dlsym(h_atspi, "atspi_state_set_contains");
	*(void **)&g_atspi.g_unref             = dlsym(h_gobj, "g_object_unref");
	*(void **)&g_atspi.g_free              = dlsym(h_glib, "g_free");

	if (!g_atspi.init || !g_atspi.get_desktop || !g_atspi.get_child_count ||
	    !g_atspi.get_child_at_index || !g_atspi.get_name || !g_atspi.get_role_name ||
	    !g_atspi.g_unref || !g_atspi.g_free) {
		return false;
	}
	g_atspi.init();
	g_atspi.ready = true;
	return true;
}

static __attribute__((unused)) void atspi_core_clean_text(const char *raw, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	size_t w = 0;
	if (raw) {
		for (const unsigned char *p = (const unsigned char *)raw; *p && w + 2 < out_sz; ) {
			if (p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBC) {
				if (w > 0 && out[w - 1] != ' ') out[w++] = ' ';
				p += 3;
				continue;
			}
			if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] >= 0xAA && p[2] <= 0xAE)) {
				p += 3;
				continue;
			}
			if (p[0] == 0xC2 && p[1] == 0xA0) {
				if (w > 0 && out[w - 1] != ' ') out[w++] = ' ';
				p += 2;
				continue;
			}
			if (*p == '\n' || *p == '\r' || *p == '\t') {
				if (w > 0 && out[w - 1] != ' ') out[w++] = ' ';
				p++;
				continue;
			}
			out[w++] = (char)*p++;
		}
	}
	while (w > 0 && out[w - 1] == ' ') w--;
	out[w] = '\0';
}

static bool atspi_has_state(void *node, int state_id)
{
	if (!node || !g_atspi.get_state_set || !g_atspi.state_set_contains) return false;
	void *ss = g_atspi.get_state_set(node);
	if (!ss) return false;
	bool has = (g_atspi.state_set_contains(ss, state_id) != 0);
	g_atspi.g_unref(ss);
	return has;
}

static bool atspi_is_interactive_role(const char *role)
{
	if (!role) return false;
	return strcmp(role, "entry") == 0 ||
	       strcmp(role, "password text") == 0 ||
	       strcmp(role, "combo box") == 0 ||
	       strcmp(role, "button") == 0 ||
	       strcmp(role, "push button") == 0 ||
	       strcmp(role, "toggle button") == 0 ||
	       strcmp(role, "check box") == 0 ||
	       strcmp(role, "radio button") == 0 ||
	       strcmp(role, "link") == 0 ||
	       strcmp(role, "list item") == 0 ||
	       strcmp(role, "page tab") == 0 ||
	       strcmp(role, "menu item") == 0 ||
	       strcmp(role, "check menu item") == 0 ||
	       strcmp(role, "radio menu item") == 0 ||
	       strcmp(role, "slider") == 0 ||
	       strcmp(role, "spin button") == 0 ||
	       strcmp(role, "table row") == 0;
}

static bool atspi_is_readable_role(const char *role)
{
	if (!role) return false;
	return strcmp(role, "heading") == 0 ||
	       strcmp(role, "label") == 0 ||
	       strcmp(role, "paragraph") == 0 ||
	       strcmp(role, "text") == 0 ||
	       strcmp(role, "static") == 0 ||
	       strcmp(role, "alert") == 0 ||
	       strcmp(role, "status") == 0 ||
	       strcmp(role, "status bar") == 0 ||
	       strcmp(role, "caption") == 0 ||
	       strcmp(role, "blockquote") == 0 ||
	       strcmp(role, "pre") == 0 ||
	       strcmp(role, "list item") == 0 ||
	       strcmp(role, "table cell") == 0 ||
	       strcmp(role, "section") == 0 ||
	       strcmp(role, "article") == 0;
}

typedef struct {
	const char *filter;
	bool        inspect_mode;
	int         wx, wy, ww, wh;
	bool        has_win;
	char       *out;
	size_t      out_sz;
	size_t      off;
	bool        truncated;
	int         visited;
	char        last_line[384];
} atspi_walk_ctx_t;

static void atspi_emit_line(atspi_walk_ctx_t *ctx, const char *line)
{
	if (!ctx || !line || !*line || ctx->truncated || ctx->out_sz < 64) return;
	if (strcmp(ctx->last_line, line) == 0) return;
	snprintf(ctx->last_line, sizeof(ctx->last_line), "%s", line);

	size_t len = strlen(line);
	size_t limit = ctx->out_sz - 48;
	if (ctx->off >= limit) {
		ctx->truncated = true;
		return;
	}
	size_t avail = limit - ctx->off;
	if (len > avail) {
		memcpy(ctx->out + ctx->off, line, avail);
		ctx->off += avail;
		ctx->out[ctx->off] = '\0';
		ctx->truncated = true;
		return;
	}
	memcpy(ctx->out + ctx->off, line, len);
	ctx->off += len;
	ctx->out[ctx->off] = '\0';
}

static char *atspi_read_node_text(void *node, int max_chars)
{
	if (!node || !g_atspi.get_text_iface || !g_atspi.text_get_text) return NULL;
	void *tif = g_atspi.get_text_iface(node);
	if (!tif) return NULL;
	void *err = NULL;
	char *txt = g_atspi.text_get_text(tif, 0, max_chars, &err);
	g_atspi.g_unref(tif);
	return txt;
}

static bool atspi_node_center(
	void *node, const atspi_walk_ctx_t *ctx, int *out_cx, int *out_cy, bool *out_in_vp)
{
	if (out_in_vp) *out_in_vp = false;
	if (!node || !g_atspi.get_component_iface || !g_atspi.comp_get_extents) return false;
	void *comp = g_atspi.get_component_iface(node);
	if (!comp) return false;
	void *err = NULL;
	atspi_rect_t *r = g_atspi.comp_get_extents(comp, 0, &err);
	if ((!r || r->width <= 0 || r->height <= 0) && ctx && ctx->has_win) {
		if (r) g_atspi.g_free(r);
		err = NULL;
		r = g_atspi.comp_get_extents(comp, 1, &err);
		if (r && r->width > 0 && r->height > 0) {
			r->x += ctx->wx;
			r->y += ctx->wy;
		}
	}
	g_atspi.g_unref(comp);
	if (!r) return false;
	int cx = r->x + r->width / 2;
	int cy = r->y + r->height / 2;
	int rw = r->width, rh = r->height;
	g_atspi.g_free(r);
	if (rw <= 0 || rh <= 0) return false;

	bool in_vp = (cx >= 0 && cy >= 0);
	if (in_vp && ctx && ctx->has_win && ctx->ww > 0 && ctx->wh > 0) {
		if (cx < ctx->wx || cx >= ctx->wx + ctx->ww || cy < ctx->wy || cy >= ctx->wy + ctx->wh) {
			in_vp = false;
		}
	}
	if (out_cx) *out_cx = cx;
	if (out_cy) *out_cy = cy;
	if (out_in_vp) *out_in_vp = in_vp;
	return true;
}

static void atspi_walk_node(void *node, int depth, atspi_walk_ctx_t *ctx)
{
	if (!node || !ctx || depth > 24 || ctx->visited > 3500 || ctx->truncated) return;
	ctx->visited++;

	if (atspi_has_state(node, ATSPI_STATE_DEFUNCT)) return;

	void *err = NULL;
	char *role = g_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_atspi.get_name(node, &err);
	err = NULL;
	int n_children = g_atspi.get_child_count(node, &err);

	bool skip_children = false;
	if (role) {
		if ((strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0 ||
		     strcmp(role, "frame") == 0 || strcmp(role, "dialog") == 0) &&
		    !atspi_has_state(node, ATSPI_STATE_SHOWING)) {
			g_atspi.g_free(role);
			if (name) g_atspi.g_free(name);
			return;
		}

		bool is_container_hdr = (strcmp(role, "frame") == 0 || strcmp(role, "dialog") == 0 ||
		                         strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0);
		if (is_container_hdr && name && *name) {
			char clean_hdr[256] = {0};
			atspi_core_clean_text(name, clean_hdr, sizeof(clean_hdr));
			if (clean_hdr[0] && (!ctx->filter || !*ctx->filter || strcasestr(clean_hdr, ctx->filter))) {
				char line[320];
				snprintf(line, sizeof(line), "[%s] %s\n", role, clean_hdr);
				atspi_emit_line(ctx, line);
			}
		} else if (ctx->inspect_mode &&
		           (atspi_is_interactive_role(role) || strcmp(role, "heading") == 0 ||
		            atspi_has_state(node, ATSPI_STATE_EDITABLE))) {
			char *txt = atspi_read_node_text(node, 200);
			char cname[180] = {0}, ctxt[180] = {0};
			atspi_core_clean_text(name, cname, sizeof(cname));
			atspi_core_clean_text(txt, ctxt, sizeof(ctxt));
			const char *disp = cname[0] ? cname : (ctxt[0] ? ctxt : "(unlabeled)");
			bool matches = (!ctx->filter || !*ctx->filter ||
			                strcasestr(disp, ctx->filter) || strcasestr(role, ctx->filter));
			if (matches && (cname[0] || ctxt[0] || atspi_has_state(node, ATSPI_STATE_EDITABLE))) {
				int cx = 0, cy = 0;
				bool in_vp = false;
				char pos[32] = {0};
				if (atspi_node_center(node, ctx, &cx, &cy, &in_vp)) {
					if (in_vp) snprintf(pos, sizeof(pos), " @(%d,%d)", cx, cy);
					else snprintf(pos, sizeof(pos), " *OFF-SCREEN*");
				}
				bool focused = atspi_has_state(node, ATSPI_STATE_FOCUSED);
				bool selected = atspi_has_state(node, ATSPI_STATE_SELECTED);
				char line[320];
				snprintf(line, sizeof(line), "[%s%s%s%s] %.160s\n",
				         role, pos,
				         focused ? " *FOCUSED*" : "",
				         selected ? " *ACTIVE*" : "",
				         disp);
				atspi_emit_line(ctx, line);
				if (strcmp(role, "button") == 0 || strcmp(role, "push button") == 0 ||
				    strcmp(role, "toggle button") == 0 || strcmp(role, "menu item") == 0 ||
				    strcmp(role, "page tab") == 0 || strcmp(role, "entry") == 0 ||
				    strcmp(role, "heading") == 0 || strcmp(role, "link") == 0) {
					skip_children = true;
				}
			}
			if (txt) g_atspi.g_free(txt);
		} else if (!ctx->inspect_mode &&
		           (atspi_is_readable_role(role) || atspi_is_interactive_role(role) ||
		            atspi_has_state(node, ATSPI_STATE_EDITABLE))) {
			bool is_leaf = (n_children <= 0);
			bool is_atomic = (strcmp(role, "heading") == 0 || strcmp(role, "label") == 0 ||
			                  strcmp(role, "push button") == 0 || strcmp(role, "button") == 0 ||
			                  strcmp(role, "toggle button") == 0 || strcmp(role, "menu item") == 0 ||
			                  strcmp(role, "entry") == 0 || strcmp(role, "combo box") == 0 ||
			                  strcmp(role, "link") == 0 || strcmp(role, "alert") == 0);
			if (is_leaf || is_atomic) {
				char *txt = atspi_read_node_text(node, 400);
				char cname[260] = {0}, ctxt[360] = {0}, disp_buf[420] = {0};
				atspi_core_clean_text(name, cname, sizeof(cname));
				atspi_core_clean_text(txt, ctxt, sizeof(ctxt));
				const char *disp = ctxt[0] ? ctxt : cname;
				if (strcmp(role, "entry") == 0 && cname[0] && ctxt[0] && strcasecmp(cname, ctxt) != 0) {
					snprintf(disp_buf, sizeof(disp_buf), "%.150s: %.240s", cname, ctxt);
					disp = disp_buf;
				}
				if (disp[0] && (!ctx->filter || !*ctx->filter || strcasestr(disp, ctx->filter))) {
					char line[420];
					if (strcmp(role, "label") == 0 || strcmp(role, "text") == 0 ||
					    strcmp(role, "static") == 0 || strcmp(role, "paragraph") == 0 ||
					    strcmp(role, "section") == 0 || strcmp(role, "article") == 0) {
						snprintf(line, sizeof(line), "%s\n", disp);
					} else {
						snprintf(line, sizeof(line), "[%s] %s\n", role, disp);
					}
					atspi_emit_line(ctx, line);
					if (is_atomic) skip_children = true;
				}
				if (txt) g_atspi.g_free(txt);
			}
		}
	}

	if (role) g_atspi.g_free(role);
	if (name) g_atspi.g_free(name);
	if (skip_children || ctx->truncated) return;

	if (n_children > 150) n_children = 150;
	for (int i = 0; i < n_children && !ctx->truncated; i++) {
		err = NULL;
		void *ch = g_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			atspi_walk_node(ch, depth + 1, ctx);
			g_atspi.g_unref(ch);
		}
	}
}

static bool atspi_app_matches_window(
	void *app, const char *aname, unsigned long target_pid,
	const char *target_class, const char *target_title)
{
	if (!app) return false;
	if (target_pid > 0 && g_atspi.get_pid) {
		void *err = NULL;
		unsigned long apid = (unsigned long)g_atspi.get_pid(app, &err);
		if (apid == target_pid) return true;
	}
	if (aname && *aname) {
		if (target_class && *target_class && strcasestr(target_class, aname)) return true;
		if (target_class && *target_class && strcasestr(aname, target_class)) return true;
	}
	if (target_title && *target_title) {
		void *err = NULL;
		int nc = g_atspi.get_child_count(app, &err);
		if (nc > 12) nc = 12;
		for (int i = 0; i < nc; i++) {
			err = NULL;
			void *ch = g_atspi.get_child_at_index(app, i, &err);
			if (!ch) continue;
			err = NULL;
			char *cname = g_atspi.get_name(ch, &err);
			bool hit = (cname && *cname && strcasestr(target_title, cname)) ||
			           (cname && *cname && strcasestr(cname, target_title));
			if (cname) g_atspi.g_free(cname);
			g_atspi.g_unref(ch);
			if (hit) return true;
		}
	}
	return false;
}

static __attribute__((unused)) bool atspi_core_read_window(
	unsigned long target_pid, const char *target_class, const char *target_title,
	int wx, int wy, int ww, int wh, bool has_win,
	bool inspect_mode, const char *filter,
	char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';
	if (!atspi_core_ensure_loaded()) return false;

	void *desktop = g_atspi.get_desktop(0);
	if (!desktop) return false;

	atspi_walk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.filter = filter;
	ctx.inspect_mode = inspect_mode;
	ctx.wx = wx;
	ctx.wy = wy;
	ctx.ww = ww;
	ctx.wh = wh;
	ctx.has_win = has_win;
	ctx.out = out;
	ctx.out_sz = out_sz;

	void *err = NULL;
	int n_apps = g_atspi.get_child_count(desktop, &err);
	bool walked_any = false;

	/* Pass 1: exact PID match */
	if (target_pid > 0 && g_atspi.get_pid) {
		for (int i = 0; i < n_apps && !ctx.truncated; i++) {
			err = NULL;
			void *app = g_atspi.get_child_at_index(desktop, i, &err);
			if (!app) continue;
			err = NULL;
			unsigned long apid = (unsigned long)g_atspi.get_pid(app, &err);
			if (apid == target_pid) {
				atspi_walk_node(app, 0, &ctx);
				walked_any = true;
			}
			g_atspi.g_unref(app);
			if (walked_any) break;
		}
	}

	/* Pass 2: fallback by WM_CLASS / title (for launcher/single-instance GUI apps) */
	if (!walked_any) {
		for (int i = 0; i < n_apps && !ctx.truncated; i++) {
			err = NULL;
			void *app = g_atspi.get_child_at_index(desktop, i, &err);
			if (!app) continue;
			err = NULL;
			char *aname = g_atspi.get_name(app, &err);
			if (atspi_app_matches_window(app, aname, target_pid, target_class, target_title)) {
				atspi_walk_node(app, 0, &ctx);
				walked_any = true;
			}
			if (aname) g_atspi.g_free(aname);
			g_atspi.g_unref(app);
		}
	}

	g_atspi.g_unref(desktop);
	if (ctx.truncated && ctx.off + 24 < out_sz) {
		snprintf(out + ctx.off, out_sz - ctx.off, "\n[truncated]");
	}
	return ctx.off > 0;
}

#endif /* JARVIS_DESKTOP_ATSPI_CORE_H */
