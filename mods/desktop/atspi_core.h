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
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define ATSPI_STATE_ACTIVE      1
#define ATSPI_STATE_DEFUNCT     6
#define ATSPI_STATE_EDITABLE    7
#define ATSPI_STATE_ENABLED     8
#define ATSPI_STATE_FOCUSABLE  11
#define ATSPI_STATE_FOCUSED    12
#define ATSPI_STATE_MULTI_LINE 17
#define ATSPI_STATE_SELECTED   23
#define ATSPI_STATE_SENSITIVE  24
#define ATSPI_STATE_SHOWING    25
#define ATSPI_SCROLL_ANYWHERE   6

typedef struct {
	int x;
	int y;
	int width;
	int height;
} atspi_rect_t;

typedef struct {
	unsigned long pid;
	const char   *cls;
	const char   *title;
	int           wx, wy, ww, wh;
	int           sw, sh;
	bool          has_win;
	bool          has_scr;
} atspi_win_ctx_t;

typedef struct {
	bool (*focus_win)(void *ud);
	bool (*is_win_focused)(void *ud);
	bool (*click_xy)(void *ud, int x, int y, int btn);
	bool (*send_key)(void *ud, const char *combo);
	bool (*type_text)(void *ud, const char *text, bool submit);
	void *ud;
} atspi_ops_t;

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
	int           (*comp_grab_focus)(void *, void **);
	int           (*comp_scroll_to)(void *, int, void **);
	void         *(*get_action_iface)(void *);
	int           (*action_get_n)(void *, void **);
	char         *(*action_get_name)(void *, int, void **);
	int           (*action_do)(void *, int, void **);
	void         *(*get_editable_text_iface)(void *);
	int           (*editable_set_text)(void *, const char *, void **);
	void         *(*get_value_iface)(void *);
	double        (*value_get_min)(void *, void **);
	double        (*value_get_max)(void *, void **);
	double        (*value_get_cur)(void *, void **);
	int           (*value_set_cur)(void *, double, void **);
	void         *(*get_state_set)(void *);
	int           (*state_set_contains)(void *, int);
	void         *(*g_ref)(void *);
	void          (*g_unref)(void *);
	void          (*g_free)(void *);
	void          (*g_err_free)(void *);
	bool          ready;
	bool          tried;
} atspi_syms_t;

static atspi_syms_t g_atspi = {0};

static void atspi_clear_err(void **err)
{
	if (err && *err) {
		if (g_atspi.g_err_free) g_atspi.g_err_free(*err);
		*err = NULL;
	}
}

