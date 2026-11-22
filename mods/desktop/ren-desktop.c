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
#include <ttypt/xy.h>

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
	if (desktop_core_spawn_detached_sh(resolved) < 0) {
		*out_result = strdup("Error: failed to launch application");
		return -1;
	}
	char buf[384];
	snprintf(buf, sizeof(buf), "launched %.320s", resolved);
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

static int tool_terminal(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	int lines = (args && *args) ? atoi(args) : 25;
	if (lines <= 0) lines = 25;
	if (lines > 200) lines = 200;
	char cmd[128], out[2048] = {0};
	snprintf(cmd, sizeof(cmd), "tmux capture-pane -p -S -%d 2>/dev/null | tail -n %d", lines, lines);
	desktop_core_run_cmd_capture(cmd, out, sizeof(out));
	*out_result = strdup(out[0] ? out : "(no active tmux pane output)");
	return 0;
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("open_app", "open_app <application_name>", tool_open_app);
	ren_register_tool("media", "media <play [query] | pause | next | prev | volume_up | volume_down | mute>", tool_media);
	ren_register_tool("system", "system [all | memory | cpu | disk | ports]", tool_system);
	ren_register_tool("terminal", "terminal [lines]", tool_terminal);
}
