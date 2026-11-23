/**
 * @file ren-jarvis.c
 * @brief Lightweight voice-assistant persona, model, and prompt-hint module for ren.
 *
 * ZERO libjarvis dependency — reads ~/.jarvisrc directly and configures ren's
 * system prompt, model, and desktop/browser guidance.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ttypt/libren.h"
#include <ttypt/xy.h>

static bool rc_lookup(const char *key, char *out, size_t out_sz)
{
	if (!key || !out || out_sz == 0) return false;
	out[0] = '\0';

	const char *env_rc = getenv("JARVISRC");
	const char *home = getenv("HOME");
	char rc_path[512];
	if (env_rc && *env_rc) {
		snprintf(rc_path, sizeof(rc_path), "%s", env_rc);
	} else if (home && *home) {
		snprintf(rc_path, sizeof(rc_path), "%s/.jarvisrc", home);
		if (access(rc_path, R_OK) != 0) {
			snprintf(rc_path, sizeof(rc_path), "%s/.config/jarvis/jarvisrc", home);
		}
	} else {
		return false;
	}

	FILE *fp = fopen(rc_path, "r");
	if (!fp) return false;

	bool found = false;
	char line[4096];
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p || *p == '#' || *p == ';') continue;
		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = '\0';
		size_t kl = strlen(p);
		while (kl > 0 && isspace((unsigned char)p[kl - 1])) p[--kl] = '\0';
		if (strcasecmp(p, key) != 0) continue;

		char *v = eq + 1;
		while (*v && isspace((unsigned char)*v)) v++;
		size_t vl = strlen(v);
		while (vl > 0 && isspace((unsigned char)v[vl - 1])) v[--vl] = '\0';
		if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') ||
		                (v[0] == '\'' && v[vl - 1] == '\''))) {
			v[vl - 1] = '\0';
			v++;
		}
		if (v[0]) {
			snprintf(out, out_sz, "%s", v);
			found = true;
		}
	}
	fclose(fp);
	return found;
}

static void set_jarvis_identity_prompt(void)
{
	char custom_prompt[4096] = {0};
	if (rc_lookup("initial_prompt", custom_prompt, sizeof(custom_prompt)) && custom_prompt[0]) {
		ren_set_system_prompt(custom_prompt);
		return;
	}

	char wake_buf[64] = {0};
	if (!rc_lookup("wake_word", wake_buf, sizeof(wake_buf)) || !wake_buf[0]) {
		snprintf(wake_buf, sizeof(wake_buf), "Assistant");
	}
	wake_buf[0] = (char)toupper((unsigned char)wake_buf[0]);

	char identity[1024];
	snprintf(identity, sizeof(identity),
	         "You are %s, the user's personal voice and desktop AI assistant. "
	         "Never identify yourself as ren. Help the user with desktop automation, "
	         "browser control, file operations, and memory using the available tools.",
	         wake_buf);
	ren_set_system_prompt(identity);
}

static void set_jarvis_model(void)
{
	char model_buf[128] = {0};
	if (rc_lookup("model", model_buf, sizeof(model_buf)) && model_buf[0]) {
		ren_set_model(model_buf);
	} else {
		ren_set_model("fast");
	}
}

XY_MODULE_API void xy_install(void)
{
	set_jarvis_identity_prompt();
	set_jarvis_model();

	ren_register_prompt_hint(
		"You have live GUI, desktop, and browser control via 'browser', 'window', 'workspace', 'x11_windows', 'clipboard', 'input', 'open_app', 'open_terminal', 'media', 'system', 'terminal', 'recall', and 'record'. Never claim you cannot access or interact with browser tabs, WhatsApp Web, or desktop apps.\n"
		"To browse, read pages, or interact with an open browser tab:\n"
		"1. 'browser url <https://...>', 'browser search <query>', 'browser tab <title>', and 'browser click <label>' automatically wait for page load and return a readable page snapshot. Use 'browser read [offset|filter]' to read more page text or paginate long articles, 'browser wait [ms|text]' if a dynamic page is still loading, and 'browser inspect [filter]' to list clickable UI controls with coordinates.\n"
		"2. After navigating or clicking a link/button, always inspect the returned snapshot (or call 'browser read' / 'browser inspect') before deciding your next step or summarizing content.\n"
		"3. For chat replies (e.g. WhatsApp Web), prefer a single 'browser' call with 'send <contact_or_entry> | <message>' (or 'browser click <contact>' then 'browser send <message>') without extra sleeps.\n"
		"After receiving tool results that complete the user's request, always output a brief 1-sentence confirmation to the user (never an empty reply).\n"
		"When answering the user directly, keep spoken replies concise, natural, and direct.");
}
