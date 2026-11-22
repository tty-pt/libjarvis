#include "../include/ttypt/jarvis.h"
#include "../include/ttypt/jarvis-desktop.h"

#if !defined(__wasm__) && !defined(_WIN32)
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>

/**
 * @brief Built-in desktop observation listener (populates system health,
 *        active tmux panes, and discovered .desktop apps across Linux/POSIX).
 */
void jarvis_desktop_populate_defaults(jarvis_world_t *w)
{
	if (!w) return;
	if (!w->system_health[0]) {
		jarvis_desktop_populate_system_health(w->system_health, sizeof(w->system_health));
	}
	if (w->pane_count == 0) {
		jarvis_desktop_populate_tmux_panes(w, true, 20);
	}
	jarvis_desktop_populate_apps(w);
}

static void press_key_via_xy(const char *combo)
{
	jarvis_dsl_t d;
	jarvis_adapter_result_t r;
	memset(&d, 0, sizeof(d));
	memset(&r, 0, sizeof(r));
	snprintf(d.op, sizeof(d.op), "press_key");
	snprintf(d.args.key_combo, sizeof(d.args.key_combo), "%s", combo);
	jarvis_xy_execute(&d, &r);
}

static void focus_window_via_xy(const char *ident)
{
	jarvis_dsl_t d;
	jarvis_adapter_result_t r;
	memset(&d, 0, sizeof(d));
	memset(&r, 0, sizeof(r));
	snprintf(d.op, sizeof(d.op), "focus_window");
	snprintf(d.target.identifier, sizeof(d.target.identifier), "%s", ident);
	jarvis_xy_execute(&d, &r);
}

static bool is_terminal_cls(const char *cls)
{
	if (!cls || !*cls) return false;
	return strcasestr(cls, "st") != NULL || strcasestr(cls, "xterm") != NULL ||
	       strcasestr(cls, "alacritty") != NULL || strcasestr(cls, "kitty") != NULL ||
	       strcasestr(cls, "foot") != NULL || strcasestr(cls, "wezterm") != NULL ||
	       strcasestr(cls, "urxvt") != NULL || strcasestr(cls, "ghostty") != NULL;
}

