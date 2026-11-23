/**
 * @file ren-desktop.c
 * @brief Standalone ren extension for desktop app launch, media/volume, system health, and tmux capture.
 *
 * ZERO libjarvis or libX11 dependency — works standalone via `ren -m ren-desktop:ren-mm`.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "ttypt/libren.h"
#include "../../include/ttypt/desktop-core.h"
#include "atspi_core.h"
#include <ttypt/xy.h>

static bool desktop_extract_json_str(const char *json, const char *key, char *out, size_t out_sz)
{
	if (!json || !key || !out || out_sz == 0) return false;
	out[0] = '\0';
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	const char *p = strstr(json, pat);
	if (!p) return false;
	p += strlen(pat);
	size_t w = 0;
	while (*p && *p != '"' && w + 1 < out_sz) {
		if (*p == '\\' && p[1]) p++;
		out[w++] = *p++;
	}
	out[w] = '\0';
	return w > 0;
}

static bool desktop_extract_json_int(const char *json, const char *key, int *out_val)
{
	if (!json || !key || !out_val) return false;
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(json, pat);
	if (!p) return false;
	p += strlen(pat);
	*out_val = atoi(p);
	return true;
}

static unsigned long desktop_extract_json_ulong(const char *json, const char *key)
{
	if (!json || !key) return 0;
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(json, pat);
	if (!p) return 0;
	return strtoul(p + strlen(pat), NULL, 10);
}

static bool desktop_find_new_window(
	const char *before_json, const char *after_json, char *out_desc, size_t out_sz)
{
	if (!after_json || !out_desc || out_sz == 0) return false;
	out_desc[0] = '\0';
	const char *p = after_json;
	while ((p = strstr(p, "{\"id\":\"")) != NULL) {
		const char *id_start = p + 7;
		const char *id_end = strchr(id_start, '"');
		if (!id_end || id_end == id_start || (size_t)(id_end - id_start) > 32) {
			p += 7;
			continue;
		}
		char needle[48];
		snprintf(needle, sizeof(needle), "\"id\":\"%.*s\"", (int)(id_end - id_start), id_start);
		if (!before_json || !strstr(before_json, needle)) {
			const char *obj_end = strchr(p, '}');
			size_t obj_len = obj_end ? (size_t)(obj_end - p + 1) : strlen(p);
			char obj[512];
			if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
			memcpy(obj, p, obj_len);
			obj[obj_len] = '\0';

			char cls[96] = {0}, title[128] = {0};
			desktop_extract_json_str(obj, "class", cls, sizeof(cls));
			desktop_extract_json_str(obj, "title", title, sizeof(title));
			snprintf(out_desc, out_sz, "%s '%.64s'", cls[0] ? cls : "window", title);
			return true;
		}
		p = id_end;
	}
	return false;
}

static int tool_open_app(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: open_app <application_name>");
		return -1;
	}
	char resolved[256];
	if (!desktop_core_resolve_known_app(args, resolved, sizeof(resolved))) {
		char err[320];
		snprintf(err, sizeof(err), "Error: unknown application: %.240s", args);
		*out_result = strdup(err);
		return -1;
	}

	char *before_wins = NULL;
	bool have_x11 = (ren_call_tool("x11_windows", "", &before_wins) == 0 && before_wins != NULL);

	if (desktop_core_spawn_detached_sh(resolved) < 0) {
		free(before_wins);
		*out_result = strdup("Error: failed to launch application");
		return -1;
	}

	char mapped_desc[256] = {0}, focused_desc[256] = {0};
	if (have_x11) {
		for (int i = 0; i < 12; i++) {
			if (ren_is_interrupted()) break;
			usleep(100000);
			char *now_wins = NULL;
			if (ren_call_tool("x11_windows", "", &now_wins) == 0 && now_wins) {
				bool found = desktop_find_new_window(before_wins, now_wins, mapped_desc, sizeof(mapped_desc));
				free(now_wins);
				if (found) {
					usleep(60000);
					break;
				}
			} else {
				free(now_wins);
				break;
			}
		}
		char *foc_json = NULL;
		if (ren_call_tool("x11_windows", "focused", &foc_json) == 0 && foc_json) {
			char fcls[96] = {0}, ftitle[128] = {0};
			desktop_extract_json_str(foc_json, "class", fcls, sizeof(fcls));
			desktop_extract_json_str(foc_json, "title", ftitle, sizeof(ftitle));
			if (fcls[0] || ftitle[0]) {
				snprintf(focused_desc, sizeof(focused_desc), "%s '%.64s'",
				         fcls[0] ? fcls : "window", ftitle);
			}
		}
		free(foc_json);
	}
	free(before_wins);

	char buf[768];
	if (mapped_desc[0] && focused_desc[0]) {
		snprintf(buf, sizeof(buf), "launched %.200s (mapped: %s; focused: %s)",
		         resolved, mapped_desc, focused_desc);
	} else if (mapped_desc[0]) {
		snprintf(buf, sizeof(buf), "launched %.240s (mapped: %s)", resolved, mapped_desc);
	} else if (focused_desc[0]) {
		snprintf(buf, sizeof(buf), "launched %.240s (focused: %s)", resolved, focused_desc);
	} else {
		snprintf(buf, sizeof(buf), "launched %.320s", resolved);
	}
	*out_result = strdup(buf);
	return 0;
}

static int tool_media(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: media <play [query] | pause | next | prev | volume_up | volume_down | mute>");
		return -1;
	}

	if (strncasecmp(args, "play", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
		const char *q = args + 4;
		while (*q == ' ') q++;
		if (*q) {
			desktop_core_mpris_control("Pause");
			char enc[512], yt_url[768], resolve_cmd[1024], vid_id[64] = {0};
			desktop_core_url_encode(q, enc, sizeof(enc));
			snprintf(
				resolve_cmd, sizeof(resolve_cmd),
				"curl -s -L --max-time 3.5 -A 'Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0' "
				"'https://www.youtube.com/results?search_query=%s' 2>/dev/null | "
				"grep -o '\"videoId\":\"[a-zA-Z0-9_-]\\{11\\}\"' | head -n 1 | cut -d'\"' -f4",
				enc
			);
			desktop_core_run_cmd_capture(resolve_cmd, vid_id, sizeof(vid_id));
			if (strlen(vid_id) == 11) {
				snprintf(yt_url, sizeof(yt_url), "https://www.youtube.com/watch?v=%s&autoplay=1", vid_id);
			} else {
				snprintf(yt_url, sizeof(yt_url), "https://www.youtube.com/results?search_query=%s", enc);
			}
			char *const argv[] = { "firefox", "--new-tab", yt_url, NULL };
			if (desktop_core_spawn_detached_argv(argv) <= 0) {
				char *const xdg[] = { "xdg-open", yt_url, NULL };
				desktop_core_spawn_detached_argv(xdg);
			}
			char buf[384];
			snprintf(buf, sizeof(buf), "playing: %.320s", q);
			*out_result = strdup(buf);
			return 0;
		}
		if (!desktop_core_mpris_control("Play")) {
			ren_call_tool("input", "key XF86AudioPlay", NULL);
		}
		*out_result = strdup("resumed media");
		return 0;
	}

	if (strcasecmp(args, "pause") == 0 || strcasecmp(args, "stop") == 0) {
		if (!desktop_core_mpris_control("Pause")) {
			ren_call_tool("input", "key XF86AudioPlay", NULL);
		}
		*out_result = strdup("paused media");
		return 0;
	}

	if (strcasecmp(args, "next") == 0) {
		desktop_core_mpris_control("Next");
		*out_result = strdup("next track");
		return 0;
	}

	if (strcasecmp(args, "prev") == 0 || strcasecmp(args, "previous") == 0) {
		desktop_core_mpris_control("Previous");
		*out_result = strdup("previous track");
		return 0;
	}

	if (strcasecmp(args, "volume_up") == 0 || strcasecmp(args, "up") == 0) {
		desktop_core_spawn_detached_sh(
			"wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+ 2>/dev/null || "
			"pactl set-sink-volume @DEFAULT_SINK@ +5% 2>/dev/null");
		*out_result = strdup("volume: volume_up");
		return 0;
	}

	if (strcasecmp(args, "volume_down") == 0 || strcasecmp(args, "down") == 0) {
		desktop_core_spawn_detached_sh(
			"wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%- 2>/dev/null || "
			"pactl set-sink-volume @DEFAULT_SINK@ -5% 2>/dev/null");
		*out_result = strdup("volume: volume_down");
		return 0;
	}

	if (strcasecmp(args, "mute") == 0 || strcasecmp(args, "volume_mute") == 0) {
		desktop_core_spawn_detached_sh(
			"wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle 2>/dev/null || "
			"pactl set-sink-mute @DEFAULT_SINK@ toggle 2>/dev/null");
		*out_result = strdup("volume: volume_mute");
		return 0;
	}

	*out_result = strdup("Error: usage: media <play [query] | pause | next | prev | volume_up | volume_down | mute>");
	return -1;
}

static int tool_system(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	const char *aspect = (args && *args) ? args : "all";

	char health[256] = {0}, top_cpu[768] = {0}, top_mem[768] = {0}, disk[512] = {0}, ports[768] = {0};
	desktop_core_populate_system_health(health, sizeof(health));

	if (strcasecmp(aspect, "all") == 0 || strcasecmp(aspect, "memory") == 0 ||
	    strcasecmp(aspect, "ram") == 0 || strcasecmp(aspect, "processes") == 0 ||
	    strcasecmp(aspect, "cpu") == 0) {
		desktop_core_run_cmd_capture("ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head -n 7", top_cpu, sizeof(top_cpu));
		desktop_core_run_cmd_capture("ps -eo pid,comm,%cpu,%mem --sort=-%mem | head -n 7", top_mem, sizeof(top_mem));
	}
	if (strcasecmp(aspect, "all") == 0 || strcasecmp(aspect, "disk") == 0 || strcasecmp(aspect, "storage") == 0) {
		struct statvfs vfs_root;
		if (statvfs("/", &vfs_root) == 0 && vfs_root.f_blocks > 0) {
			double tot_gb = (double)(vfs_root.f_blocks * vfs_root.f_frsize) / (1024.0 * 1024.0 * 1024.0);
			double avail_gb = (double)(vfs_root.f_bavail * vfs_root.f_frsize) / (1024.0 * 1024.0 * 1024.0);
			double used_gb = tot_gb - avail_gb;
			snprintf(disk, sizeof(disk), "/: %.1fG used / %.1fG total (%.1fG free)", used_gb, tot_gb, avail_gb);
		}
	}
	if (strcasecmp(aspect, "all") == 0 || strcasecmp(aspect, "ports") == 0 || strcasecmp(aspect, "network") == 0) {
		desktop_core_run_cmd_capture("ss -tulpn 2>/dev/null | head -n 15", ports, sizeof(ports));
	}

	char eh[300], ecpu[512], emem[512], edisk[384], eports[512], out[2560];
	desktop_core_json_escape(health, eh, sizeof(eh));
	desktop_core_json_escape(top_cpu, ecpu, sizeof(ecpu));
	desktop_core_json_escape(top_mem, emem, sizeof(emem));
	desktop_core_json_escape(disk, edisk, sizeof(edisk));
	desktop_core_json_escape(ports, eports, sizeof(eports));
	snprintf(out, sizeof(out),
	         "{\"health\":\"%s\",\"top_cpu\":\"%s\",\"top_mem\":\"%s\",\"disk\":\"%s\",\"listening_ports\":\"%s\"}",
	         eh, ecpu, emem, edisk, eports);
	*out_result = strdup(out);
	return 0;
}

typedef struct {
	unsigned long pid;
	char          id[32];
	char          cls[128];
	char          title[256];
	int           wx, wy, ww, wh;
	int           sw, sh;
	bool          has_win;
	bool          has_scr;
	char          win_sel[160];
} desktop_win_state_t;

static void desktop_query_focused_window(desktop_win_state_t *out)
{
	if (!out) return;
	char saved_sel[160] = {0};
	snprintf(saved_sel, sizeof(saved_sel), "%s", out->win_sel);
	memset(out, 0, sizeof(*out));
	snprintf(out->win_sel, sizeof(out->win_sel), "%s", saved_sel);

	char *foc_json = NULL;
	if (ren_call_tool("x11_windows", "focused", &foc_json) != 0 || !foc_json) {
		free(foc_json);
		return;
	}
	out->pid = desktop_extract_json_ulong(foc_json, "pid");
	desktop_extract_json_str(foc_json, "id", out->id, sizeof(out->id));
	desktop_extract_json_str(foc_json, "class", out->cls, sizeof(out->cls));
	desktop_extract_json_str(foc_json, "title", out->title, sizeof(out->title));
	int gx = 0, gy = 0, gw = 0, gh = 0;
	if (desktop_extract_json_int(foc_json, "x", &gx) &&
	    desktop_extract_json_int(foc_json, "y", &gy) &&
	    desktop_extract_json_int(foc_json, "w", &gw) &&
	    desktop_extract_json_int(foc_json, "h", &gh) &&
	    gw > 0 && gh > 0) {
		out->wx = gx;
		out->wy = gy;
		out->ww = gw;
		out->wh = gh;
		out->has_win = true;
	}
	int gsw = 0, gsh = 0;
	if (desktop_extract_json_int(foc_json, "sw", &gsw) &&
	    desktop_extract_json_int(foc_json, "sh", &gsh) &&
	    gsw > 0 && gsh > 0) {
		out->sw = gsw;
		out->sh = gsh;
		out->has_scr = true;
	}
	free(foc_json);
}

static atspi_win_ctx_t desktop_win_to_atspi(const desktop_win_state_t *w)
{
	atspi_win_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	if (!w) return ctx;
	ctx.pid = w->pid;
	ctx.cls = w->cls;
	ctx.title = w->title;
	ctx.wx = w->wx;
	ctx.wy = w->wy;
	ctx.ww = w->ww;
	ctx.wh = w->wh;
	ctx.sw = w->sw;
	ctx.sh = w->sh;
	ctx.has_win = w->has_win;
	ctx.has_scr = w->has_scr;
	return ctx;
}

static const char *desktop_parse_win_target(const char *args, char *out_win, size_t win_sz)
{
	if (out_win && win_sz > 0) out_win[0] = '\0';
	if (!args) return "";
	while (*args && isspace((unsigned char)*args)) args++;
	if (*args != '@') return args;
	args++;
	if (!*args) return args;

	size_t w = 0;
	if (*args == '"' || *args == '\'') {
		char q = *args++;
		while (*args && *args != q && w + 1 < win_sz) {
			out_win[w++] = *args++;
		}
		if (*args == q) args++;
	} else {
		while (*args && !isspace((unsigned char)*args) && w + 1 < win_sz) {
			out_win[w++] = *args++;
		}
	}
	if (out_win && win_sz > 0) out_win[w] = '\0';
	while (*args && isspace((unsigned char)*args)) args++;
	return args;
}

static bool desktop_win_matches_sel(const desktop_win_state_t *w, const char *sel)
{
	if (!w || !sel || !*sel) return false;
	if (w->id[0] && strcasecmp(w->id, sel) == 0) return true;
	if (w->cls[0] && strcasestr(w->cls, sel) != NULL) return true;
	if (w->title[0] && strcasestr(w->title, sel) != NULL) return true;
	return false;
}

static bool desktop_focus_target_win(
	const char *win_sel, desktop_win_state_t *out_win, char *out_err, size_t err_sz)
{
	if (out_err && err_sz > 0) out_err[0] = '\0';
	memset(out_win, 0, sizeof(*out_win));
	if (win_sel && *win_sel) {
		snprintf(out_win->win_sel, sizeof(out_win->win_sel), "%s", win_sel);
	}
	desktop_query_focused_window(out_win);
	if (!win_sel || !*win_sel) return true;

	if (desktop_win_matches_sel(out_win, win_sel)) return true;

	char cmd[200];
	snprintf(cmd, sizeof(cmd), "focus %s", win_sel);
	char *fout = NULL;
	int rc = ren_call_tool("x11_windows", cmd, &fout);
	if (rc != 0 && fout && strstr(fout, "unknown tool")) {
		free(fout);
		fout = NULL;
		rc = ren_call_tool("window", cmd, &fout);
	}
	if (rc != 0) {
		if (out_err && err_sz > 0) {
			if (fout && *fout) snprintf(out_err, err_sz, "%s", fout);
			else snprintf(out_err, err_sz, "Error: window not found matching '@%.120s'", win_sel);
		}
		free(fout);
		return false;
	}
	free(fout);
	usleep(120000);
	desktop_query_focused_window(out_win);
	if (!out_win->cls[0] && !out_win->title[0]) {
		snprintf(out_win->title, sizeof(out_win->title), "%s", win_sel);
	}
	return true;
}

static bool desktop_atspi_focus_win(void *ud)
{
	desktop_win_state_t *w = (desktop_win_state_t *)ud;
	const char *sel = (w && w->win_sel[0]) ? w->win_sel : (w && w->id[0] ? w->id : NULL);
	if (!sel || !*sel) return false;
	char cmd[200], *fout = NULL;
	snprintf(cmd, sizeof(cmd), "focus %s", sel);
	int rc = ren_call_tool("x11_windows", cmd, &fout);
	if (rc != 0 && fout && strstr(fout, "unknown tool")) {
		free(fout);
		fout = NULL;
		rc = ren_call_tool("window", cmd, &fout);
	}
	free(fout);
	return rc == 0;
}

static bool desktop_atspi_is_win_focused(void *ud)
{
	desktop_win_state_t *w = (desktop_win_state_t *)ud;
	char *foc_json = NULL;
	int rc = ren_call_tool("x11_windows", "focused", &foc_json);
	if (rc != 0 || !foc_json) {
		free(foc_json);
		return true; /* No x11_windows tool loaded (e.g. Wayland) */
	}
	char cur_id[32] = {0};
	desktop_extract_json_str(foc_json, "id", cur_id, sizeof(cur_id));
	free(foc_json);
	if (w && w->id[0] && cur_id[0]) {
		return strcmp(cur_id, w->id) == 0;
	}
	return cur_id[0] != '\0';
}

