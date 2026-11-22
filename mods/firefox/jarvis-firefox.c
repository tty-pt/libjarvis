/**
 * @file jarvis-firefox.c
 * @brief Firefox / Browser MozLZ4 & AT-SPI2 libxylem adapter plugin for libjarvis.
 *
 * ZERO link-time X11 or WM dependencies. Coordinates window focus, coordinate
 * clicks, and typing through jarvis_xy_* hooks, with an optional runtime dlopen
 * X11 geometry helper when running under X11 (e.g., dwm negative offscreen coords).
 */

#include "../../include/ttypt/jarvis-types.h"
#include "../../include/ttypt/jarvis-desktop.h"
#include <ttypt/xy.h>
#include <ttypt/xy-mod.h>

#include "firefox_core.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

XY_MODULE_API void xy_install(void)
{
}

typedef struct {
	void *(*open_display)(const char *);
	int   (*close_display)(void *);
	unsigned long (*default_root)(void *);
	unsigned long (*intern_atom)(void *, const char *, int);
	int   (*get_window_prop)(void *, unsigned long, unsigned long, long, long, int,
	                         unsigned long, unsigned long *, int *, unsigned long *,
	                         unsigned long *, unsigned char **);
	int   (*xfree)(void *);
	int   (*get_geometry)(void *, unsigned long, unsigned long *, int *, int *,
	                      unsigned int *, unsigned int *, unsigned int *, unsigned int *);
	int   (*translate_coords)(void *, unsigned long, unsigned long, int, int,
	                          int *, int *, unsigned long *);
	bool  tried;
	bool  ready;
} ff_lazy_x11_t;

static ff_lazy_x11_t g_lazy_x11 = {0};

static bool ensure_lazy_x11(void)
{
	if (g_lazy_x11.tried) return g_lazy_x11.ready;
	g_lazy_x11.tried = true;
	const char *dpy = getenv("DISPLAY");
	if (!dpy || !*dpy) return false;
	void *h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
	if (!h) h = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
	if (!h) return false;
	*(void **)&g_lazy_x11.open_display     = dlsym(h, "XOpenDisplay");
	*(void **)&g_lazy_x11.close_display    = dlsym(h, "XCloseDisplay");
	*(void **)&g_lazy_x11.default_root     = dlsym(h, "XDefaultRootWindow");
	*(void **)&g_lazy_x11.intern_atom      = dlsym(h, "XInternAtom");
	*(void **)&g_lazy_x11.get_window_prop  = dlsym(h, "XGetWindowProperty");
	*(void **)&g_lazy_x11.xfree            = dlsym(h, "XFree");
	*(void **)&g_lazy_x11.get_geometry     = dlsym(h, "XGetGeometry");
	*(void **)&g_lazy_x11.translate_coords = dlsym(h, "XTranslateCoordinates");
	if (g_lazy_x11.open_display && g_lazy_x11.close_display && g_lazy_x11.default_root &&
	    g_lazy_x11.intern_atom && g_lazy_x11.get_window_prop && g_lazy_x11.xfree &&
	    g_lazy_x11.get_geometry && g_lazy_x11.translate_coords) {
		g_lazy_x11.ready = true;
	}
	return g_lazy_x11.ready;
}

static bool ops_get_focused_geometry(void *ud, int *out_x, int *out_y, int *out_w, int *out_h)
{
	(void)ud;
	if (!ensure_lazy_x11()) return false;
	void *dpy = g_lazy_x11.open_display(NULL);
	if (!dpy) return false;
	unsigned long root = g_lazy_x11.default_root(dpy);
	unsigned long net_active = g_lazy_x11.intern_atom(dpy, "_NET_ACTIVE_WINDOW", 0);
	unsigned long actual_type = 0, nitems = 0, bytes_after = 0;
	int actual_format = 0;
	unsigned char *prop = NULL;
	unsigned long active_win = 0;
	if (g_lazy_x11.get_window_prop(dpy, root, net_active, 0, 1, 0, 33 /* XA_WINDOW */,
	                               &actual_type, &actual_format, &nitems, &bytes_after, &prop) == 0 && prop) {
		if (nitems > 0) active_win = *(unsigned long *)prop;
		g_lazy_x11.xfree(prop);
	}
	bool ok = false;
	if (active_win) {
		unsigned long child = 0, rret = 0;
		int rx = 0, ry = 0;
		unsigned int rw = 0, rh = 0, bw = 0, depth = 0;
		if (g_lazy_x11.get_geometry(dpy, active_win, &rret, &rx, &ry, &rw, &rh, &bw, &depth)) {
			int ax = rx, ay = ry;
			g_lazy_x11.translate_coords(dpy, active_win, root, 0, 0, &ax, &ay, &child);
			*out_x = ax;
			*out_y = ay;
			*out_w = (int)rw;
			*out_h = (int)rh;
			ok = true;
		}
	}
	g_lazy_x11.close_display(dpy);
	return ok;
}

