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

#ifdef _WIN32
#include <io.h>
#ifndef R_OK
#define R_OK 4
#endif
#define access _access
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#include <unistd.h>
#endif

#include <ttypt/stele.h>
#include "ttypt/libren.h"
#include <ttypt/xy.h>

static bool rc_lookup(const char *key, char *out, size_t out_sz)
{
	if (!key || !out || out_sz == 0) return false;
	out[0] = '\0';

	const char *env_rc = getenv("JARVISRC");
	char hbuf[512] = {0};
	const char *home = getenv("HOME");
	if (!home || !*home) {
		stl_home_dir(hbuf, sizeof(hbuf));
		home = hbuf[0] ? hbuf : NULL;
	}
	char rc_path[512] = {0};
	if (env_rc && *env_rc) {
		snprintf(rc_path, sizeof(rc_path), "%s", env_rc);
	} else {
		if (home && *home) {
			snprintf(rc_path, sizeof(rc_path), "%s/.jarvisrc", home);
		}
		if (!rc_path[0] || access(rc_path, R_OK) != 0) {
			char cdir[512] = {0};
			if (stl_config_dir("jarvis", cdir, sizeof(cdir)) && cdir[0]) {
				snprintf(rc_path, sizeof(rc_path), "%s/jarvisrc", cdir);
			}
		}
		if (!rc_path[0])
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

static bool normalize_lang_local(const char *raw, char *out, size_t out_sz)
{
	if (!raw || !*raw || !out || out_sz == 0) return false;
	char buf[32];
	size_t w = 0;
	while (*raw && isspace((unsigned char)*raw)) raw++;
	for (size_t i = 0; raw[i] && w + 1 < sizeof(buf); i++) {
		char c = raw[i];
		if (c == '.' || c == '@') break;
		if (c == '_') c = '-';
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		buf[w++] = c;
	}
	while (w > 0 && isspace((unsigned char)buf[w - 1])) w--;
	buf[w] = '\0';
	if (!buf[0] || strcmp(buf, "c") == 0 || strcmp(buf, "posix") == 0)
		return false;
	snprintf(out, out_sz, "%s", buf);
	return true;
}

static bool resolve_kbd_local(char *out, size_t out_sz)
{
	char ubuf[64] = {0};
	stl_username(ubuf, sizeof(ubuf));
	const char *u = ubuf[0] ? ubuf : "user";
	char tdir[256] = {0};
	stl_temp_dir(tdir, sizeof(tdir));
	char path[384], buf[32] = {0};
	snprintf(path, sizeof(path), "%s/dwm-kbd-%s.state", tdir[0] ? tdir : "/tmp", u);
	FILE *fp = fopen(path, "r");
	if (!fp) return false;
	bool ok = false;
	if (fgets(buf, sizeof(buf), fp)) {
		char code[8] = {0};
		size_t ci = 0;
		for (size_t i = 0; buf[i] && ci < 2; i++) {
			if (isalpha((unsigned char)buf[i]))
				code[ci++] = (char)tolower((unsigned char)buf[i]);
		}
		code[ci] = '\0';
		if (strcmp(code, "us") == 0 || strcmp(code, "gb") == 0 || strcmp(code, "uk") == 0) {
			snprintf(out, out_sz, "en");
			ok = true;
		} else if (strcmp(code, "br") == 0) {
			snprintf(out, out_sz, "pt-br");
			ok = true;
		} else if (ci == 2) {
			snprintf(out, out_sz, "%s", code);
			ok = true;
		}
	}
	fclose(fp);
	return ok;
}

static void resolve_lang_local(char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	char tag[32] = {0};

	const char *env_l = getenv("JARVIS_LANG");
	if (env_l && normalize_lang_local(env_l, tag, sizeof(tag))) {
		if (strcmp(tag, "kbd") == 0) {
			if (resolve_kbd_local(out, out_sz)) return;
		} else if (strcmp(tag, "auto") != 0) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}

	char rc_lang[32] = {0};
	if (rc_lookup("lang", rc_lang, sizeof(rc_lang)) &&
	    normalize_lang_local(rc_lang, tag, sizeof(tag))) {
		if (strcmp(tag, "kbd") == 0) {
			if (resolve_kbd_local(out, out_sz)) return;
		} else if (strcmp(tag, "auto") != 0) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}

	static const char *const lc_vars[] = { "LC_ALL", "LC_MESSAGES", "LANG", NULL };
	for (size_t i = 0; lc_vars[i]; i++) {
		const char *lv = getenv(lc_vars[i]);
		if (lv && normalize_lang_local(lv, tag, sizeof(tag))) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}

	snprintf(out, out_sz, "en");
}

static void set_jarvis_identity_prompt(void)
{
	char custom_prompt[4096] = {0};
	if (rc_lookup("initial_prompt", custom_prompt, sizeof(custom_prompt)) && custom_prompt[0]) {
		ren_set_system_prompt(custom_prompt);
		return;
	}

	char lang[32] = {0};
	resolve_lang_local(lang, sizeof(lang));

	char wake_buf[64] = {0};
	const char *env_w = getenv("JARVIS_WAKE_WORD");
	if (env_w && *env_w) {
		snprintf(wake_buf, sizeof(wake_buf), "%s", env_w);
	} else if (!rc_lookup("wake_word", wake_buf, sizeof(wake_buf)) ||
	           !wake_buf[0] || strcasecmp(wake_buf, "auto") == 0) {
		snprintf(wake_buf, sizeof(wake_buf), "%s",
		         (strncmp(lang, "pt", 2) == 0) ? "Jarbas" : "Jarvis");
	}
	wake_buf[0] = (char)toupper((unsigned char)wake_buf[0]);

	char lang_rule[128];
	if (strcmp(lang, "pt-br") == 0) {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in Brazilian Portuguese (pt-BR).");
	} else if (strncmp(lang, "pt", 2) == 0) {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in European Portuguese (pt-PT).");
	} else if (strncmp(lang, "en", 2) == 0) {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in English.");
	} else if (strncmp(lang, "es", 2) == 0) {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in Spanish.");
	} else if (strncmp(lang, "fr", 2) == 0) {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in French.");
	} else if (strncmp(lang, "de", 2) == 0) {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in German.");
	} else if (strncmp(lang, "it", 2) == 0) {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in Italian.");
	} else {
		snprintf(lang_rule, sizeof(lang_rule), "Always reply to the user in language '%s'.", lang);
	}

	char identity[1280];
	snprintf(identity, sizeof(identity),
	         "You are %s, the user's personal voice and desktop AI assistant. "
	         "Never identify yourself as ren. Help the user with desktop automation, "
	         "browser control, file operations, and memory using the available tools. %s",
	         wake_buf, lang_rule);
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
		"You have live GUI, desktop, and browser control via 'browser', 'ui', 'read_window', 'window', 'workspace', 'x11_windows', 'clipboard', 'input', 'open_app', 'open_terminal', 'media', 'system', 'terminal', 'recall', and 'record'. Never claim you cannot access or interact with browser tabs, WhatsApp Web, or desktop apps.\n"
		"To inspect or interact with any non-browser desktop app (GTK, Qt, Electron, LibreOffice, etc.) without screenshots:\n"
		"- Call 'read_window [@win]' (or 'read_window [@win] inspect [filter]' for interactive controls) to read text or UI elements via AT-SPI2 or from the focused st/tmux terminal pane; call 'terminal [lines]' directly for tmux output.\n"
		"- Call 'ui [@win] click <label>', 'ui [@win] rclick <label>', 'ui [@win] focus <label>', 'ui [@win] type [target |] <text>', 'ui [@win] submit [target |] <text>', or 'ui [@win] set <target> | <text_or_number>' to interact with UI controls across any application (prefix @<class_or_title> to target an unfocused window).\n"
		"To browse, read pages, or interact with an open browser tab:\n"
		"1. 'browser url <https://...>', 'browser search <query>', 'browser tab <title>', and 'browser click <label>' automatically wait for page load and return a readable page snapshot. Use 'browser read [offset|filter]' to read more page text or paginate long articles, 'browser wait [ms|text]' if a dynamic page is still loading, and 'browser inspect [filter]' to list controls with coordinates (*FOCUSED* marks active inputs; *OFF-SCREEN* controls lack screen coordinates and must be scrolled or filtered).\n"
		"2. After navigating or clicking a link/button, always inspect the returned snapshot (or call 'browser read' / 'browser inspect') before deciding your next step or summarizing content.\n"
		"3. For chat replies (e.g. WhatsApp Web), prefer a single 'browser' call with 'send <contact_or_entry> | <message>' (or 'browser click <contact>' then 'browser send <message>') without extra sleeps.\n"
		"After receiving tool results that complete the user's request, always output a brief 1-sentence confirmation to the user (never an empty reply).\n"
		"When answering the user directly, keep spoken replies concise, natural, and direct.");
}