static bool desktop_atspi_click_xy(void *ud, int x, int y, int btn)
{
	(void)ud;
	char arg[64], *out = NULL;
	snprintf(arg, sizeof(arg), "click %d %d %d", x, y, btn > 0 ? btn : 1);
	int rc = ren_call_tool("input", arg, &out);
	free(out);
	return rc == 0;
}

static bool desktop_atspi_send_key(void *ud, const char *combo)
{
	(void)ud;
	if (!combo || !*combo) return false;
	char arg[128], *out = NULL;
	snprintf(arg, sizeof(arg), "key %.110s", combo);
	int rc = ren_call_tool("input", arg, &out);
	free(out);
	return rc == 0;
}

static bool desktop_atspi_type_text(void *ud, const char *text, bool submit)
{
	(void)ud;
	if (!text) return false;
	size_t len = strlen(text);
	char *arg = (char *)malloc(len + 16);
	if (!arg) return false;
	snprintf(arg, len + 16, "%s %s", submit ? "submit" : "type", text);
	char *out = NULL;
	int rc = ren_call_tool("input", arg, &out);
	free(arg);
	free(out);
	return rc == 0;
}

static atspi_ops_t desktop_make_atspi_ops(desktop_win_state_t *w)
{
	atspi_ops_t ops;
	memset(&ops, 0, sizeof(ops));
	ops.focus_win = desktop_atspi_focus_win;
	ops.is_win_focused = desktop_atspi_is_win_focused;
	ops.click_xy = desktop_atspi_click_xy;
	ops.send_key = desktop_atspi_send_key;
	ops.type_text = desktop_atspi_type_text;
	ops.ud = w;
	return ops;
}

