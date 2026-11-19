/**
 * @file mod_sway.c
 * @brief Pure-C libxylem (XY) adapter plugin for Sway / i3 & libjarvis.
 */

#include "../../include/ttypt/jarvis-types.h"
#include <ttypt/xy.h>
#include <ttypt/xy-mod.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

XY_MODULE_API void xy_install(void)
{
}

static void spawn_detached_argv(char *const argv[])
{
	if (!argv || !argv[0]) return;
	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0) {
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
}

static void run_argv_sync(char *const argv[])
{
	if (!argv || !argv[0]) return;
	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	int st = 0;
	waitpid(pid, &st, 0);
}

static void run_cmd_capture(const char *cmd, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	size_t rd = fread(out, 1, out_sz - 1, fp);
	out[rd] = '\0';
	while (rd > 0 && (out[rd - 1] == '\n' || out[rd - 1] == '\r')) out[--rd] = '\0';
	pclose(fp);
}

static void url_encode(const char *src, char *dst, size_t dst_sz)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && w + 4 < dst_sz; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') dst[w++] = (char)*p;
		else if (*p == ' ') dst[w++] = '+';
		else {
			dst[w++] = '%';
			dst[w++] = hex[*p >> 4];
			dst[w++] = hex[*p & 0x0F];
		}
	}
	dst[w] = '\0';
}

XY_IMPL(int, jarvis_xy_observe, jarvis_world_t *, w)
{
	if (!w) return 0;
	snprintf(w->wm, sizeof(w->wm), "sway");
	char status[64] = {0};
	run_cmd_capture("playerctl status 2>/dev/null", status, sizeof(status));
	w->audio_playing = (strcasecmp(status, "Playing") == 0);
	return 0;
}