static bool ops_focus_browser(void *ud);
static bool ops_is_browser_focused(void *ud);
static void ops_click_xy(void *ud, int x, int y, int btn);

static firefox_core_ops_t make_ff_ops(void)
{
	firefox_core_ops_t ops;
	ops.focus_browser        = ops_focus_browser;
	ops.is_browser_focused   = ops_is_browser_focused;
	ops.get_focused_geometry = ops_get_focused_geometry;
	ops.click_xy             = ops_click_xy;
	ops.ud                   = NULL;
	return ops;
}

static size_t world_tabs_to_ff(const jarvis_world_t *w, firefox_tab_t *out_tabs, size_t max_tabs)
{
	size_t n = 0;
	for (size_t i = 0; w && i < w->tab_count && n < max_tabs; i++) {
		out_tabs[n].index = w->tabs[i].index;
		snprintf(out_tabs[n].title, sizeof(out_tabs[n].title), "%.*s",
		         (int)(sizeof(out_tabs[n].title) - 1), w->tabs[i].title);
		snprintf(out_tabs[n].url, sizeof(out_tabs[n].url), "%.*s",
		         (int)(sizeof(out_tabs[n].url) - 1), w->tabs[i].url);
		n++;
	}
	return n;
}

static void inspect_world_browser(jarvis_world_t *w, const char *filter_q)
{
	firefox_tab_t ff_tabs[FIREFOX_CORE_MAX_TABS];
	size_t ntabs = firefox_core_populate_tabs(ff_tabs, FIREFOX_CORE_MAX_TABS);
	for (size_t i = 0; i < ntabs && w->tab_count < JARVIS_MAX_TABS; i++) {
		jarvis_tab_t *t = &w->tabs[w->tab_count++];
		t->index = ff_tabs[i].index;
		snprintf(t->title, sizeof(t->title), "%.*s", (int)(sizeof(t->title) - 1), ff_tabs[i].title);
		snprintf(t->url, sizeof(t->url), "%.*s", (int)(sizeof(t->url) - 1), ff_tabs[i].url);
	}
	firefox_core_ops_t ops = make_ff_ops();
	firefox_core_inspect_a11y(&ops, ff_tabs, ntabs, filter_q, w->browser_page, sizeof(w->browser_page));

	if (w->client_count > 0 && !strstr(w->browser_page, "[document web]")) {
		size_t off = strlen(w->browser_page);
		for (size_t i = 0; i < w->client_count && off + 128 < sizeof(w->browser_page); i++) {
			if (ff_is_browser_name(w->clients[i].wm_class) && !strstr(w->browser_page, w->clients[i].title)) {
				off += (size_t)snprintf(w->browser_page + off, sizeof(w->browser_page) - off,
				                        "[window] %s\n", w->clients[i].title);
			}
		}
	}
}

XY_IMPL(int, jarvis_xy_observe, jarvis_world_t *, w)
{
	if (!w) return 0;

	/* Always populate cached Firefox tabs if empty */
	if (w->tab_count == 0) {
		firefox_tab_t ff_tabs[FIREFOX_CORE_MAX_TABS];
		size_t ntabs = firefox_core_populate_tabs(ff_tabs, FIREFOX_CORE_MAX_TABS);
		for (size_t i = 0; i < ntabs && w->tab_count < JARVIS_MAX_TABS; i++) {
			jarvis_tab_t *t = &w->tabs[w->tab_count++];
			t->index = ff_tabs[i].index;
			snprintf(t->title, sizeof(t->title), "%.*s", (int)(sizeof(t->title) - 1), ff_tabs[i].title);
			snprintf(t->url, sizeof(t->url), "%.*s", (int)(sizeof(t->url) - 1), ff_tabs[i].url);
		}
	}

	bool browser_visible = false;
	for (size_t i = 0; i < w->client_count; i++) {
		if (w->clients[i].visible && ff_is_browser_name(w->clients[i].wm_class)) {
			browser_visible = true;
			break;
		}
	}

	if ((w->client_count == 0 || browser_visible) && !w->browser_page[0]) {
		firefox_tab_t ff_tabs[FIREFOX_CORE_MAX_TABS];
		size_t ntabs = world_tabs_to_ff(w, ff_tabs, FIREFOX_CORE_MAX_TABS);
		firefox_core_ops_t ops = make_ff_ops();
		firefox_core_inspect_a11y(&ops, ff_tabs, ntabs, NULL, w->browser_page, sizeof(w->browser_page));
	}
	return 0;
}