int jarvis_desktop_execute_fallback(const jarvis_dsl_t *dsl, jarvis_adapter_result_t *res)
{
	if (!dsl || !res) return -1;
	const char *op = dsl->op;

	if (strcmp(op, "open_application") == 0) {
		const char *ident = dsl->target.identifier[0] ? dsl->target.identifier :
		                    (dsl->target.command[0] ? dsl->target.command : dsl->args.text);
		res->handled = true;
		if (!ident || !*ident) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "open_application requires target.identifier");
			return 0;
		}
		const char *resolved = jarvis_desktop_resolve_app_exec(ident);
		jarvis_desktop_spawn_detached_sh(resolved);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "launched %s", resolved);
		return 0;
	}

	if (strcmp(op, "open_url") == 0 || strcmp(op, "search_web") == 0) {
		res->handled = true;
		char url[768] = {0};
		if (strcmp(op, "search_web") == 0) {
			const char *q = dsl->target.query[0] ? dsl->target.query : dsl->args.text;
			if (!q || !*q) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "search_web requires target.query");
				return 0;
			}
			char enc[512];
			jarvis_desktop_url_encode(q, enc, sizeof(enc));
			snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", enc);
		} else {
			if (!dsl->target.url[0]) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "open_url requires target.url");
				return 0;
			}
			if (strncmp(dsl->target.url, "http://", 7) == 0 || strncmp(dsl->target.url, "https://", 8) == 0) {
				snprintf(url, sizeof(url), "%s", dsl->target.url);
			} else {
				snprintf(url, sizeof(url), "https://%s", dsl->target.url);
			}
		}
		char *const argv[] = { "firefox", "--new-tab", url, NULL };
		if (jarvis_desktop_spawn_detached_argv(argv) <= 0) {
			char *const xdg_argv[] = { "xdg-open", url, NULL };
			jarvis_desktop_spawn_detached_argv(xdg_argv);
		}
		focus_window_via_xy("firefox");
		res->success = true;
		snprintf(res->output, sizeof(res->output), "opened URL: %s", url);
		return 0;
	}

	if (strcmp(op, "media_play") == 0) {
		res->handled = true;
		const char *q = dsl->target.query;
		if (q[0]) {
			jarvis_world_t w;
			memset(&w, 0, sizeof(w));
			jarvis_xy_observe(&w);
			bool preserve_foc = dsl->args.background || is_terminal_cls(w.focused.wm_class);
			jarvis_desktop_mpris_control("Pause");

			char enc[512], yt_url[768], resolve_cmd[1024], vid_id[64] = {0};
			jarvis_desktop_url_encode(q, enc, sizeof(enc));
			snprintf(
				resolve_cmd, sizeof(resolve_cmd),
				"curl -s -L --max-time 3.5 -A 'Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0' "
				"'https://www.youtube.com/results?search_query=%s' 2>/dev/null | "
				"grep -o '\"videoId\":\"[a-zA-Z0-9_-]\\{11\\}\"' | head -n 1 | cut -d'\"' -f4",
				enc
			);
			jarvis_desktop_run_cmd_capture(resolve_cmd, vid_id, sizeof(vid_id));
			if (strlen(vid_id) == 11) {
				snprintf(yt_url, sizeof(yt_url), "https://www.youtube.com/watch?v=%s&autoplay=1", vid_id);
			} else {
				snprintf(yt_url, sizeof(yt_url), "https://www.youtube.com/results?search_query=%s", enc);
			}
			char *const argv[] = { "firefox", "--new-tab", yt_url, NULL };
			jarvis_desktop_spawn_detached_argv(argv);
			if (!preserve_foc) {
				focus_window_via_xy("firefox");
			} else if (w.focused.id[0]) {
				focus_window_via_xy(w.focused.id);
			}
			res->success = true;
			snprintf(res->output, sizeof(res->output), "playing: %s", q);
			return 0;
		}
		if (!jarvis_desktop_mpris_control("Play")) {
			press_key_via_xy("XF86AudioPlay");
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "resumed media");
		return 0;
	}

	if (strcmp(op, "media_pause") == 0) {
		res->handled = true;
		if (!jarvis_desktop_mpris_control("Pause")) {
			press_key_via_xy("XF86AudioPlay");
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "paused media");
		return 0;
	}

	if (strcmp(op, "media_next") == 0 || strcmp(op, "media_prev") == 0) {
		res->handled = true;
		bool is_next = (strcmp(op, "media_next") == 0);
		jarvis_desktop_mpris_control(is_next ? "Next" : "Previous");
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s track", is_next ? "next" : "previous");
		return 0;
	}

	if (strcmp(op, "media_volume") == 0) {
		res->handled = true;
		const char *act = dsl->args.action[0] ? dsl->args.action : "volume_up";
		const char *cmd = "wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+ 2>/dev/null || pactl set-sink-volume @DEFAULT_SINK@ +5% 2>/dev/null";
		if (strcmp(act, "volume_down") == 0) {
			cmd = "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%- 2>/dev/null || pactl set-sink-volume @DEFAULT_SINK@ -5% 2>/dev/null";
		} else if (strcmp(act, "volume_mute") == 0 || strcmp(act, "mute") == 0) {
			cmd = "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle 2>/dev/null || pactl set-sink-mute @DEFAULT_SINK@ toggle 2>/dev/null";
		}
		jarvis_desktop_spawn_detached_sh(cmd);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "volume: %s", act);
		return 0;
	}

	if (strcmp(op, "wait") == 0) {
		res->handled = true;
		double sec = dsl->args.seconds > 0.0 ? dsl->args.seconds : 1.0;
		if (sec > 10.0) sec = 10.0;
		usleep((useconds_t)(sec * 1e6));
		res->success = true;
		snprintf(res->output, sizeof(res->output), "waited %.1fs", sec);
		return 0;
	}

	if (strcmp(op, "tts_control") == 0) {
		res->handled = true;
		const char *act = dsl->args.action[0] ? dsl->args.action : "on";
		bool turn_on = (strcasecmp(act, "off") != 0 && strcasecmp(act, "0") != 0 && strcasecmp(act, "disable") != 0);
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "jarvis config tts_enabled %s >/dev/null 2>&1", turn_on ? "1" : "0");
		jarvis_desktop_spawn_detached_sh(cmd);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s", turn_on ? "1" : "0");
		return 0;
	}

	if (strcmp(op, "answer_question") == 0) {
		res->handled = true;
		const char *ans = dsl->args.answer[0] ? dsl->args.answer :
		                  (dsl->args.text[0] ? dsl->args.text : dsl->target.query);
		if (!ans[0]) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "answer_question requires args.answer");
			return 0;
		}
		char *const argv[] = {
			"notify-send", "-t", "8000", "-h", "string:x-dunst-stack-tag:jarvis-answer",
			"  Jarvis", (char *)ans, NULL
		};
		jarvis_desktop_spawn_detached_argv(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s", ans);
		return 0;
	}

	if (strcmp(op, "inspect_system") == 0) {
		res->handled = true;
		const char *aspect = dsl->args.aspect[0] ? dsl->args.aspect :
		                     (dsl->target.query[0] ? dsl->target.query : "all");
		char health[256] = {0}, top_cpu[768] = {0}, top_mem[768] = {0}, disk[512] = {0}, ports[768] = {0};
		jarvis_desktop_populate_system_health(health, sizeof(health));
		if (strcasecmp(aspect, "all") == 0 || strcasecmp(aspect, "memory") == 0 ||
		    strcasecmp(aspect, "ram") == 0 || strcasecmp(aspect, "processes") == 0 ||
		    strcasecmp(aspect, "cpu") == 0) {
			jarvis_desktop_run_cmd_capture("ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head -n 7", top_cpu, sizeof(top_cpu));
			jarvis_desktop_run_cmd_capture("ps -eo pid,comm,%cpu,%mem --sort=-%mem | head -n 7", top_mem, sizeof(top_mem));
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
			jarvis_desktop_run_cmd_capture("ss -tulpn 2>/dev/null | head -n 15", ports, sizeof(ports));
		}
		snprintf(res->output, sizeof(res->output),
		         "{\"health\":\"%.200s\",\"top_cpu\":\"%.400s\",\"top_mem\":\"%.400s\",\"disk\":\"%.320s\",\"listening_ports\":\"%.450s\"}",
		         health, top_cpu, top_mem, disk, ports);
		res->success = true;
		return 0;
	}

	if (strcmp(op, "inspect_clipboard") == 0) {
		res->handled = true;
#ifdef __APPLE__
		jarvis_desktop_run_cmd_capture("pbpaste 2>/dev/null", res->output, sizeof(res->output));
#else
		char prim[768] = {0}, clip[768] = {0};
		jarvis_desktop_run_cmd_capture("xclip -o -selection primary 2>/dev/null || xsel -op 2>/dev/null || wl-paste -p 2>/dev/null", prim, sizeof(prim));
		jarvis_desktop_run_cmd_capture("xclip -o -selection clipboard 2>/dev/null || xsel -ob 2>/dev/null || wl-paste 2>/dev/null", clip, sizeof(clip));
		snprintf(res->output, sizeof(res->output),
		         "{\"primary_selection\":\"%.700s\",\"clipboard\":\"%.700s\"}", prim, clip);
#endif
		res->success = true;
		return 0;
	}

	if (strcmp(op, "inspect_terminal") == 0) {
		res->handled = true;
		jarvis_desktop_run_cmd_capture("tmux capture-pane -p -S -25 2>/dev/null | tail -n 20",
		                               res->output, sizeof(res->output));
		res->success = true;
		return 0;
	}

	if (strcmp(op, "run_background_command") == 0 && dsl->target.command[0]) {
		if (jarvis_is_destructive_command(dsl->target.command) &&
		    jarvis_xy_confirm(dsl->target.command) != 1) {
			res->handled = true;
			res->success = false;
			snprintf(res->output, sizeof(res->output),
			         "Blocked destructive command: %.200s", dsl->target.command);
			return 0;
		}
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		jarvis_xy_observe(&w);
		char full_cmd[1024];
		if (w.focused_cwd[0]) {
			snprintf(full_cmd, sizeof(full_cmd), "cd \"%s\" 2>/dev/null && (%s) 2>&1",
			         w.focused_cwd, dsl->target.command);
		} else {
			snprintf(full_cmd, sizeof(full_cmd), "(%s) 2>&1", dsl->target.command);
		}
		jarvis_desktop_run_cmd_capture(full_cmd, res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	return -1;
}
#elif defined(_WIN32)
void jarvis_desktop_populate_defaults(jarvis_world_t *w)
{
	(void)w;
}

int jarvis_desktop_execute_fallback(const jarvis_dsl_t *dsl, jarvis_adapter_result_t *res)
{
	(void)dsl;
	(void)res;
	return -1;
}
#endif