static int tool_terminal(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	int lines = (args && *args) ? atoi(args) : 40;
	if (lines <= 0) lines = 40;
	if (lines > 200) lines = 200;

	desktop_win_state_t win;
	memset(&win, 0, sizeof(win));
	desktop_query_focused_window(&win);

	char target[128] = {0};
	char *cap = (char *)malloc(8192);
	if (!cap) return -1;
	cap[0] = '\0';

	if (desktop_core_find_tmux_target(win.pid, target, sizeof(target)) &&
	    desktop_core_capture_tmux_target(target, lines, cap, 8192)) {
		size_t out_sz = strlen(cap) + 192;
		char *out = (char *)malloc(out_sz);
		if (!out) {
			free(cap);
			return -1;
		}
		snprintf(out, out_sz, "[tmux %s]\n%s", target, cap);
		free(cap);
		*out_result = out;
		return 0;
	}

	free(cap);
	*out_result = strdup("(no active user tmux pane output)");
	return 0;
}

static int tool_read_window(const char *args, char **out_result)
{
	char win_sel[160] = {0};
	const char *rest = desktop_parse_win_target(args, win_sel, sizeof(win_sel));
	bool inspect_mode = false;
	const char *filter = NULL;
	if (rest && *rest) {
		if (strcasecmp(rest, "inspect") == 0) {
			inspect_mode = true;
		} else if (strncasecmp(rest, "inspect ", 8) == 0) {
			inspect_mode = true;
			filter = rest + 8;
			while (*filter == ' ') filter++;
			if (!*filter) filter = NULL;
		} else if (strcasecmp(rest, "read") == 0) {
			inspect_mode = false;
		} else if (strncasecmp(rest, "read ", 5) == 0) {
			filter = rest + 5;
			while (*filter == ' ') filter++;
			if (!*filter) filter = NULL;
		} else {
			filter = rest;
		}
	}

	desktop_win_state_t win;
	char ferr[256] = {0};
	if (!desktop_focus_target_win(win_sel, &win, ferr, sizeof(ferr))) {
		*out_result = strdup(ferr[0] ? ferr : "Error: window not found");
		return -1;
	}

	char *buf = (char *)malloc(8192);
	if (!buf) return -1;
	buf[0] = '\0';

	if (desktop_core_is_terminal_class(win.cls)) {
		char target[128] = {0};
		if (desktop_core_find_tmux_target(win.pid, target, sizeof(target)) &&
		    desktop_core_capture_tmux_target(target, 60, buf, 8192)) {
			size_t out_sz = strlen(buf) + 384;
			char *out = (char *)malloc(out_sz);
			if (!out) {
				free(buf);
				return -1;
			}
			snprintf(out, out_sz, "[terminal tmux=%s class='%s' title='%.80s']\n%s",
			         target, win.cls, win.title, buf);
			free(buf);
			*out_result = out;
			return 0;
		}
	}

	atspi_win_ctx_t awin = desktop_win_to_atspi(&win);
	if (atspi_core_read_window(&awin, inspect_mode, filter, buf, 8192)) {
		size_t out_sz = strlen(buf) + 384;
		char *out = (char *)malloc(out_sz);
		if (!out) {
			free(buf);
			return -1;
		}
		snprintf(out, out_sz, "[window class='%s' title='%.80s' pid=%lu]\n%s",
		         win.cls[0] ? win.cls : "unknown", win.title, win.pid, buf);
		free(buf);
		*out_result = out;
		return 0;
	}

	free(buf);
	char msg[512];
	snprintf(msg, sizeof(msg),
	         "(no AT-SPI2 or tmux text tree available for window class='%s' title='%.120s' pid=%lu)",
	         win.cls[0] ? win.cls : "none", win.title, win.pid);
	*out_result = strdup(msg);
	return 0;
}

