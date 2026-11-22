/**
 * @file ren-firefox.c
 * @brief Standalone ren extension for Firefox/Chromium AT-SPI2 + MozLZ4 browser automation.
 *
 * ZERO libjarvis dependency — works standalone with `ren -m ren-firefox:ren-x11:ren-mm`.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ttypt/libren.h"
#include <ttypt/xy.h>

#include "../x11/x11_core.h"
#include "firefox_core.h"

static bool ren_ff_focus_browser(void *ud)
{
	(void)ud;
	x11_win_info_t clients[32];
	x11_win_info_t foc, oth;
	size_t n = x11_core_query_windows(clients, 32, &foc, &oth);
	if (x11_core_is_browser_class(foc.wm_class)) return true;

	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < n; i++) {
			if (pass == 0 && !clients[i].visible) continue;
			if (x11_core_is_browser_class(clients[i].wm_class)) {
				return x11_core_focus_ewmh(clients[i].id, 1.2);
			}
		}
	}
	return false;
}

static bool ren_ff_is_browser_focused(void *ud)
{
	(void)ud;
	x11_win_info_t foc;
	return x11_core_get_focused(&foc) && x11_core_is_browser_class(foc.wm_class);
}

static bool ren_ff_get_geometry(void *ud, int *x, int *y, int *w, int *h)
{
	(void)ud;
	x11_core_get_geometry(NULL, x, y, w, h);
	return true;
}

static void ren_ff_click_xy(void *ud, int x, int y, int btn)
{
	(void)ud;
	x11_core_mouse_click(x, y, btn > 0 ? btn : 1);
}

static firefox_core_ops_t ren_ff_ops(void)
{
	firefox_core_ops_t ops;
	ops.focus_browser        = ren_ff_focus_browser;
	ops.is_browser_focused   = ren_ff_is_browser_focused;
	ops.get_focused_geometry = ren_ff_get_geometry;
	ops.click_xy             = ren_ff_click_xy;
	ops.ud                   = NULL;
	return ops;
}

static void spawn_browser_url(const char *url)
{
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		execlp("firefox", "firefox", "--new-tab", url, (char *)NULL);
		execlp("xdg-open", "xdg-open", url, (char *)NULL);
		_exit(127);
	}
}

static int tool_browser(const char *args, char **out_result)
{
	firefox_core_ops_t ops = ren_ff_ops();
	while (args && *args == ' ') args++;

	if (!args || !*args || strncasecmp(args, "inspect", 7) == 0) {
		const char *filter_q = (args && strlen(args) > 7) ? (args + 7) : NULL;
		while (filter_q && *filter_q == ' ') filter_q++;
		if (filter_q && !*filter_q) filter_q = NULL;

		ren_ff_focus_browser(NULL);
		if (filter_q) {
			firefox_core_switch_tab(&ops, filter_q, NULL, 0);
		}
		firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
		size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
		char out[2048] = {0};
		firefox_core_inspect_a11y(&ops, tabs, ntabs, filter_q, out, sizeof(out));
		*out_result = strdup(out[0] ? out : "(no browser accessibility tree found)");
		return 0;
	}

	if (strncasecmp(args, "tab ", 4) == 0) {
		const char *q = args + 4;
		while (*q == ' ') q++;
		ren_ff_focus_browser(NULL);
		char tab_title[128] = {0};
		if (firefox_core_switch_tab(&ops, q, tab_title, sizeof(tab_title))) {
			firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
			size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
			char page[1850] = {0}, out[2048] = {0};
			firefox_core_inspect_a11y(&ops, tabs, ntabs, NULL, page, sizeof(page));
			snprintf(out, sizeof(out), "switched to tab '%s':\n%.1850s", tab_title, page);
			*out_result = strdup(out);
			return 0;
		}
		char err[300];
		snprintf(err, sizeof(err), "Error: no open browser tab matching: %.200s", q);
		*out_result = strdup(err);
		return -1;
	}

	if (strncasecmp(args, "click ", 6) == 0) {
		const char *q = args + 6;
		while (*q == ' ') q++;
		char role[64] = {0}, msg[512] = {0};
		if (firefox_core_click_element(&ops, q, 1, role, sizeof(role), msg, sizeof(msg))) {
			char out[2048];
			if (strcmp(role, "table row") == 0 || strcmp(role, "page tab") == 0 || strcmp(role, "list item") == 0) {
				usleep(120000);
				firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
				size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
				char page[1400] = {0};
				firefox_core_inspect_a11y(&ops, tabs, ntabs, "entry", page, sizeof(page));
				snprintf(out, sizeof(out), "%s%s%s", msg, page[0] ? "\n" : "", page);
			} else {
				snprintf(out, sizeof(out), "%s", msg);
			}
			*out_result = strdup(out);
			return 0;
		}
		char err[300];
		snprintf(err, sizeof(err), "Error: UI element not found in accessibility tree: %.200s", q);
		*out_result = strdup(err);
		return -1;
	}

	if (strncasecmp(args, "send ", 5) == 0) {
		const char *raw = args + 5;
		while (*raw == ' ') raw++;
		const char *pipe = strchr(raw, '|');
		char target[256] = {0}, text[512] = {0};
		if (pipe) {
			size_t tlen = (size_t)(pipe - raw);
			while (tlen > 0 && isspace((unsigned char)raw[tlen - 1])) tlen--;
			if (tlen >= sizeof(target)) tlen = sizeof(target) - 1;
			memcpy(target, raw, tlen);
			target[tlen] = '\0';
			const char *m = pipe + 1;
			while (*m == ' ') m++;
			snprintf(text, sizeof(text), "%s", m);
		} else {
			snprintf(text, sizeof(text), "%s", raw);
		}

		char click_msg[160] = {0}, role[64] = {0};
		if (target[0]) {
			if (!firefox_core_click_element(&ops, target, 1, role, sizeof(role), click_msg, sizeof(click_msg))) {
				char err[300];
				snprintf(err, sizeof(err), "Error: UI element not found: %.200s", target);
				*out_result = strdup(err);
				return -1;
			}
			if (strcmp(role, "entry") != 0 && strcmp(role, "combo box") != 0) usleep(160000);
		}
		firefox_core_ensure_entry_focused(&ops);
		x11_core_type_text(text, true);
		char out[512];
		if (click_msg[0]) {
			snprintf(out, sizeof(out), "%s -> typed+enter: %.120s", click_msg, text);
		} else {
			snprintf(out, sizeof(out), "typed+enter: %.120s", text);
		}
		*out_result = strdup(out);
		return 0;
	}

	char url[768] = {0};
	if (strncasecmp(args, "url ", 4) == 0) {
		const char *u = args + 4;
		while (*u == ' ') u++;
		if (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0) {
			snprintf(url, sizeof(url), "%s", u);
		} else {
			snprintf(url, sizeof(url), "https://%s", u);
		}
	} else if (strncmp(args, "http://", 7) == 0 || strncmp(args, "https://", 8) == 0) {
		snprintf(url, sizeof(url), "%s", args);
	} else if (strncasecmp(args, "search ", 7) == 0) {
		const char *q = args + 7;
		while (*q == ' ') q++;
		char enc[512];
		ff_url_encode(q, enc, sizeof(enc));
		snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", enc);
	} else {
		char enc[512];
		ff_url_encode(args, enc, sizeof(enc));
		snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", enc);
	}

	if (strncasecmp(args, "url ", 4) == 0 || strncmp(args, "http://", 7) == 0 || strncmp(args, "https://", 8) == 0) {
		char domain_kw[64] = {0};
		firefox_core_extract_domain_kw(url, domain_kw, sizeof(domain_kw));
		if (strlen(domain_kw) >= 3 && ren_ff_focus_browser(NULL)) {
			char tab_title[128] = {0};
			if (firefox_core_switch_tab(&ops, domain_kw, tab_title, sizeof(tab_title))) {
				firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
				size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
				char page[1850] = {0}, out[2048] = {0};
				firefox_core_inspect_a11y(&ops, tabs, ntabs, NULL, page, sizeof(page));
				snprintf(out, sizeof(out), "focused existing tab '%s':\n%.1850s", tab_title, page);
				*out_result = strdup(out);
				return 0;
			}
		}
	}

	spawn_browser_url(url);
	ren_ff_focus_browser(NULL);
	char out[832];
	snprintf(out, sizeof(out), "opened URL: %s", url);
	*out_result = strdup(out);
	return 0;
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("browser", "browser <inspect [filter] | tab <title_or_url_substring> | url <https://...> | search <query> | click <element_label> | send <element_label> | <text>>", tool_browser);
}
