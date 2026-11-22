/**
 * @file jarvis-x11.c
 * @brief Generic X11 / EWMH / XTest libxylem adapter plugin for libjarvis.
 *
 * Provides platform-clean X11 observation, keyboard/mouse synthesis (XTest),
 * scroll, click, clipboard reading, and EWMH window focus/close fallbacks.
 */

#include "../../include/ttypt/jarvis-types.h"
#include "../../include/ttypt/jarvis-desktop.h"
#include <ttypt/xy.h>
#include <ttypt/xy-mod.h>

#include "x11_core.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

XY_MODULE_API void xy_install(void)
{
}

static void fill_jarvis_win(jarvis_window_t *dst, const x11_win_info_t *src)
{
	if (!dst || !src) return;
	snprintf(dst->id, sizeof(dst->id), "%.*s", (int)(sizeof(dst->id) - 1), src->id);
	snprintf(dst->wm_class, sizeof(dst->wm_class), "%.*s", (int)(sizeof(dst->wm_class) - 1), src->wm_class);
	snprintf(dst->title, sizeof(dst->title), "%.*s", (int)(sizeof(dst->title) - 1), src->title);
	dst->tag = src->tag;
	dst->visible = src->visible;
	dst->focused = src->focused;
}

static unsigned long find_pid_by_id(const x11_win_info_t *clients, size_t n, const char *id)
{
	if (!id || !*id) return 0;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(clients[i].id, id) == 0) return clients[i].pid;
	}
	return 0;
}

XY_IMPL(int, jarvis_xy_observe, jarvis_world_t *, w)
{
	if (!w) return 0;
	if (!w->wm[0]) {
		snprintf(w->wm, sizeof(w->wm), "x11");
	}

	x11_win_info_t xclients[JARVIS_MAX_CLIENTS];
	x11_win_info_t xfoc, xoth;
	size_t xn = x11_core_query_windows(xclients, JARVIS_MAX_CLIENTS, &xfoc, &xoth);

	if (w->client_count == 0 && xn > 0) {
		fill_jarvis_win(&w->focused, &xfoc);
		fill_jarvis_win(&w->other, &xoth);
		for (size_t i = 0; i < xn && w->client_count < JARVIS_MAX_CLIENTS; i++) {
			fill_jarvis_win(&w->clients[w->client_count++], &xclients[i]);
		}
	}

	if (!w->focused_cwd[0]) {
		const jarvis_window_t *term_win = &w->focused;
		if (!x11_core_is_terminal_class(term_win->wm_class) &&
		    x11_core_is_terminal_class(w->other.wm_class)) {
			term_win = &w->other;
		}
		unsigned long pid = find_pid_by_id(xclients, xn, term_win->id);
		if (pid > 0) {
			jarvis_desktop_resolve_pid_cwd_fg(pid,
				w->focused_cwd, sizeof(w->focused_cwd),
				w->focused_process, sizeof(w->focused_process));
		}
	}

	return 0;
}

static bool focus_window_via_xy_or_ewmh(const char *wid_str);