static bool desktop_split_pipe_arg(
	const char *raw, char *out_left, size_t left_sz, char *out_right, size_t right_sz)
{
	if (out_left && left_sz > 0) out_left[0] = '\0';
	if (out_right && right_sz > 0) out_right[0] = '\0';
	if (!raw) return false;
	while (*raw && isspace((unsigned char)*raw)) raw++;
	const char *pipe = strchr(raw, '|');
	if (!pipe) {
		snprintf(out_right, right_sz, "%s", raw);
		size_t rlen = strlen(out_right);
		while (rlen > 0 && isspace((unsigned char)out_right[rlen - 1])) out_right[--rlen] = '\0';
		return false;
	}
	size_t llen = (size_t)(pipe - raw);
	while (llen > 0 && isspace((unsigned char)raw[llen - 1])) llen--;
	if (llen >= left_sz) llen = left_sz - 1;
	memcpy(out_left, raw, llen);
	out_left[llen] = '\0';

	const char *r = pipe + 1;
	while (*r && isspace((unsigned char)*r)) r++;
	snprintf(out_right, right_sz, "%s", r);
	size_t rlen = strlen(out_right);
	while (rlen > 0 && isspace((unsigned char)out_right[rlen - 1])) out_right[--rlen] = '\0';
	return true;
}