XY_IMPL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, res)
{
	if (!dsl || !res || res->handled) return 0;
	const char *op = dsl->op;

	if (strcmp(op, "focus_window") == 0) {
		res->handled = true;
		const char *id = dsl->target.identifier[0] ? dsl->target.identifier :
		                 (dsl->target.title_contains[0] ? dsl->target.title_contains : dsl->target.class_contains);
		if (!id[0]) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "focus_window requires target identifier");
			return 0;
		}
		char crit[320];
		if (isdigit((unsigned char)id[0])) {
			snprintf(crit, sizeof(crit), "[con_id=%s] focus", id);
		} else {
			snprintf(crit, sizeof(crit), "[app_id=\"(?i).*%s.*\"] focus", id);
		}
		char *const argv[] = { "swaymsg", crit, NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "focused %s", id);
		return 0;
	}

	if (strcmp(op, "focus_relative_window") == 0) {
		res->handled = true;
		const char *rel = dsl->target.relation[0] ? dsl->target.relation : "other";
		const char *dir = (strcmp(rel, "other") == 0 || strcmp(rel, "previous") == 0) ? "prev" : "next";
		char *const argv[] = { "swaymsg", "focus", (char *)dir, NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "focused %s window", rel);
		return 0;
	}

	if (strcmp(op, "open_terminal") == 0) {
		res->handled = true;
		const char *cmd = dsl->target.command[0] ? dsl->target.command : "foot";
		char *const argv[] = { "swaymsg", "exec", (char *)cmd, NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "opened terminal: %s", cmd);
		return 0;
	}

	if (strcmp(op, "open_application") == 0) {
		res->handled = true;
		const char *id = dsl->target.identifier[0] ? dsl->target.identifier : dsl->target.command;
		if (!id[0]) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "open_application requires target.identifier");
			return 0;
		}
		char *const argv[] = { "swaymsg", "exec", (char *)id, NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "launched %s", id);
		return 0;
	}

	if (strcmp(op, "open_url") == 0 || strcmp(op, "search_web") == 0) {
		res->handled = true;
		char url[768];
		if (strcmp(op, "search_web") == 0) {
			char enc[512];
			url_encode(dsl->target.query, enc, sizeof(enc));
			snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", enc);
		} else {
			if (strncmp(dsl->target.url, "http://", 7) == 0 || strncmp(dsl->target.url, "https://", 8) == 0)
				snprintf(url, sizeof(url), "%s", dsl->target.url);
			else
				snprintf(url, sizeof(url), "https://%s", dsl->target.url);
		}
		char *const argv[] = { "xdg-open", url, NULL };
		spawn_detached_argv(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "opened URL: %s", url);
		return 0;
	}

	if (strcmp(op, "spawn_shell_command") == 0) {
		res->handled = true;
		if (!dsl->target.command[0]) {
			res->success = false;
			return 0;
		}
		char *const argv[] = { "wtype", (char *)dsl->target.command, "-k", "Return", NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "ran command: %s", dsl->target.command);
		return 0;
	}

	if (strcmp(op, "type_text") == 0 || strcmp(op, "type_and_enter") == 0 || strcmp(op, "press_key") == 0) {
		res->handled = true;
		if (strcmp(op, "press_key") == 0) {
			const char *k = dsl->args.key_combo[0] ? dsl->args.key_combo : "Return";
			char *const argv[] = { "wtype", "-k", (char *)k, NULL };
			run_argv_sync(argv);
			snprintf(res->output, sizeof(res->output), "key: %s", k);
		} else if (strcmp(op, "type_and_enter") == 0) {
			char *const argv[] = { "wtype", (char *)dsl->args.text, "-k", "Return", NULL };
			run_argv_sync(argv);
			snprintf(res->output, sizeof(res->output), "typed+enter: %.40s", dsl->args.text);
		} else {
			char *const argv[] = { "wtype", (char *)dsl->args.text, NULL };
			run_argv_sync(argv);
			snprintf(res->output, sizeof(res->output), "typed: %.40s", dsl->args.text);
		}
		res->success = true;
		return 0;
	}

	if (strcmp(op, "workspace_switch") == 0) {
		res->handled = true;
		int ws = dsl->args.workspace > 0 ? dsl->args.workspace : 1;
		char ws_str[16];
		snprintf(ws_str, sizeof(ws_str), "%d", ws);
		char *const argv[] = { "swaymsg", "workspace", "number", ws_str, NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "switched to workspace %d", ws);
		return 0;
	}

	if (strcmp(op, "workspace_move_window") == 0) {
		res->handled = true;
		int ws = dsl->args.workspace > 0 ? dsl->args.workspace : 1;
		char ws_str[16];
		snprintf(ws_str, sizeof(ws_str), "%d", ws);
		char *const argv[] = { "swaymsg", "move", "container", "to", "workspace", "number", ws_str, NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "moved window to workspace %d", ws);
		return 0;
	}

	if (strcmp(op, "window_fullscreen") == 0 || strcmp(op, "window_float") == 0 || strcmp(op, "window_close") == 0) {
		res->handled = true;
		if (strcmp(op, "window_fullscreen") == 0) {
			char *const argv[] = { "swaymsg", "fullscreen", "toggle", NULL };
			run_argv_sync(argv);
			snprintf(res->output, sizeof(res->output), "toggled fullscreen");
		} else if (strcmp(op, "window_float") == 0) {
			char *const argv[] = { "swaymsg", "floating", "toggle", NULL };
			run_argv_sync(argv);
			snprintf(res->output, sizeof(res->output), "toggled floating");
		} else {
			char *const argv[] = { "swaymsg", "kill", NULL };
			run_argv_sync(argv);
			snprintf(res->output, sizeof(res->output), "closed window");
		}
		res->success = true;
		return 0;
	}

	if (strcmp(op, "media_play") == 0 || strcmp(op, "media_pause") == 0 ||
	    strcmp(op, "media_next") == 0 || strcmp(op, "media_prev") == 0) {
		res->handled = true;
		const char *act = "play";
		if (strcmp(op, "media_pause") == 0) act = "pause";
		else if (strcmp(op, "media_next") == 0) act = "next";
		else if (strcmp(op, "media_prev") == 0) act = "previous";
		char *const argv[] = { "playerctl", (char *)act, NULL };
		run_argv_sync(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "media: %s", act);
		return 0;
	}

	if (strcmp(op, "media_volume") == 0) {
		res->handled = true;
		const char *act = dsl->args.action[0] ? dsl->args.action : "volume_up";
		if (strcmp(act, "volume_up") == 0) {
			char *const argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%+", NULL };
			run_argv_sync(argv);
		} else if (strcmp(act, "volume_down") == 0) {
			char *const argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", NULL };
			run_argv_sync(argv);
		} else {
			char *const argv[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
			run_argv_sync(argv);
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "volume: %s", act);
		return 0;
	}

	return 0;
}
