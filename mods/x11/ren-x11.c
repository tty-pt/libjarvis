/**
 * @file ren-x11.c
 * @brief Standalone ren extension for X11/EWMH/XTest input, windows, and clipboard.
 *
 * ZERO libjarvis dependency — works standalone with `ren -m ren-x11:ren-mm`.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ttypt/libren.h"
#include <ttypt/xy.h>
#include "x11_core.h"

static int tool_input(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: input <submit <text> | type <text> | key <combo> | click <x y | center> | scroll <up|down|left|right> [amount]>");
		return -1;
	}

	if (strncasecmp(args, "submit ", 7) == 0 || strncasecmp(args, "enter ", 6) == 0) {
		const char *txt = (args[0] == 's' || args[0] == 'S') ? (args + 7) : (args + 6);
		while (*txt == ' ') txt++;
		x11_core_type_text(txt, true);
		char buf[256];
		snprintf(buf, sizeof(buf), "typed+enter: %.120s", txt);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "type ", 5) == 0 || strncasecmp(args, "fill ", 5) == 0) {
		const char *txt = args + 5;
		while (*txt == ' ') txt++;
		x11_core_type_text(txt, false);
		char buf[256];
		snprintf(buf, sizeof(buf), "typed: %.120s", txt);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "key ", 4) == 0) {
		const char *k = args + 4;
		while (*k == ' ') k++;
		x11_core_press_key_combo(*k ? k : "Return");
		usleep(20000);
		char buf[160];
		snprintf(buf, sizeof(buf), "key: %s", *k ? k : "Return");
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "click", 5) == 0) {
		const char *rest = args + 5;
		while (*rest == ' ') rest++;
		int x = 0, y = 0;
		char extra = '\0';
		if (*rest && sscanf(rest, "%d %d %c", &x, &y, &extra) == 2) {
			/* parsed x y */
		} else if (*rest && sscanf(rest, "%d,%d %c", &x, &y, &extra) == 2) {
			/* parsed x,y */
		} else if (!*rest || strcasecmp(rest, "center") == 0) {
			int wx, wy, ww, wh;
			x11_core_get_geometry(NULL, &wx, &wy, &ww, &wh);
			x = wx + ww / 2;
			y = wy + wh / 2;
		} else {
			*out_result = strdup("Error: X11 input click accepts '<x> <y>' or 'center' (use browser tool for accessibility labels)");
			return -1;
		}
		x11_core_mouse_click(x, y, 1);
		char buf[128];
		snprintf(buf, sizeof(buf), "click at (%d,%d) button 1", x, y);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "scroll", 6) == 0) {
		const char *rest = args + 6;
		while (*rest == ' ') rest++;
		char dir[32] = "down";
		int amt = 5;
		if (*rest) sscanf(rest, "%31s %d", dir, &amt);
		if (amt <= 0) amt = 5;
		if (amt > 20) amt = 20;
		int btn = 5;
		if (strcasecmp(dir, "up") == 0) btn = 4;
		else if (strcasecmp(dir, "left") == 0) btn = 6;
		else if (strcasecmp(dir, "right") == 0) btn = 7;

		int wx, wy, ww, wh, mx, my;
		x11_core_get_geometry(NULL, &wx, &wy, &ww, &wh);
		x11_core_get_mouse_location(&mx, &my);
		bool need_move = !(mx >= wx && mx <= wx + ww && my >= wy && my <= wy + wh);
		x11_core_mouse_scroll(wx + ww / 2, wy + wh / 2, need_move, btn, amt);
		usleep(20000);
		char buf[128];
		snprintf(buf, sizeof(buf), "scrolled %s x%d", dir, amt);
		*out_result = strdup(buf);
		return 0;
	}

	*out_result = strdup("Error: usage: input <submit <text> | type <text> | key <combo> | click <x y | center> | scroll <up|down|left|right> [amount]>");
	return -1;
}

static int tool_x11_windows(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (args && strncasecmp(args, "focus ", 6) == 0) {
		const char *ident = args + 6;
		while (*ident == ' ') ident++;
		if (strncmp(ident, "0x", 2) == 0) {
			bool ok = x11_core_focus_ewmh(ident, 1.0);
			*out_result = strdup(ok ? "focused window" : "Error: failed to focus window");
			return ok ? 0 : -1;
		}
		x11_win_info_t clients[32];
		x11_win_info_t foc, oth;
		size_t n = x11_core_query_windows(clients, 32, &foc, &oth);
		for (int pass = 0; pass < 2; pass++) {
			for (size_t i = 0; i < n; i++) {
				if (pass == 0 && !clients[i].visible) continue;
				if (x11_core_str_ci(clients[i].wm_class, ident) ||
				    x11_core_str_ci(clients[i].title, ident)) {
					bool ok = x11_core_focus_ewmh(clients[i].id, 1.0);
					char buf[192];
					snprintf(buf, sizeof(buf), "%s %s: %.64s",
					         ok ? "focused" : "failed to focus",
					         clients[i].wm_class, clients[i].title);
					*out_result = strdup(buf);
					return ok ? 0 : -1;
				}
			}
		}
		*out_result = strdup("Error: window not found");
		return -1;
	}

	if (args && strncasecmp(args, "close ", 6) == 0) {
		const char *id = args + 6;
		while (*id == ' ') id++;
		bool ok = x11_core_close_ewmh(id);
		*out_result = strdup(ok ? "closed window" : "Error: invalid window id");
		return ok ? 0 : -1;
	}

	x11_win_info_t clients[32];
	x11_win_info_t foc, oth;
	size_t n = x11_core_query_windows(clients, 32, &foc, &oth);
	char out[2048];
	size_t off = 0;
	off += (size_t)snprintf(out + off, sizeof(out) - off, "{\"windows\":[");
	for (size_t i = 0; i < n && off + 160 < sizeof(out); i++) {
		char ec[128], et[200];
		x11_core_json_escape(clients[i].wm_class, ec, sizeof(ec));
		x11_core_json_escape(clients[i].title, et, sizeof(et));
		off += (size_t)snprintf(out + off, sizeof(out) - off,
		                        "%s{\"id\":\"%s\",\"class\":\"%s\",\"title\":\"%s\",\"tag\":%d,\"visible\":%s,\"focused\":%s}",
		                        i > 0 ? "," : "",
		                        clients[i].id, ec, et, clients[i].tag,
		                        clients[i].visible ? "true" : "false",
		                        clients[i].focused ? "true" : "false");
	}
	snprintf(out + off, sizeof(out) - off, "]}");
	*out_result = strdup(out);
	return 0;
}

static int tool_clipboard(const char *args, char **out_result)
{
	(void)args;
	char prim[768] = {0}, clip[768] = {0};
	x11_core_read_clipboard(prim, sizeof(prim), clip, sizeof(clip));
	char e_prim[900], e_clip[900], buf[2048];
	x11_core_json_escape(prim, e_prim, sizeof(e_prim));
	x11_core_json_escape(clip, e_clip, sizeof(e_clip));
	snprintf(buf, sizeof(buf), "{\"primary_selection\":\"%.750s\",\"clipboard\":\"%.750s\"}",
	         e_prim, e_clip);
	*out_result = strdup(buf);
	return 0;
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("input", "input <submit <text> | type <text> | key <combo> | click <x y | center> | scroll <up|down|left|right> [amount]>", tool_input);
	ren_register_tool("x11_windows", "x11_windows [focus <id_or_title> | close <id>]", tool_x11_windows);
	ren_register_tool("clipboard", "clipboard", tool_clipboard);
}