XY_IMPL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, res)
{
	if (!dsl || !res || res->handled) return 0;
	const char *op = dsl->op;

	/* 1. Keyboard typing & shortcuts */
	if (strcmp(op, "type_text") == 0 || strcmp(op, "type_and_enter") == 0 || strcmp(op, "press_key") == 0) {
		res->handled = true;
		x11_win_info_t xclients[JARVIS_MAX_CLIENTS];
		x11_win_info_t xfoc, xoth;
		bool to_other = (strcmp(dsl->target.relation, "other") == 0);
		if (to_other) {
			x11_core_query_windows(xclients, JARVIS_MAX_CLIENTS, &xfoc, &xoth);
			if (!xoth.id[0]) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "No other window to type into");
				return 0;
			}
			focus_window_via_xy_or_ewmh(xoth.id);
		}

		if (strcmp(op, "press_key") == 0) {
			const char *combo = dsl->args.key_combo[0] ? dsl->args.key_combo : "Return";
			x11_core_press_key_combo(combo);
			usleep(20000);
			snprintf(res->output, sizeof(res->output), "key: %s", combo);
		} else {
			bool enter = (strcmp(op, "type_and_enter") == 0);
			char prefix[256] = {0};
			if (res->output[0]) {
				snprintf(prefix, sizeof(prefix), "%.*s", (int)(sizeof(prefix) - 1), res->output);
			}
			x11_core_type_text(dsl->args.text, enter);
			if (prefix[0]) {
				snprintf(res->output, sizeof(res->output), "%s -> %s: %.80s",
				         prefix, enter ? "typed+enter" : "typed", dsl->args.text);
			} else {
				snprintf(res->output, sizeof(res->output), "%s: %.80s",
				         enter ? "typed+enter" : "typed", dsl->args.text);
			}
		}

		if (to_other && xfoc.id[0]) {
			focus_window_via_xy_or_ewmh(xfoc.id);
		}
		res->success = true;
		return 0;
	}

	/* 2. Coordinate click (or focused window center if no query given) */
	if (strcmp(op, "click") == 0) {
		int x = dsl->target.x, y = dsl->target.y;
		int btn = dsl->args.button > 0 ? dsl->args.button : 1;
		const char *q = dsl->target.query[0] ? dsl->target.query : dsl->target.identifier;
		if (x == 0 && y == 0 && q && *q) {
			/* Element-query clicks are handled by jarvis-firefox / AT-SPI2 */
			res->handled = true;
			res->success = false;
			snprintf(res->error, sizeof(res->error), "UI element not found or browser accessibility unavailable: %.200s", q);
			return 0;
		}
		res->handled = true;
		if (x == 0 && y == 0) {
			int wx, wy, ww, wh;
			x11_core_get_geometry(NULL, &wx, &wy, &ww, &wh);
			x = wx + ww / 2;
			y = wy + wh / 2;
		}
		x11_core_mouse_click(x, y, btn);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "click at (%d,%d) button %d", x, y, btn);
		return 0;
	}

	/* 3. Mouse scroll */
	if (strcmp(op, "scroll") == 0) {
		res->handled = true;
		const char *dir = dsl->args.direction[0] ? dsl->args.direction : "down";
		int amount = dsl->args.amount > 0 ? dsl->args.amount : 5;
		if (amount > 20) amount = 20;
		int btn = 5;
		if (strcasecmp(dir, "up") == 0) btn = 4;
		else if (strcasecmp(dir, "left") == 0) btn = 6;
		else if (strcasecmp(dir, "right") == 0) btn = 7;

		int wx, wy, ww, wh, mx, my;
		x11_core_get_geometry(NULL, &wx, &wy, &ww, &wh);
		x11_core_get_mouse_location(&mx, &my);
		bool need_move = !(mx >= wx && mx <= wx + ww && my >= wy && my <= wy + wh);
		x11_core_mouse_scroll(wx + ww / 2, wy + wh / 2, need_move, btn, amount);
		usleep(20000);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "scrolled %s x%d", dir, amount);
		return 0;
	}

	/* 4. Tab close via Ctrl+W */
	if (strcmp(op, "window_close") == 0 && strcasecmp(dsl->target.kind, "tab") == 0) {
		res->handled = true;
		x11_core_press_key_combo("ctrl+w");
		res->success = true;
		snprintf(res->output, sizeof(res->output), "closed browser tab");
		return 0;
	}

	/* 5. X11 Clipboard inspection */
	if (strcmp(op, "inspect_clipboard") == 0) {
		res->handled = true;
		char prim[768] = {0}, clip[768] = {0};
		x11_core_read_clipboard(prim, sizeof(prim), clip, sizeof(clip));
		snprintf(res->output, sizeof(res->output),
		         "{\"primary_selection\":\"%.700s\",\"clipboard\":\"%.700s\"}", prim, clip);
		res->success = true;
		return 0;
	}

	/* 6. X11 Window list inspection */
	if (strcmp(op, "inspect_windows") == 0) {
		res->handled = true;
		x11_win_info_t xclients[JARVIS_MAX_CLIENTS];
		x11_win_info_t xfoc, xoth;
		size_t xn = x11_core_query_windows(xclients, JARVIS_MAX_CLIENTS, &xfoc, &xoth);
		size_t off = 0;
		off += (size_t)snprintf(res->output + off, sizeof(res->output) - off, "{\"windows\":[");
		for (size_t i = 0; i < xn && off + 160 < sizeof(res->output); i++) {
			char ec[128], et[200];
			x11_core_json_escape(xclients[i].wm_class, ec, sizeof(ec));
			x11_core_json_escape(xclients[i].title, et, sizeof(et));
			off += (size_t)snprintf(res->output + off, sizeof(res->output) - off,
			                        "%s{\"id\":\"%s\",\"class\":\"%s\",\"title\":\"%s\",\"tag\":%d,\"visible\":%s,\"focused\":%s}",
			                        i > 0 ? "," : "",
			                        xclients[i].id, ec, et, xclients[i].tag,
			                        xclients[i].visible ? "true" : "false",
			                        xclients[i].focused ? "true" : "false");
		}
		snprintf(res->output + off, sizeof(res->output) - off, "]}");
		res->success = true;
		return 0;
	}

	/* 7. Generic EWMH focus_window / window_close fallback (only if WM plugin didn't claim) */
	if (strcmp(op, "focus_window") == 0) {
		res->handled = true;
		const char *ident = dsl->target.identifier[0] ? dsl->target.identifier :
		                    (dsl->target.title_contains[0] ? dsl->target.title_contains : dsl->target.class_contains);
		if (!ident || !*ident) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "focus_window requires target.identifier");
			return 0;
		}
		if (strncmp(ident, "0x", 2) == 0) {
			res->success = x11_core_focus_ewmh(ident, 1.2);
			snprintf(res->output, sizeof(res->output), "focused window %s", ident);
			return 0;
		}
		x11_win_info_t xclients[JARVIS_MAX_CLIENTS];
		x11_win_info_t xfoc, xoth;
		size_t xn = x11_core_query_windows(xclients, JARVIS_MAX_CLIENTS, &xfoc, &xoth);
		for (int pass = 0; pass < 2; pass++) {
			for (size_t i = 0; i < xn; i++) {
				if (pass == 0 && !xclients[i].visible) continue;
				if (x11_core_str_ci(xclients[i].wm_class, ident) ||
				    x11_core_str_ci(xclients[i].title, ident)) {
					res->success = x11_core_focus_ewmh(xclients[i].id, 1.2);
					snprintf(res->output, sizeof(res->output), "focused %s: %.40s",
					         xclients[i].wm_class, xclients[i].title);
					return 0;
				}
			}
		}
		res->success = false;
		snprintf(res->error, sizeof(res->error), "Window not found: %s", ident);
		return 0;
	}

	if (strcmp(op, "window_close") == 0) {
		res->handled = true;
		x11_win_info_t xclients[JARVIS_MAX_CLIENTS];
		x11_win_info_t xfoc, xoth;
		x11_core_query_windows(xclients, JARVIS_MAX_CLIENTS, &xfoc, &xoth);
		const char *wid = NULL;
		if (strcmp(dsl->target.relation, "other") == 0 && xoth.id[0]) wid = xoth.id;
		else if (dsl->target.identifier[0]) wid = dsl->target.identifier;
		else if (xfoc.id[0]) wid = xfoc.id;
		if (wid && x11_core_close_ewmh(wid)) {
			res->success = true;
			snprintf(res->output, sizeof(res->output), "closed window %s", wid);
		} else {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "No window to close");
		}
		return 0;
	}

	return 0;
}

static bool focus_window_via_xy_or_ewmh(const char *wid_str)
{
	if (!wid_str || !*wid_str) return false;
	jarvis_dsl_t fd;
	jarvis_adapter_result_t fr;
	memset(&fd, 0, sizeof(fd));
	memset(&fr, 0, sizeof(fr));
	snprintf(fd.op, sizeof(fd.op), "focus_window");
	snprintf(fd.target.identifier, sizeof(fd.target.identifier), "%s", wid_str);
	int rret = 0;
	XY_CALL(&rret, jarvis_xy_execute, &fd, &fr);
	if (fr.handled) return fr.success;
	return x11_core_focus_ewmh(wid_str, 1.0);
}
