/**
 * @file ren-desktop.c
 * @brief Standalone ren extension for desktop app launch, media/volume, system health, and tmux capture.
 *
 * ZERO libjarvis or libX11 dependency — works standalone via `ren -m ren-desktop:ren-mm`.
 */

#define _GNU_SOURCE
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

static void desktop_query_focused_window(
	unsigned long *out_pid, char *out_cls, size_t cls_sz,
	char *out_title, size_t title_sz,
	int *out_x, int *out_y, int *out_w, int *out_h, bool *out_has_win)
{
	if (out_pid) *out_pid = 0;
	if (out_cls && cls_sz > 0) out_cls[0] = '\0';
	if (out_title && title_sz > 0) out_title[0] = '\0';
	if (out_has_win) *out_has_win = false;

	char *foc_json = NULL;
	if (ren_call_tool("x11_windows", "focused", &foc_json) != 0 || !foc_json) {
		free(foc_json);
		return;
	}
	if (out_pid) *out_pid = desktop_extract_json_ulong(foc_json, "pid");
	if (out_cls && cls_sz > 0) desktop_extract_json_str(foc_json, "class", out_cls, cls_sz);
	if (out_title && title_sz > 0) desktop_extract_json_str(foc_json, "title", out_title, title_sz);
	int gx = 0, gy = 0, gw = 0, gh = 0;
	if (desktop_extract_json_int(foc_json, "x", &gx) &&
	    desktop_extract_json_int(foc_json, "y", &gy) &&
	    desktop_extract_json_int(foc_json, "w", &gw) &&
	    desktop_extract_json_int(foc_json, "h", &gh) &&
	    gw > 0 && gh > 0) {
		if (out_x) *out_x = gx;
		if (out_y) *out_y = gy;
		if (out_w) *out_w = gw;
		if (out_h) *out_h = gh;
		if (out_has_win) *out_has_win = true;
	}
	free(foc_json);
}

static int tool_terminal(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	int lines = (args && *args) ? atoi(args) : 40;
	if (lines <= 0) lines = 40;
	if (lines > 200) lines = 200;

	unsigned long foc_pid = 0;
	desktop_query_focused_window(&foc_pid, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, NULL);

	char target[128] = {0};
	char *cap = (char *)malloc(8192);
	if (!cap) return -1;
	cap[0] = '\0';

	if (desktop_core_find_tmux_target(foc_pid, target, sizeof(target)) &&
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
	while (args && *args == ' ') args++;
	bool inspect_mode = false;
	const char *filter = NULL;
	if (args && *args) {
		if (strcasecmp(args, "inspect") == 0) {
			inspect_mode = true;
		} else if (strncasecmp(args, "inspect ", 8) == 0) {
			inspect_mode = true;
			filter = args + 8;
			while (*filter == ' ') filter++;
			if (!*filter) filter = NULL;
		} else if (strcasecmp(args, "read") == 0) {
			inspect_mode = false;
		} else if (strncasecmp(args, "read ", 5) == 0) {
			filter = args + 5;
			while (*filter == ' ') filter++;
			if (!*filter) filter = NULL;
		} else {
			filter = args;
		}
	}

	unsigned long foc_pid = 0;
	char cls[128] = {0}, title[256] = {0};
	int wx = 0, wy = 0, ww = 0, wh = 0;
	bool has_win = false;
	desktop_query_focused_window(&foc_pid, cls, sizeof(cls), title, sizeof(title),
	                             &wx, &wy, &ww, &wh, &has_win);

	char *buf = (char *)malloc(8192);
	if (!buf) return -1;
	buf[0] = '\0';

	if (desktop_core_is_terminal_class(cls)) {
		char target[128] = {0};
		if (desktop_core_find_tmux_target(foc_pid, target, sizeof(target)) &&
		    desktop_core_capture_tmux_target(target, 60, buf, 8192)) {
			size_t out_sz = strlen(buf) + 384;
			char *out = (char *)malloc(out_sz);
			if (!out) {
				free(buf);
				return -1;
			}
			snprintf(out, out_sz, "[terminal tmux=%s class='%s' title='%.80s']\n%s",
			         target, cls, title, buf);
			free(buf);
			*out_result = out;
			return 0;
		}
	}

	if (atspi_core_read_window(foc_pid, cls, title, wx, wy, ww, wh, has_win,
	                           inspect_mode, filter, buf, 8192)) {
		size_t out_sz = strlen(buf) + 384;
		char *out = (char *)malloc(out_sz);
		if (!out) {
			free(buf);
			return -1;
		}
		snprintf(out, out_sz, "[window class='%s' title='%.80s' pid=%lu]\n%s",
		         cls[0] ? cls : "unknown", title, foc_pid, buf);
		free(buf);
		*out_result = out;
		return 0;
	}

	free(buf);
	char msg[512];
	snprintf(msg, sizeof(msg),
	         "(no AT-SPI2 or tmux text tree available for focused window class='%s' title='%.120s' pid=%lu)",
	         cls[0] ? cls : "none", title, foc_pid);
	*out_result = strdup(msg);
	return 0;
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("open_app", "open_app <application_name>", tool_open_app);
	ren_register_tool("media", "media <play [query] | pause | next | prev | volume_up | volume_down | mute>", tool_media);
	ren_register_tool("system", "system [all | memory | cpu | disk | ports]", tool_system);
	ren_register_tool("terminal", "terminal [lines]", tool_terminal);
	ren_register_tool("read_window", "read_window [inspect [filter] | <filter>]", tool_read_window);
}