static int tool_ui(const char *args, char **out_result)
{
	static const char *const k_usage =
		"Error: usage: ui [@win] <click <target> | rclick <target> | focus <target> | type [target |] <text> | submit [target |] <text> | set <target> | <val>>";

	char win_sel[160] = {0};
	const char *rest = desktop_parse_win_target(args, win_sel, sizeof(win_sel));
	if (!rest || !*rest) {
		*out_result = strdup(k_usage);
		return -1;
	}

	enum { UI_NONE, UI_CLICK, UI_RCLICK, UI_FOCUS, UI_TYPE, UI_SUBMIT, UI_SET } verb = UI_NONE;
	const char *arg = "";
	if (strncasecmp(rest, "click ", 6) == 0) { verb = UI_CLICK; arg = rest + 6; }
	else if (strncasecmp(rest, "rclick ", 7) == 0) { verb = UI_RCLICK; arg = rest + 7; }
	else if (strncasecmp(rest, "focus ", 6) == 0) { verb = UI_FOCUS; arg = rest + 6; }
	else if (strncasecmp(rest, "type ", 5) == 0) { verb = UI_TYPE; arg = rest + 5; }
	else if (strncasecmp(rest, "submit ", 7) == 0) { verb = UI_SUBMIT; arg = rest + 7; }
	else if (strncasecmp(rest, "set ", 4) == 0) { verb = UI_SET; arg = rest + 4; }
	else {
		*out_result = strdup(k_usage);
		return -1;
	}
	while (*arg && isspace((unsigned char)*arg)) arg++;

	char target[256] = {0}, text[1024] = {0};
	if (verb == UI_CLICK || verb == UI_RCLICK || verb == UI_FOCUS) {
		snprintf(target, sizeof(target), "%s", arg);
		size_t tlen = strlen(target);
		while (tlen > 0 && isspace((unsigned char)target[tlen - 1])) target[--tlen] = '\0';
		if (!target[0]) {
			*out_result = strdup(k_usage);
			return -1;
		}
	} else if (verb == UI_TYPE || verb == UI_SUBMIT) {
		bool has_pipe = desktop_split_pipe_arg(arg, target, sizeof(target), text, sizeof(text));
		if ((has_pipe && !target[0]) || !text[0]) {
			*out_result = strdup(k_usage);
			return -1;
		}
	} else if (verb == UI_SET) {
		bool has_pipe = desktop_split_pipe_arg(arg, target, sizeof(target), text, sizeof(text));
		if (!has_pipe || !target[0] || !text[0]) {
			*out_result = strdup("Error: usage: ui [@win] set <target> | <text_or_number>");
			return -1;
		}
	}

	desktop_win_state_t win;
	char msg[512] = {0};
	if (!desktop_focus_target_win(win_sel, &win, msg, sizeof(msg))) {
		*out_result = strdup(msg[0] ? msg : "Error: window not found");
		return -1;
	}

	atspi_win_ctx_t awin = desktop_win_to_atspi(&win);
	atspi_ops_t ops = desktop_make_atspi_ops(&win);

	if (verb == UI_CLICK || verb == UI_RCLICK) {
		bool ok = atspi_core_click(&awin, &ops, target, verb == UI_RCLICK ? 3 : 1, msg, sizeof(msg));
		*out_result = strdup(msg[0] ? msg : (ok ? "ok" : "Error: ui click failed"));
		return ok ? 0 : -1;
	}
	if (verb == UI_FOCUS) {
		bool ok = atspi_core_focus(&awin, &ops, target, msg, sizeof(msg));
		*out_result = strdup(msg[0] ? msg : (ok ? "ok" : "Error: ui focus failed"));
		return ok ? 0 : -1;
	}
	if (verb == UI_SET) {
		bool ok = atspi_core_set(&awin, &ops, target, text, msg, sizeof(msg));
		*out_result = strdup(msg[0] ? msg : (ok ? "ok" : "Error: ui set failed"));
		return ok ? 0 : -1;
	}

	/* UI_TYPE / UI_SUBMIT */
	char role[64] = {0}, label[128] = {0};
	bool is_term = desktop_core_is_terminal_class(win.cls);
	if (!atspi_core_prepare_input(&awin, &ops, target[0] ? target : NULL, is_term,
	                              role, sizeof(role), label, sizeof(label), msg, sizeof(msg))) {
		*out_result = strdup(msg[0] ? msg : "Error: no editable input focused");
		return -1;
	}
	if (!desktop_atspi_type_text(&win, text, verb == UI_SUBMIT)) {
		*out_result = strdup("Error: input tool unavailable to type text");
		return -1;
	}
	char out[512];
	snprintf(out, sizeof(out), "%s into [%s '%.64s']: %.160s",
	         verb == UI_SUBMIT ? "typed+enter" : "typed",
	         role[0] ? role : "input", label, text);
	*out_result = strdup(out);
	return 0;
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("open_app", "open_app <application_name>", tool_open_app);
	ren_register_tool("media", "media <play [query] | pause | next | prev | volume_up | volume_down | mute>", tool_media);
	ren_register_tool("system", "system [all | memory | cpu | disk | ports]", tool_system);
	ren_register_tool("terminal", "terminal [lines]", tool_terminal);
	ren_register_tool("read_window", "read_window [@win] [inspect [filter] | <filter>]", tool_read_window);
	ren_register_tool("ui", "ui [@win] <click <target> | rclick <target> | focus <target> | type [target |] <text> | submit [target |] <text> | set <target> | <val>>", tool_ui);
}