static __attribute__((unused)) bool atspi_core_ensure_loaded(void)
{
	if (g_atspi.tried) return g_atspi.ready;
	g_atspi.tried = true;

	void *h_atspi = dlopen("libatspi.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_gobj  = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_glib  = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	if (!h_atspi || !h_gobj || !h_glib) return false;

	*(void **)&g_atspi.init                    = dlsym(h_atspi, "atspi_init");
	*(void **)&g_atspi.get_desktop             = dlsym(h_atspi, "atspi_get_desktop");
	*(void **)&g_atspi.get_child_count         = dlsym(h_atspi, "atspi_accessible_get_child_count");
	*(void **)&g_atspi.get_child_at_index      = dlsym(h_atspi, "atspi_accessible_get_child_at_index");
	*(void **)&g_atspi.get_name                = dlsym(h_atspi, "atspi_accessible_get_name");
	*(void **)&g_atspi.get_role_name           = dlsym(h_atspi, "atspi_accessible_get_role_name");
	*(void **)&g_atspi.get_pid                 = dlsym(h_atspi, "atspi_accessible_get_process_id");
	*(void **)&g_atspi.get_text_iface          = dlsym(h_atspi, "atspi_accessible_get_text_iface");
	*(void **)&g_atspi.text_get_text           = dlsym(h_atspi, "atspi_text_get_text");
	*(void **)&g_atspi.get_component_iface     = dlsym(h_atspi, "atspi_accessible_get_component_iface");
	*(void **)&g_atspi.comp_get_extents        = dlsym(h_atspi, "atspi_component_get_extents");
	*(void **)&g_atspi.comp_grab_focus         = dlsym(h_atspi, "atspi_component_grab_focus");
	*(void **)&g_atspi.comp_scroll_to          = dlsym(h_atspi, "atspi_component_scroll_to");
	*(void **)&g_atspi.get_action_iface        = dlsym(h_atspi, "atspi_accessible_get_action_iface");
	*(void **)&g_atspi.action_get_n            = dlsym(h_atspi, "atspi_action_get_n_actions");
	*(void **)&g_atspi.action_get_name         = dlsym(h_atspi, "atspi_action_get_action_name");
	*(void **)&g_atspi.action_do               = dlsym(h_atspi, "atspi_action_do_action");
	*(void **)&g_atspi.get_editable_text_iface = dlsym(h_atspi, "atspi_accessible_get_editable_text_iface");
	*(void **)&g_atspi.editable_set_text       = dlsym(h_atspi, "atspi_editable_text_set_text_contents");
	*(void **)&g_atspi.get_value_iface         = dlsym(h_atspi, "atspi_accessible_get_value_iface");
	*(void **)&g_atspi.value_get_min           = dlsym(h_atspi, "atspi_value_get_minimum_value");
	*(void **)&g_atspi.value_get_max           = dlsym(h_atspi, "atspi_value_get_maximum_value");
	*(void **)&g_atspi.value_get_cur           = dlsym(h_atspi, "atspi_value_get_current_value");
	*(void **)&g_atspi.value_set_cur           = dlsym(h_atspi, "atspi_value_set_current_value");
	*(void **)&g_atspi.get_state_set           = dlsym(h_atspi, "atspi_accessible_get_state_set");
	*(void **)&g_atspi.state_set_contains      = dlsym(h_atspi, "atspi_state_set_contains");
	*(void **)&g_atspi.g_ref                   = dlsym(h_gobj, "g_object_ref");
	*(void **)&g_atspi.g_unref                 = dlsym(h_gobj, "g_object_unref");
	*(void **)&g_atspi.g_free                  = dlsym(h_glib, "g_free");
	*(void **)&g_atspi.g_err_free              = dlsym(h_glib, "g_error_free");

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

static bool atspi_node_is_enabled(void *node)
{
	if (!node || !g_atspi.get_state_set || !g_atspi.state_set_contains) return true;
	void *ss = g_atspi.get_state_set(node);
	if (!ss) return true;
	bool en = (g_atspi.state_set_contains(ss, ATSPI_STATE_ENABLED) != 0);
	bool sens = (g_atspi.state_set_contains(ss, ATSPI_STATE_SENSITIVE) != 0);
	g_atspi.g_unref(ss);
	return en && sens;
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
	       strcmp(role, "tree item") == 0 ||
	       strcmp(role, "page tab") == 0 ||
	       strcmp(role, "menu item") == 0 ||
	       strcmp(role, "check menu item") == 0 ||
	       strcmp(role, "radio menu item") == 0 ||
	       strcmp(role, "slider") == 0 ||
	       strcmp(role, "spin button") == 0 ||
	       strcmp(role, "table row") == 0;
}

static bool atspi_is_editable_role(const char *role)
{
	if (!role) return false;
	return strcmp(role, "entry") == 0 ||
	       strcmp(role, "password text") == 0 ||
	       strcmp(role, "combo box") == 0 ||
	       strcmp(role, "spin button") == 0;
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
	const char            *filter;
	bool                   inspect_mode;
	const atspi_win_ctx_t *win;
	char                  *out;
	size_t                 out_sz;
	size_t                 off;
	bool                   truncated;
	int                    visited;
	char                   last_line[384];
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
	atspi_clear_err(&err);
	g_atspi.g_unref(tif);
	return txt;
}

static bool atspi_node_center(
	void *node, const atspi_win_ctx_t *win, int *out_cx, int *out_cy, bool *out_in_vp)
{
	if (out_in_vp) *out_in_vp = false;
	if (!node || !g_atspi.get_component_iface || !g_atspi.comp_get_extents) return false;
	void *comp = g_atspi.get_component_iface(node);
	if (!comp) return false;
	void *err = NULL;
	atspi_rect_t *r = g_atspi.comp_get_extents(comp, 0, &err);
	atspi_clear_err(&err);
	if ((!r || r->width <= 0 || r->height <= 0) && win && win->has_win) {
		if (r) g_atspi.g_free(r);
		err = NULL;
		r = g_atspi.comp_get_extents(comp, 1, &err);
		atspi_clear_err(&err);
		if (r && r->width > 0 && r->height > 0) {
			r->x += win->wx;
			r->y += win->wy;
		}
	}
	g_atspi.g_unref(comp);
	if (!r) return false;
	int cx = r->x + r->width / 2;
	int cy = r->y + r->height / 2;
	int rw = r->width, rh = r->height;
	g_atspi.g_free(r);
	if (rw <= 0 || rh <= 0) return false;

	bool in_vp = true;
	if (win && win->has_win && win->ww > 0 && win->wh > 0) {
		if (cx < win->wx || cx >= win->wx + win->ww || cy < win->wy || cy >= win->wy + win->wh) {
			in_vp = false;
		}
	} else if (cx < 0 || cy < 0) {
		in_vp = false;
	}
	if (in_vp && win && win->has_scr && win->sw > 0 && win->sh > 0) {
		if (cx < 0 || cy < 0 || cx >= win->sw || cy >= win->sh) {
			in_vp = false;
		}
	}
	if (out_cx) *out_cx = cx;
	if (out_cy) *out_cy = cy;
	if (out_in_vp) *out_in_vp = in_vp;
	return true;
}

static bool atspi_node_has_action(void *node)
{
	if (!node || !g_atspi.get_action_iface || !g_atspi.action_get_n) return false;
	void *act = g_atspi.get_action_iface(node);
	if (!act) return false;
	void *err = NULL;
	int n = g_atspi.action_get_n(act, &err);
	atspi_clear_err(&err);
	g_atspi.g_unref(act);
	return n > 0;
}

static bool atspi_do_node_action(void *node, char *out_act, size_t act_sz)
{
	if (out_act && act_sz > 0) out_act[0] = '\0';
	if (!node || !g_atspi.get_action_iface || !g_atspi.action_do) return false;
	void *act = g_atspi.get_action_iface(node);
	if (!act) return false;
	void *err = NULL;
	int n = g_atspi.action_get_n ? g_atspi.action_get_n(act, &err) : 1;
	atspi_clear_err(&err);
	bool did = false;
	if (n > 0) {
		int chosen = 0;
		if (g_atspi.action_get_name) {
			for (int i = 0; i < n && i < 8; i++) {
				void *nerr = NULL;
				char *an = g_atspi.action_get_name(act, i, &nerr);
				atspi_clear_err(&nerr);
				if (an) {
					bool pref = (strcasecmp(an, "click") == 0 ||
					             strcasecmp(an, "activate") == 0 ||
					             strcasecmp(an, "press") == 0 ||
					             strcasecmp(an, "toggle") == 0 ||
					             strcasecmp(an, "open") == 0);
					if (pref) {
						chosen = i;
						if (out_act && act_sz > 0) snprintf(out_act, act_sz, "%s", an);
						g_atspi.g_free(an);
						break;
					}
					if (i == 0 && out_act && act_sz > 0) snprintf(out_act, act_sz, "%s", an);
					g_atspi.g_free(an);
				}
			}
		}
		err = NULL;
		did = (g_atspi.action_do(act, chosen, &err) != 0);
		atspi_clear_err(&err);
	}
	g_atspi.g_unref(act);
	return did;
}

static bool atspi_grab_node_focus(void *node)
{
	if (!node || !g_atspi.get_component_iface || !g_atspi.comp_grab_focus) return false;
	void *comp = g_atspi.get_component_iface(node);
	if (!comp) return false;
	void *err = NULL;
	bool ok = (g_atspi.comp_grab_focus(comp, &err) != 0);
	atspi_clear_err(&err);
	g_atspi.g_unref(comp);
	return ok;
}

static bool atspi_scroll_node_into_view(
	void *node, const atspi_win_ctx_t *win, int *out_cx, int *out_cy, bool *out_in_vp)
{
	if (!node || !g_atspi.get_component_iface || !g_atspi.comp_scroll_to) return false;
	void *comp = g_atspi.get_component_iface(node);
	if (!comp) return false;
	void *err = NULL;
	g_atspi.comp_scroll_to(comp, ATSPI_SCROLL_ANYWHERE, &err);
	atspi_clear_err(&err);
	g_atspi.g_unref(comp);
	usleep(120000);
	return atspi_node_center(node, win, out_cx, out_cy, out_in_vp);
}

static void atspi_walk_node(void *node, int depth, atspi_walk_ctx_t *ctx)
{
	if (!node || !ctx || depth > 24 || ctx->visited > 3500 || ctx->truncated) return;
	ctx->visited++;

	if (atspi_has_state(node, ATSPI_STATE_DEFUNCT)) return;

	void *err = NULL;
	char *role = g_atspi.get_role_name(node, &err);
	atspi_clear_err(&err);
	err = NULL;
	char *name = g_atspi.get_name(node, &err);
	atspi_clear_err(&err);
	err = NULL;
	int n_children = g_atspi.get_child_count(node, &err);
	atspi_clear_err(&err);

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
				if (atspi_node_center(node, ctx->win, &cx, &cy, &in_vp)) {
					if (in_vp) snprintf(pos, sizeof(pos), " @(%d,%d)", cx, cy);
					else snprintf(pos, sizeof(pos), " *OFF-SCREEN*");
				}
				bool focused = atspi_has_state(node, ATSPI_STATE_FOCUSED);
				bool selected = atspi_has_state(node, ATSPI_STATE_SELECTED);
				bool enabled = atspi_node_is_enabled(node);
				char line[320];
				snprintf(line, sizeof(line), "[%s%s%s%s%s] %.150s\n",
				         role, pos,
				         focused ? " *FOCUSED*" : "",
				         selected ? " *ACTIVE*" : "",
				         enabled ? "" : " *DISABLED*",
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
		atspi_clear_err(&err);
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
		atspi_clear_err(&err);
		if (apid == target_pid) return true;
	}
	if (aname && *aname) {
		if (target_class && *target_class && strcasestr(target_class, aname)) return true;
		if (target_class && *target_class && strcasestr(aname, target_class)) return true;
	}
	if (target_title && *target_title) {
		void *err = NULL;
		int nc = g_atspi.get_child_count(app, &err);
		atspi_clear_err(&err);
		if (nc > 12) nc = 12;
		for (int i = 0; i < nc; i++) {
			err = NULL;
			void *ch = g_atspi.get_child_at_index(app, i, &err);
			atspi_clear_err(&err);
			if (!ch) continue;
			err = NULL;
			char *cname = g_atspi.get_name(ch, &err);
			atspi_clear_err(&err);
			bool hit = (cname && *cname && strcasestr(target_title, cname)) ||
			           (cname && *cname && strcasestr(cname, target_title));
			if (cname) g_atspi.g_free(cname);
			g_atspi.g_unref(ch);
			if (hit) return true;
		}
	}
	return false;
}

static bool atspi_app_has_active_frame(void *app)
{
	if (!app) return false;
	void *err = NULL;
	int nc = g_atspi.get_child_count(app, &err);
	atspi_clear_err(&err);
	if (nc > 12) nc = 12;
	for (int i = 0; i < nc; i++) {
		err = NULL;
		void *ch = g_atspi.get_child_at_index(app, i, &err);
		atspi_clear_err(&err);
		if (!ch) continue;
		bool act = atspi_has_state(ch, ATSPI_STATE_ACTIVE) && atspi_has_state(ch, ATSPI_STATE_SHOWING);
		g_atspi.g_unref(ch);
		if (act) return true;
	}
	return false;
}

typedef void (*atspi_app_visit_fn)(void *app, void *ud);

static void atspi_for_each_target_app(const atspi_win_ctx_t *win, atspi_app_visit_fn fn, void *ud)
{
	if (!fn || !atspi_core_ensure_loaded()) return;
	void *desktop = g_atspi.get_desktop(0);
	if (!desktop) return;

	unsigned long tpid = win ? win->pid : 0;
	const char *tcls = win ? win->cls : NULL;
	const char *ttitle = win ? win->title : NULL;

	void *err = NULL;
	int n_apps = g_atspi.get_child_count(desktop, &err);
	atspi_clear_err(&err);
	bool walked_any = false;

	/* Pass 1: exact PID match */
	if (tpid > 0 && g_atspi.get_pid) {
		for (int i = 0; i < n_apps; i++) {
			err = NULL;
			void *app = g_atspi.get_child_at_index(desktop, i, &err);
			atspi_clear_err(&err);
			if (!app) continue;
			err = NULL;
			unsigned long apid = (unsigned long)g_atspi.get_pid(app, &err);
			atspi_clear_err(&err);
			if (apid == tpid) {
				fn(app, ud);
				walked_any = true;
			}
			g_atspi.g_unref(app);
			if (walked_any) break;
		}
	}

	/* Pass 2: fallback by WM_CLASS / title (for launcher/single-instance GUI apps) */
	if (!walked_any && ((tcls && *tcls) || (ttitle && *ttitle))) {
		for (int i = 0; i < n_apps; i++) {
			err = NULL;
			void *app = g_atspi.get_child_at_index(desktop, i, &err);
			atspi_clear_err(&err);
			if (!app) continue;
			err = NULL;
			char *aname = g_atspi.get_name(app, &err);
			atspi_clear_err(&err);
			if (atspi_app_matches_window(app, aname, tpid, tcls, ttitle)) {
				fn(app, ud);
				walked_any = true;
			}
			if (aname) g_atspi.g_free(aname);
			g_atspi.g_unref(app);
		}
	}

	/* Pass 3: Wayland / no-WM metadata fallback: active AT-SPI2 frame */
	if (!walked_any && tpid == 0 && (!tcls || !*tcls) && (!ttitle || !*ttitle)) {
		for (int i = 0; i < n_apps; i++) {
			err = NULL;
			void *app = g_atspi.get_child_at_index(desktop, i, &err);
			atspi_clear_err(&err);
			if (!app) continue;
			if (atspi_app_has_active_frame(app)) {
				fn(app, ud);
				walked_any = true;
			}
			g_atspi.g_unref(app);
			if (walked_any) break;
		}
	}

	g_atspi.g_unref(desktop);
}

static void atspi_visit_read_app(void *app, void *ud)
{
	atspi_walk_ctx_t *ctx = (atspi_walk_ctx_t *)ud;
	if (!ctx->truncated) atspi_walk_node(app, 0, ctx);
}

static __attribute__((unused)) bool atspi_core_read_window(
	const atspi_win_ctx_t *win,
	bool inspect_mode, const char *filter,
	char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';

	atspi_walk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.filter = filter;
	ctx.inspect_mode = inspect_mode;
	ctx.win = win;
	ctx.out = out;
	ctx.out_sz = out_sz;

	atspi_for_each_target_app(win, atspi_visit_read_app, &ctx);
	if (ctx.truncated && ctx.off + 24 < out_sz) {
		snprintf(out + ctx.off, out_sz - ctx.off, "\n[truncated]");
	}
	return ctx.off > 0;
}

typedef enum {
	ATSPI_INTENT_CLICK = 0,
	ATSPI_INTENT_FOCUS,
	ATSPI_INTENT_EDIT,
	ATSPI_INTENT_VALUE
} atspi_intent_t;

typedef struct {
	const char            *query;
	atspi_intent_t         intent;
	const atspi_win_ctx_t *win;
	int                    best_score;
	void                  *best_node;
	int                    best_cx, best_cy;
	bool                   best_have_coords;
	bool                   best_in_vp;
	bool                   best_editable;
	char                   best_role[64];
	char                   best_label[128];
	int                    best_dis_score;
	char                   best_dis_role[64];
	char                   best_dis_label[128];
	int                    visited;
} atspi_find_ctx_t;

static int atspi_score_text_field(const char *field, const char *query, int exact_pts, int prefix_pts, int sub_pts)
{
	if (!field || !*field || !query || !*query) return 0;
	if (strcasecmp(field, query) == 0) return exact_pts;
	size_t qlen = strlen(query);
	if (strncasecmp(field, query, qlen) == 0) return prefix_pts;
	if (strcasestr(field, query) != NULL) return sub_pts;
	return 0;
}

static int atspi_score_candidate(
	void *node, const char *role, const char *cname, const char *ctxt,
	atspi_intent_t intent, const char *query)
{
	if (!role || !query || !*query) return -1;
	if (strcmp(role, "application") == 0 || strcmp(role, "frame") == 0 ||
	    strcmp(role, "dialog") == 0 || strcmp(role, "document web") == 0 ||
	    strcmp(role, "document frame") == 0) {
		return -1;
	}

	int n_pts = atspi_score_text_field(cname, query, 100, 70, 45);
	int t_pts = atspi_score_text_field(ctxt, query, 85, 55, 30);
	int text_pts = (n_pts >= t_pts) ? n_pts : t_pts;

	bool is_inter = atspi_is_interactive_role(role);
	bool is_edit = atspi_is_editable_role(role) || atspi_has_state(node, ATSPI_STATE_EDITABLE);
	bool is_role_hit = (is_inter || is_edit) && (strcasestr(role, query) != NULL);

	if (text_pts == 0 && !is_role_hit) return -1;

	bool is_focusable = atspi_has_state(node, ATSPI_STATE_FOCUSABLE);
	bool is_val_role = (strcmp(role, "slider") == 0 || strcmp(role, "spin button") == 0);
	bool has_act = false;
	if (!is_inter && !is_edit) {
		if (intent == ATSPI_INTENT_FOCUS && is_focusable) {
			/* focusable non-standard widget allowed */
		} else {
			has_act = atspi_node_has_action(node);
			if (!has_act) return -1;
		}
	}

	int score = 10 + (text_pts > 0 ? text_pts : 18);
	if (is_inter) score += 40;
	if (has_act) score += 30;
	if (intent == ATSPI_INTENT_EDIT && is_edit) score += 55;
	else if (intent == ATSPI_INTENT_FOCUS && (is_focusable || is_edit)) score += 40;
	else if (intent == ATSPI_INTENT_VALUE && (is_val_role || is_edit)) score += 55;
	if (atspi_has_state(node, ATSPI_STATE_SHOWING)) score += 15;
	return score;
}

static void atspi_walk_find(void *node, int depth, atspi_find_ctx_t *ctx)
{
	if (!node || !ctx || depth > 24 || ctx->visited > 3500) return;
	ctx->visited++;
	if (atspi_has_state(node, ATSPI_STATE_DEFUNCT)) return;

	void *err = NULL;
	char *role = g_atspi.get_role_name(node, &err);
	atspi_clear_err(&err);
	err = NULL;
	char *name = g_atspi.get_name(node, &err);
	atspi_clear_err(&err);

	if (role) {
		if ((strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0 ||
		     strcmp(role, "frame") == 0 || strcmp(role, "dialog") == 0) &&
		    !atspi_has_state(node, ATSPI_STATE_SHOWING)) {
			g_atspi.g_free(role);
			if (name) g_atspi.g_free(name);
			return;
		}

		char cname[180] = {0}, ctxt[180] = {0};
		atspi_core_clean_text(name, cname, sizeof(cname));
		if ((cname[0] && strcasestr(cname, ctx->query)) ||
		    atspi_is_interactive_role(role) || atspi_has_state(node, ATSPI_STATE_EDITABLE)) {
			char *txt = atspi_read_node_text(node, 200);
			atspi_core_clean_text(txt, ctxt, sizeof(ctxt));
			if (txt) g_atspi.g_free(txt);
		}

		int score = atspi_score_candidate(node, role, cname, ctxt, ctx->intent, ctx->query);
		if (score > 0) {
			const char *disp = cname[0] ? cname : (ctxt[0] ? ctxt : role);
			int cx = 0, cy = 0;
			bool in_vp = false;
			bool have_coords = atspi_node_center(node, ctx->win, &cx, &cy, &in_vp);
			if (have_coords && in_vp) score += 25;

			if (!atspi_node_is_enabled(node)) {
				if (score > ctx->best_dis_score) {
					ctx->best_dis_score = score;
					snprintf(ctx->best_dis_role, sizeof(ctx->best_dis_role), "%.63s", role);
					snprintf(ctx->best_dis_label, sizeof(ctx->best_dis_label), "%.120s", disp);
				}
			} else if (score > ctx->best_score) {
				ctx->best_score = score;
				ctx->best_cx = cx;
				ctx->best_cy = cy;
				ctx->best_have_coords = have_coords;
				ctx->best_in_vp = in_vp;
				ctx->best_editable = (atspi_is_editable_role(role) ||
				                      atspi_has_state(node, ATSPI_STATE_EDITABLE));
				if (ctx->best_node) {
					g_atspi.g_unref(ctx->best_node);
					ctx->best_node = NULL;
				}
				if (g_atspi.g_ref) ctx->best_node = g_atspi.g_ref(node);
				snprintf(ctx->best_role, sizeof(ctx->best_role), "%.63s", role);
				snprintf(ctx->best_label, sizeof(ctx->best_label), "%.120s", disp);
			}
		}
	}

	if (role) g_atspi.g_free(role);
	if (name) g_atspi.g_free(name);

	err = NULL;
	int nc = g_atspi.get_child_count(node, &err);
	atspi_clear_err(&err);
	if (nc > 150) nc = 150;
	for (int i = 0; i < nc; i++) {
		err = NULL;
		void *ch = g_atspi.get_child_at_index(node, i, &err);
		atspi_clear_err(&err);
		if (ch) {
			atspi_walk_find(ch, depth + 1, ctx);
			g_atspi.g_unref(ch);
		}
	}
}

static void atspi_visit_find_app(void *app, void *ud)
{
	atspi_walk_find(app, 0, (atspi_find_ctx_t *)ud);
}

static bool atspi_find_best_node(
	const atspi_win_ctx_t *win, const char *query, atspi_intent_t intent,
	atspi_find_ctx_t *out_ctx, char *out_err, size_t err_sz)
{
	memset(out_ctx, 0, sizeof(*out_ctx));
	out_ctx->query = query;
	out_ctx->intent = intent;
	out_ctx->win = win;
	if (out_err && err_sz > 0) out_err[0] = '\0';

	if (!query || !*query) {
		if (out_err && err_sz > 0) snprintf(out_err, err_sz, "Error: target control label is required");
		return false;
	}
	atspi_for_each_target_app(win, atspi_visit_find_app, out_ctx);
	if (out_ctx->best_score > 0 && out_ctx->best_node) return true;

	if (out_ctx->best_node) {
		g_atspi.g_unref(out_ctx->best_node);
		out_ctx->best_node = NULL;
	}
	if (out_err && err_sz > 0) {
		if (out_ctx->best_dis_score > 0) {
			snprintf(out_err, err_sz, "Error: control [%s] '%.64s' is disabled",
			         out_ctx->best_dis_role, out_ctx->best_dis_label);
		} else {
			snprintf(out_err, err_sz, "Error: UI element not found in accessibility tree: %.200s", query);
		}
	}
	return false;
}

static bool atspi_ensure_win_focused(const atspi_ops_t *ops, char *out_err, size_t err_sz)
{
	if (!ops || !ops->is_win_focused) return true;
	if (ops->is_win_focused(ops->ud)) return true;
	if (ops->focus_win) {
		ops->focus_win(ops->ud);
		usleep(40000);
		if (ops->is_win_focused(ops->ud)) return true;
	}
	if (out_err && err_sz > 0) snprintf(out_err, err_sz, "Error: target window is not focused");
	return false;
}

static __attribute__((unused)) bool atspi_core_click(
	const atspi_win_ctx_t *win, const atspi_ops_t *ops,
	const char *query, int btn,
	char *out_msg, size_t msg_sz)
{
	if (out_msg && msg_sz > 0) out_msg[0] = '\0';
	atspi_find_ctx_t f;
	if (!atspi_find_best_node(win, query, ATSPI_INTENT_CLICK, &f, out_msg, msg_sz)) {
		return false;
	}

	if (!f.best_in_vp) {
		atspi_scroll_node_into_view(f.best_node, win, &f.best_cx, &f.best_cy, &f.best_in_vp);
		if (f.best_in_vp) f.best_have_coords = true;
	}

	if (btn == 3) {
		if (!f.best_have_coords || !f.best_in_vp || !ops || !ops->click_xy) {
			if (out_msg && msg_sz > 0) {
				snprintf(out_msg, msg_sz,
				         "Error: right-click requires visible coordinates for [%s] '%.64s'",
				         f.best_role, f.best_label);
			}
			g_atspi.g_unref(f.best_node);
			return false;
		}
		if (!atspi_ensure_win_focused(ops, out_msg, msg_sz)) {
			g_atspi.g_unref(f.best_node);
			return false;
		}
		bool ok = ops->click_xy(ops->ud, f.best_cx, f.best_cy, 3);
		usleep(80000);
		if (out_msg && msg_sz > 0) {
			snprintf(out_msg, msg_sz, ok ? "right-clicked [%s] '%.64s' at (%d,%d)"
			                             : "Error: input click failed on [%s] '%.64s'",
			         f.best_role, f.best_label, f.best_cx, f.best_cy);
		}
		g_atspi.g_unref(f.best_node);
		return ok;
	}

	/* Left-click: prefer X11 pointer click when visible in viewport, fall back to AtspiAction */
	if (f.best_have_coords && f.best_in_vp && ops && ops->click_xy) {
		if (!atspi_ensure_win_focused(ops, out_msg, msg_sz)) {
			g_atspi.g_unref(f.best_node);
			return false;
		}
		if (f.best_editable || atspi_has_state(f.best_node, ATSPI_STATE_FOCUSABLE)) {
			atspi_grab_node_focus(f.best_node);
		}
		if (ops->click_xy(ops->ud, f.best_cx, f.best_cy, 1)) {
			usleep(80000);
			if (out_msg && msg_sz > 0) {
				snprintf(out_msg, msg_sz, "clicked [%s] '%.64s' at (%d,%d)",
				         f.best_role, f.best_label, f.best_cx, f.best_cy);
			}
			g_atspi.g_unref(f.best_node);
			return true;
		}
	}

	char act_name[48] = {0};
	if (atspi_do_node_action(f.best_node, act_name, sizeof(act_name))) {
		usleep(80000);
		if (out_msg && msg_sz > 0) {
			snprintf(out_msg, msg_sz, "activated [%s] '%.64s' via native action%s%s",
			         f.best_role, f.best_label,
			         act_name[0] ? " '" : "", act_name[0] ? act_name : "");
			if (act_name[0] && strlen(out_msg) + 2 < msg_sz) strcat(out_msg, "'");
		}
		g_atspi.g_unref(f.best_node);
		return true;
	}

	if (out_msg && msg_sz > 0) {
		snprintf(out_msg, msg_sz,
		         f.best_in_vp ? "Error: failed to click or activate [%s] '%.64s'"
		                      : "Error: control [%s] '%.64s' is off-screen and has no native action",
		         f.best_role, f.best_label);
	}
	g_atspi.g_unref(f.best_node);
	return false;
}

static __attribute__((unused)) bool atspi_core_focus(
	const atspi_win_ctx_t *win, const atspi_ops_t *ops,
	const char *query, char *out_msg, size_t msg_sz)
{
	if (out_msg && msg_sz > 0) out_msg[0] = '\0';
	atspi_find_ctx_t f;
	if (!atspi_find_best_node(win, query, ATSPI_INTENT_FOCUS, &f, out_msg, msg_sz)) {
		return false;
	}
	if (!atspi_ensure_win_focused(ops, out_msg, msg_sz)) {
		g_atspi.g_unref(f.best_node);
		return false;
	}
	if (!f.best_in_vp) {
		atspi_scroll_node_into_view(f.best_node, win, &f.best_cx, &f.best_cy, &f.best_in_vp);
		if (f.best_in_vp) f.best_have_coords = true;
	}

	bool ok = atspi_grab_node_focus(f.best_node) || atspi_has_state(f.best_node, ATSPI_STATE_FOCUSED);
	if (!ok && f.best_have_coords && f.best_in_vp && ops && ops->click_xy) {
		ok = ops->click_xy(ops->ud, f.best_cx, f.best_cy, 1);
	}
	if (ok) {
		usleep(40000);
		if (out_msg && msg_sz > 0) {
			snprintf(out_msg, msg_sz, "focused [%s] '%.64s'", f.best_role, f.best_label);
		}
	} else if (out_msg && msg_sz > 0) {
		snprintf(out_msg, msg_sz, "Error: failed to focus [%s] '%.64s'", f.best_role, f.best_label);
	}
	g_atspi.g_unref(f.best_node);
	return ok;
}

typedef struct {
	char role[64];
	char label[128];
	int  cx;
	int  cy;
	bool have_coords;
	bool in_vp;
	bool multi_line;
} atspi_edit_cand_t;

typedef struct {
	const atspi_win_ctx_t *win;
	bool                   focused_found;
	char                   focused_role[64];
	char                   focused_label[128];
	atspi_edit_cand_t      cands[16];
	int                    n_cands;
	int                    visited;
} atspi_edit_probe_t;

static void atspi_walk_editable(void *node, int depth, atspi_edit_probe_t *ctx)
{
	if (!node || !ctx || depth > 24 || ctx->visited > 3000 || ctx->focused_found) return;
	ctx->visited++;
	if (atspi_has_state(node, ATSPI_STATE_DEFUNCT)) return;

	void *err = NULL;
	char *role = g_atspi.get_role_name(node, &err);
	atspi_clear_err(&err);
	err = NULL;
	char *name = g_atspi.get_name(node, &err);
	atspi_clear_err(&err);

	bool skip_children = false;
	if (role) {
		if ((strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0 ||
		     strcmp(role, "frame") == 0 || strcmp(role, "dialog") == 0) &&
		    !atspi_has_state(node, ATSPI_STATE_SHOWING)) {
			g_atspi.g_free(role);
			if (name) g_atspi.g_free(name);
			return;
		}

		if (strcmp(role, "application") != 0 && strcmp(role, "frame") != 0 &&
		    strcmp(role, "dialog") != 0 && strcmp(role, "document web") != 0 &&
		    strcmp(role, "document frame") != 0) {
			bool is_edit = atspi_is_editable_role(role) || atspi_has_state(node, ATSPI_STATE_EDITABLE);
			if (is_edit && atspi_node_is_enabled(node) && atspi_has_state(node, ATSPI_STATE_SHOWING)) {
				bool has_focus = atspi_has_state(node, ATSPI_STATE_FOCUSED);
				if (!has_focus) {
					void *cerr = NULL;
					int nc = g_atspi.get_child_count(node, &cerr);
					atspi_clear_err(&cerr);
					for (int i = 0; i < nc && i < 4; i++) {
						cerr = NULL;
						void *ch = g_atspi.get_child_at_index(node, i, &cerr);
						atspi_clear_err(&cerr);
						if (ch) {
							if (atspi_has_state(ch, ATSPI_STATE_FOCUSED)) has_focus = true;
							g_atspi.g_unref(ch);
							if (has_focus) break;
						}
					}
				}

				char cname[128] = {0};
				atspi_core_clean_text(name, cname, sizeof(cname));
				const char *disp = cname[0] ? cname : "(unlabeled)";
				if (has_focus) {
					ctx->focused_found = true;
					snprintf(ctx->focused_role, sizeof(ctx->focused_role), "%.63s", role);
					snprintf(ctx->focused_label, sizeof(ctx->focused_label), "%.127s", disp);
				} else if (ctx->n_cands < 16) {
					int cx = 0, cy = 0;
					bool in_vp = false;
					bool hc = atspi_node_center(node, ctx->win, &cx, &cy, &in_vp);
					if (hc && in_vp) {
						atspi_edit_cand_t *c = &ctx->cands[ctx->n_cands++];
						snprintf(c->role, sizeof(c->role), "%.63s", role);
						snprintf(c->label, sizeof(c->label), "%.127s", disp);
						c->cx = cx;
						c->cy = cy;
						c->have_coords = hc;
						c->in_vp = in_vp;
						c->multi_line = atspi_has_state(node, ATSPI_STATE_MULTI_LINE);
					}
				}
				skip_children = true;
			}
		}
	}

	if (role) g_atspi.g_free(role);
	if (name) g_atspi.g_free(name);
	if (skip_children || ctx->focused_found) return;

	err = NULL;
	int nc = g_atspi.get_child_count(node, &err);
	atspi_clear_err(&err);
	if (nc > 150) nc = 150;
	for (int i = 0; i < nc && !ctx->focused_found; i++) {
		err = NULL;
		void *ch = g_atspi.get_child_at_index(node, i, &err);
		atspi_clear_err(&err);
		if (ch) {
			atspi_walk_editable(ch, depth + 1, ctx);
			g_atspi.g_unref(ch);
		}
	}
}

static void atspi_visit_probe_app(void *app, void *ud)
{
	atspi_edit_probe_t *ctx = (atspi_edit_probe_t *)ud;
	if (!ctx->focused_found) atspi_walk_editable(app, 0, ctx);
}

static __attribute__((unused)) bool atspi_core_prepare_input(
	const atspi_win_ctx_t *win, const atspi_ops_t *ops,
	const char *target, bool is_terminal,
	char *out_role, size_t role_sz,
	char *out_label, size_t label_sz,
	char *out_err, size_t err_sz)
{
	if (out_role && role_sz > 0) out_role[0] = '\0';
	if (out_label && label_sz > 0) out_label[0] = '\0';
	if (out_err && err_sz > 0) out_err[0] = '\0';

	if (!atspi_ensure_win_focused(ops, out_err, err_sz)) return false;

	if (target && *target) {
		atspi_find_ctx_t f;
		if (!atspi_find_best_node(win, target, ATSPI_INTENT_EDIT, &f, out_err, err_sz)) {
			return false;
		}
		if (!f.best_in_vp) {
			atspi_scroll_node_into_view(f.best_node, win, &f.best_cx, &f.best_cy, &f.best_in_vp);
			if (f.best_in_vp) f.best_have_coords = true;
		}
		bool already_focused = atspi_has_state(f.best_node, ATSPI_STATE_FOCUSED);
		if (!already_focused || !f.best_editable) {
			atspi_grab_node_focus(f.best_node);
			if (f.best_have_coords && f.best_in_vp && ops && ops->click_xy) {
				ops->click_xy(ops->ud, f.best_cx, f.best_cy, 1);
				usleep(f.best_editable ? 60000 : 150000);
			} else {
				atspi_do_node_action(f.best_node, NULL, 0);
				usleep(f.best_editable ? 60000 : 150000);
			}
		}
		if (f.best_editable) {
			if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", f.best_role);
			if (out_label && label_sz > 0) snprintf(out_label, label_sz, "%s", f.best_label);
			g_atspi.g_unref(f.best_node);
			return atspi_ensure_win_focused(ops, out_err, err_sz);
		}
		g_atspi.g_unref(f.best_node);
		/* Non-editable target activated (e.g. tab/list row); fall through to probe editable */
	}

	if (is_terminal) {
		if (out_role && role_sz > 0) snprintf(out_role, role_sz, "terminal");
		if (out_label && label_sz > 0 && win && win->title) {
			snprintf(out_label, label_sz, "%.120s", win->title);
		}
		return atspi_ensure_win_focused(ops, out_err, err_sz);
	}

	atspi_edit_probe_t probe;
	memset(&probe, 0, sizeof(probe));
	probe.win = win;
	atspi_for_each_target_app(win, atspi_visit_probe_app, &probe);

	if (probe.focused_found) {
		if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", probe.focused_role);
		if (out_label && label_sz > 0) snprintf(out_label, label_sz, "%s", probe.focused_label);
		return atspi_ensure_win_focused(ops, out_err, err_sz);
	}

	int chosen = -1;
	if (probe.n_cands == 1) {
		chosen = 0;
	} else if (probe.n_cands > 1) {
		int m_idx = -1, n_multi = 0;
		for (int i = 0; i < probe.n_cands; i++) {
			if (probe.cands[i].multi_line) {
				m_idx = i;
				n_multi++;
			}
		}
		if (n_multi == 1) chosen = m_idx;
	}

	if (chosen >= 0) {
		const atspi_edit_cand_t *c = &probe.cands[chosen];
		if (ops && ops->click_xy) {
			ops->click_xy(ops->ud, c->cx, c->cy, 1);
			usleep(70000);
		}
		if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", c->role);
		if (out_label && label_sz > 0) snprintf(out_label, label_sz, "%s", c->label);
		return atspi_ensure_win_focused(ops, out_err, err_sz);
	}

	if (out_err && err_sz > 0) {
		if (probe.n_cands == 0) {
			snprintf(out_err, err_sz, "Error: no visible editable input found in window");
		} else {
			size_t off = (size_t)snprintf(
				out_err, err_sz,
				"Error: no input focused and %d editable inputs visible (", probe.n_cands);
			for (int i = 0; i < probe.n_cands && i < 4 && off + 48 < err_sz; i++) {
				off += (size_t)snprintf(
					out_err + off, err_sz - off,
					"%s[%s '%.32s']", i > 0 ? ", " : "",
					probe.cands[i].role, probe.cands[i].label);
			}
			snprintf(out_err + off, err_sz - off, "); specify target with '<target> | <text>'");
		}
	}
	return false;
}

static __attribute__((unused)) bool atspi_core_set(
	const atspi_win_ctx_t *win, const atspi_ops_t *ops,
	const char *target, const char *val,
	char *out_msg, size_t msg_sz)
{
	if (out_msg && msg_sz > 0) out_msg[0] = '\0';
	if (!val || !*val) {
		if (out_msg && msg_sz > 0) snprintf(out_msg, msg_sz, "Error: value is required");
		return false;
	}

	atspi_find_ctx_t f;
	if (!atspi_find_best_node(win, target, ATSPI_INTENT_VALUE, &f, out_msg, msg_sz)) {
		return false;
	}

	/* 1. Try numeric AtspiValue (sliders / spin buttons) when val is numeric */
	char *endp = NULL;
	errno = 0;
	double num = strtod(val, &endp);
	while (endp && *endp && isspace((unsigned char)*endp)) endp++;
	bool is_num = (endp != val && endp && *endp == '\0' && errno != ERANGE);

	if (is_num && g_atspi.get_value_iface && g_atspi.value_set_cur) {
		void *valif = g_atspi.get_value_iface(f.best_node);
		if (valif) {
			double v = num;
			if (g_atspi.value_get_min && g_atspi.value_get_max) {
				void *merr = NULL;
				double vmin = g_atspi.value_get_min(valif, &merr);
				atspi_clear_err(&merr);
				double vmax = g_atspi.value_get_max(valif, &merr);
				atspi_clear_err(&merr);
				if (vmax > vmin) {
					if (v < vmin) v = vmin;
					if (v > vmax) v = vmax;
				}
			}
			void *verr = NULL;
			bool ok = (g_atspi.value_set_cur(valif, v, &verr) != 0);
			atspi_clear_err(&verr);
			g_atspi.g_unref(valif);
			if (ok) {
				if (out_msg && msg_sz > 0) {
					snprintf(out_msg, msg_sz, "set [%s] '%.64s' value=%.6g",
					         f.best_role, f.best_label, v);
				}
				g_atspi.g_unref(f.best_node);
				return true;
			}
		}
	}

	/* 2. Try AtspiEditableText replacement */
	if (g_atspi.get_editable_text_iface && g_atspi.editable_set_text) {
		void *etif = g_atspi.get_editable_text_iface(f.best_node);
		if (etif) {
			void *terr = NULL;
			bool ok = (g_atspi.editable_set_text(etif, val, &terr) != 0);
			atspi_clear_err(&terr);
			g_atspi.g_unref(etif);
			if (ok) {
				if (out_msg && msg_sz > 0) {
					snprintf(out_msg, msg_sz, "set [%s] '%.64s' text='%.80s'",
					         f.best_role, f.best_label, val);
				}
				g_atspi.g_unref(f.best_node);
				return true;
			}
		}
	}

	/* 3. Fallback: focus/click + Ctrl+A + BackSpace + input type */
	if (!f.best_in_vp) {
		atspi_scroll_node_into_view(f.best_node, win, &f.best_cx, &f.best_cy, &f.best_in_vp);
		if (f.best_in_vp) f.best_have_coords = true;
	}
	if (!atspi_ensure_win_focused(ops, out_msg, msg_sz)) {
		g_atspi.g_unref(f.best_node);
		return false;
	}
	bool focused = atspi_grab_node_focus(f.best_node) || atspi_has_state(f.best_node, ATSPI_STATE_FOCUSED);
	if (f.best_have_coords && f.best_in_vp && ops && ops->click_xy) {
		if (ops->click_xy(ops->ud, f.best_cx, f.best_cy, 1)) focused = true;
		usleep(60000);
	}
	if (!focused || !ops || !ops->send_key || !ops->type_text) {
		if (out_msg && msg_sz > 0) {
			snprintf(out_msg, msg_sz, "Error: control [%s] '%.64s' does not support setting value/text",
			         f.best_role, f.best_label);
		}
		g_atspi.g_unref(f.best_node);
		return false;
	}

	if (!atspi_ensure_win_focused(ops, out_msg, msg_sz)) {
		g_atspi.g_unref(f.best_node);
		return false;
	}
	ops->send_key(ops->ud, "ctrl+a");
	usleep(25000);
	ops->send_key(ops->ud, "BackSpace");
	usleep(25000);
	bool typed = ops->type_text(ops->ud, val, false);
	if (out_msg && msg_sz > 0) {
		snprintf(out_msg, msg_sz, typed ? "set [%s] '%.64s' text='%.80s'"
		                                : "Error: failed to type value into [%s] '%.64s'",
		         f.best_role, f.best_label, val);
	}
	g_atspi.g_unref(f.best_node);
	return typed;
}

#endif /* JARVIS_DESKTOP_ATSPI_CORE_H */