XY_IMPL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, res)
{
	if (!dsl || !res || res->handled) return 0;
	const char *op = dsl->op;
	firefox_core_ops_t ops = make_ff_ops();

	/* 1. focus_tab */
	if (strcmp(op, "focus_tab") == 0) {
		res->handled = true;
		const char *q = dsl->target.query[0] ? dsl->target.query : dsl->target.identifier;
		if (!q || !*q) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "focus_tab requires target.query");
			return 0;
		}
		ops_focus_browser(NULL);
		char tab_title[128] = {0};
		if (firefox_core_switch_tab(&ops, q, tab_title, sizeof(tab_title))) {
			jarvis_world_t w;
			memset(&w, 0, sizeof(w));
			w.browser_page[0] = '-';
			int oret = 0;
			XY_CALL(&oret, jarvis_xy_observe, &w);
			w.browser_page[0] = '\0';
			inspect_world_browser(&w, NULL);
			snprintf(res->output, sizeof(res->output),
			         "switched to tab '%s':\n%.1850s", tab_title, w.browser_page);
			res->success = true;
		} else {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "No open browser tab matching: %.200s", q);
		}
		return 0;
	}

	/* 2. inspect_browser */
	if (strcmp(op, "inspect_browser") == 0) {
		res->handled = true;
		ops_focus_browser(NULL);
		const char *filter_q = dsl->target.query[0] ? dsl->target.query : NULL;
		if (filter_q && *filter_q) {
			firefox_core_switch_tab(&ops, filter_q, NULL, 0);
		}
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		w.browser_page[0] = '-';
		int oret = 0;
		XY_CALL(&oret, jarvis_xy_observe, &w);
		w.browser_page[0] = '\0';
		inspect_world_browser(&w, filter_q);
		snprintf(res->output, sizeof(res->output), "%.2040s",
		         w.browser_page[0] ? w.browser_page : "(no browser accessibility tree found)");
		res->success = true;
		return 0;
	}

	/* 3. open_url: reuse existing matching Firefox tab if present; otherwise let jarvis-desktop open it */
	if (strcmp(op, "open_url") == 0 && dsl->target.url[0]) {
		char url[768];
		if (strncmp(dsl->target.url, "http://", 7) == 0 || strncmp(dsl->target.url, "https://", 8) == 0) {
			snprintf(url, sizeof(url), "%s", dsl->target.url);
		} else {
			snprintf(url, sizeof(url), "https://%s", dsl->target.url);
		}
		char domain_kw[64] = {0};
		firefox_core_extract_domain_kw(url, domain_kw, sizeof(domain_kw));
		if (strlen(domain_kw) >= 3 && ops_focus_browser(NULL)) {
			char tab_title[128] = {0};
			if (firefox_core_switch_tab(&ops, domain_kw, tab_title, sizeof(tab_title))) {
				jarvis_world_t w;
				memset(&w, 0, sizeof(w));
				inspect_world_browser(&w, NULL);
				snprintf(res->output, sizeof(res->output),
				         "focused existing tab '%s':\n%.1850s", tab_title, w.browser_page);
				res->handled = true;
				res->success = true;
				return 0;
			}
		}
		/* Not in existing tabs -> leave !res->handled so jarvis_desktop_execute_fallback spawns firefox --new-tab */
		return 0;
	}

	/* 4. Semantic element click by AT-SPI2 label */
	if (strcmp(op, "click") == 0) {
		int x = dsl->target.x, y = dsl->target.y;
		int btn = dsl->args.button > 0 ? dsl->args.button : 1;
		const char *q = dsl->target.query[0] ? dsl->target.query : dsl->target.identifier;
		if (x == 0 && y == 0 && q && *q) {
			res->handled = true;
			char clicked_role[64] = {0};
			if (firefox_core_click_element(&ops, q, btn, clicked_role, sizeof(clicked_role),
			                               res->output, sizeof(res->output))) {
				if (strcmp(clicked_role, "table row") == 0 ||
				    strcmp(clicked_role, "page tab") == 0 ||
				    strcmp(clicked_role, "list item") == 0) {
					usleep(120000);
					jarvis_world_t w;
					memset(&w, 0, sizeof(w));
					inspect_world_browser(&w, "entry");
					size_t cur_len = strlen(res->output);
					if (w.browser_page[0] && cur_len + 64 < sizeof(res->output)) {
						int rem = (int)(sizeof(res->output) - cur_len - 3);
						snprintf(res->output + cur_len, sizeof(res->output) - cur_len,
						         "\n%.*s", rem > 0 ? rem : 0, w.browser_page);
					}
				}
				res->success = true;
				return 0;
			}
			res->success = false;
			snprintf(res->error, sizeof(res->error), "UI element not found in accessibility tree: %.200s", q);
			return 0;
		}
		return 0;
	}

	/* 5. type_text / type_and_enter: pre-click target element & focus browser entry box,
	      then leave res->handled = false so jarvis-x11 injects the keystrokes */
	if ((strcmp(op, "type_text") == 0 || strcmp(op, "type_and_enter") == 0) &&
	    strcmp(dsl->target.relation, "other") != 0) {
		if (dsl->target.query[0]) {
			char click_msg[160] = {0};
			char clicked_role[64] = {0};
			if (!firefox_core_click_element(&ops, dsl->target.query, 1,
			                                clicked_role, sizeof(clicked_role),
			                                click_msg, sizeof(click_msg))) {
				res->handled = true;
				res->success = false;
				snprintf(res->error, sizeof(res->error),
				         "UI element not found in accessibility tree: %.200s", dsl->target.query);
				return 0;
			}
			if (strcmp(clicked_role, "entry") != 0 && strcmp(clicked_role, "combo box") != 0) {
				usleep(160000);
			}
			snprintf(res->output, sizeof(res->output), "%s", click_msg);
		}
		firefox_core_ensure_entry_focused(&ops);
		return 0;
	}

	return 0;
}

