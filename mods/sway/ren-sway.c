/**
 * @file ren-sway.c
 * @brief Standalone ren extension for Sway / i3 window and workspace control.
 *
 * ZERO libjarvis dependency — works standalone via `ren -m ren-sway:ren-reflex`.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ttypt/libren.h"
#include <ttypt/xy.h>

static int run_argv_sync(char *const argv[])
{
	if (!argv || !argv[0]) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
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
	if (waitpid(pid, &st, 0) < 0) return -1;
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static int tool_window(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: window <close | fullscreen | float | focus <class_or_title|other|next|previous>>");
		return -1;
	}

	if (strcasecmp(args, "close") == 0) {
		char *const argv[] = { "swaymsg", "kill", NULL };
		int rc = run_argv_sync(argv);
		*out_result = strdup(rc == 0 ? "closed window" : "Error: swaymsg kill failed");
		return rc;
	}
	if (strcasecmp(args, "fullscreen") == 0) {
		char *const argv[] = { "swaymsg", "fullscreen", "toggle", NULL };
		int rc = run_argv_sync(argv);
		*out_result = strdup(rc == 0 ? "toggled fullscreen" : "Error: swaymsg fullscreen failed");
		return rc;
	}
	if (strcasecmp(args, "float") == 0) {
		char *const argv[] = { "swaymsg", "floating", "toggle", NULL };
		int rc = run_argv_sync(argv);
		*out_result = strdup(rc == 0 ? "toggled floating" : "Error: swaymsg floating failed");
		return rc;
	}
	if (strncasecmp(args, "focus ", 6) == 0) {
		const char *ident = args + 6;
		while (*ident == ' ') ident++;
		if (!*ident) {
			*out_result = strdup("Error: window focus requires a target");
			return -1;
		}
		if (strcasecmp(ident, "other") == 0 || strcasecmp(ident, "previous") == 0) {
			char *const argv[] = { "swaymsg", "focus", "prev", NULL };
			int rc = run_argv_sync(argv);
			*out_result = strdup(rc == 0 ? "focused previous window" : "Error: swaymsg focus failed");
			return rc;
		}
		if (strcasecmp(ident, "next") == 0 || strcasecmp(ident, "master") == 0) {
			char *const argv[] = { "swaymsg", "focus", "next", NULL };
			int rc = run_argv_sync(argv);
			*out_result = strdup(rc == 0 ? "focused next window" : "Error: swaymsg focus failed");
			return rc;
		}
		char crit[320];
		if (isdigit((unsigned char)ident[0])) {
			snprintf(crit, sizeof(crit), "[con_id=%s] focus", ident);
		} else {
			snprintf(crit, sizeof(crit), "[app_id=\"(?i).*%s.*\"] focus", ident);
		}
		char *const argv[] = { "swaymsg", crit, NULL };
		int rc = run_argv_sync(argv);
		char buf[384];
		snprintf(buf, sizeof(buf), rc == 0 ? "focused %.300s" : "Error: failed to focus %.300s", ident);
		*out_result = strdup(buf);
		return rc;
	}

	*out_result = strdup("Error: usage: window <close | fullscreen | float | focus <class_or_title|other|next|previous>>");
	return -1;
}

static int tool_workspace(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: workspace <1..9 | move 1..9>");
		return -1;
	}

	bool move = false;
	const char *num_s = args;
	if (strncasecmp(args, "move ", 5) == 0) {
		move = true;
		num_s = args + 5;
		while (*num_s == ' ') num_s++;
	}

	int ws = atoi(num_s);
	if (ws < 1 || ws > 20) {
		*out_result = strdup("Error: workspace number must be between 1 and 20");
		return -1;
	}

	char ws_str[16];
	snprintf(ws_str, sizeof(ws_str), "%d", ws);
	int rc;
	if (move) {
		char *const argv[] = { "swaymsg", "move", "container", "to", "workspace", "number", ws_str, NULL };
		rc = run_argv_sync(argv);
	} else {
		char *const argv[] = { "swaymsg", "workspace", "number", ws_str, NULL };
		rc = run_argv_sync(argv);
	}

	char buf[96];
	snprintf(buf, sizeof(buf),
	         rc == 0 ? (move ? "moved window to workspace %d" : "switched to workspace %d")
	                 : "Error: swaymsg workspace failed",
	         ws);
	*out_result = strdup(buf);
	return rc;
}

static int tool_open_terminal(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	const char *cmd = (args && *args) ? args : "foot";
	char *const argv[] = { "swaymsg", "exec", (char *)cmd, NULL };
	int rc = run_argv_sync(argv);
	char buf[320];
	snprintf(buf, sizeof(buf), rc == 0 ? "opened terminal: %.256s" : "Error: swaymsg exec failed", cmd);
	*out_result = strdup(buf);
	return rc;
}

static char *sway_context_provider(void)
{
	const char *sock = getenv("SWAYSOCK");
	if (!sock || !*sock) return NULL;
	return strdup("wm=sway");
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("window", "window <close | fullscreen | float | focus <class_or_title|other|next|previous>>", tool_window);
	ren_register_tool("workspace", "workspace <1..9 | move 1..9>", tool_workspace);
	ren_register_tool("open_terminal", "open_terminal [command]", tool_open_terminal);
	ren_register_context_provider(sway_context_provider);
}