static bool ops_focus_browser(void *ud)
{
	(void)ud;
	static const char *const BROWSERS[] = { "firefox", "chromium", "chrome", "brave", NULL };
	jarvis_world_t w;
	memset(&w, 0, sizeof(w));
	w.browser_page[0] = '-';
	int oret = 0;
	XY_CALL(&oret, jarvis_xy_observe, &w);

	if (ff_is_browser_name(w.focused.wm_class)) return true;

	for (size_t i = 0; i < w.client_count; i++) {
		if (w.clients[i].visible && ff_is_browser_name(w.clients[i].wm_class)) {
			jarvis_dsl_t fd;
			jarvis_adapter_result_t fr;
			memset(&fd, 0, sizeof(fd));
			memset(&fr, 0, sizeof(fr));
			snprintf(fd.op, sizeof(fd.op), "focus_window");
			snprintf(fd.target.identifier, sizeof(fd.target.identifier), "%s", w.clients[i].id);
			int rret = 0;
			XY_CALL(&rret, jarvis_xy_execute, &fd, &fr);
			if (fr.handled && fr.success) return true;
		}
	}
	for (size_t b = 0; BROWSERS[b]; b++) {
		jarvis_dsl_t fd;
		jarvis_adapter_result_t fr;
		memset(&fd, 0, sizeof(fd));
		memset(&fr, 0, sizeof(fr));
		snprintf(fd.op, sizeof(fd.op), "focus_window");
		snprintf(fd.target.identifier, sizeof(fd.target.identifier), "%s", BROWSERS[b]);
		int rret = 0;
		XY_CALL(&rret, jarvis_xy_execute, &fd, &fr);
		if (fr.handled && fr.success) return true;
	}
	return false;
}

static bool ops_is_browser_focused(void *ud)
{
	(void)ud;
	jarvis_world_t w;
	memset(&w, 0, sizeof(w));
	w.browser_page[0] = '-';
	int oret = 0;
	XY_CALL(&oret, jarvis_xy_observe, &w);
	return ff_is_browser_name(w.focused.wm_class);
}

static void ops_click_xy(void *ud, int x, int y, int btn)
{
	(void)ud;
	jarvis_dsl_t cd;
	jarvis_adapter_result_t cr;
	memset(&cd, 0, sizeof(cd));
	memset(&cr, 0, sizeof(cr));
	snprintf(cd.op, sizeof(cd.op), "click");
	cd.target.x = x;
	cd.target.y = y;
	cd.args.button = btn > 0 ? btn : 1;
	int rret = 0;
	XY_CALL(&rret, jarvis_xy_execute, &cd, &cr);
}
