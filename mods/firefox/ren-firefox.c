/**
 * @file ren-firefox.c
 * @brief Standalone ren extension for Firefox/Chromium AT-SPI2 + MozLZ4 browser automation.
 *
 * ZERO libjarvis or libX11 dependency — delegates window/input actions via ren_call_tool().
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
#include "../../include/ttypt/desktop-core.h"

#include "firefox_core.h"

static bool extract_json_str(const char *json, const char *key, char *out, size_t out_sz)
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

static bool extract_json_int(const char *json, const char *key, int *out_val)
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

static bool ren_ff_is_browser_focused(void *ud)
{
	(void)ud;
	char *out = NULL;
	int rc = ren_call_tool("x11_windows", "focused", &out);
	if (rc != 0 || !out) {
		free(out);
		/* If no X11 window query tool is loaded (e.g. Wayland), allow AT-SPI2 inspection */
		return true;
	}
	char cls[128] = {0};
	extract_json_str(out, "class", cls, sizeof(cls));
	free(out);
	return ff_is_browser_name(cls);
}

static bool ren_ff_focus_browser(void *ud)
{
	(void)ud;
	if (ren_ff_is_browser_focused(NULL)) return true;

	static const char *const BROWSERS[] = { "firefox", "chromium", "chrome", "brave", "qutebrowser", NULL };
	for (size_t i = 0; BROWSERS[i]; i++) {
		char arg[64], *out = NULL;
		snprintf(arg, sizeof(arg), "focus %s", BROWSERS[i]);
		int rc = ren_call_tool("x11_windows", arg, &out);
		free(out);
		if (rc == 0) return true;
	}
	for (size_t i = 0; BROWSERS[i]; i++) {
		char arg[64], *out = NULL;
		snprintf(arg, sizeof(arg), "focus %s", BROWSERS[i]);
		int rc = ren_call_tool("window", arg, &out);
		free(out);
		if (rc == 0) return true;
	}
	return false;
}

static bool ren_ff_get_geometry(void *ud, int *x, int *y, int *w, int *h)
{
	(void)ud;
	char *out = NULL;
	int rc = ren_call_tool("x11_windows", "focused", &out);
	if (rc != 0 || !out) {
		free(out);
		return false;
	}
	int gx = 0, gy = 0, gw = 0, gh = 0;
	bool ok = extract_json_int(out, "x", &gx) &&
	          extract_json_int(out, "y", &gy) &&
	          extract_json_int(out, "w", &gw) &&
	          extract_json_int(out, "h", &gh) &&
	          gw > 0 && gh > 0;
	free(out);
	if (!ok) return false;
	*x = gx;
	*y = gy;
	*w = gw;
	*h = gh;
	return true;
}

static void ren_ff_click_xy(void *ud, int x, int y, int btn)
{
	(void)ud;
	char arg[64];
	snprintf(arg, sizeof(arg), "click %d %d %d", x, y, btn > 0 ? btn : 1);
	ren_call_tool("input", arg, NULL);
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
	char *const argv[] = { "firefox", "--new-tab", (char *)url, NULL };
	if (desktop_core_spawn_detached_argv(argv) <= 0) {
		char *const xdg[] = { "xdg-open", (char *)url, NULL };
		desktop_core_spawn_detached_argv(xdg);
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
		char input_arg[600], *input_out = NULL;
		snprintf(input_arg, sizeof(input_arg), "submit %s", text);
		if (ren_call_tool("input", input_arg, &input_out) != 0) {
			*out_result = input_out ? input_out : strdup("Error: input tool unavailable to type text");
			return -1;
		}
		free(input_out);
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

static char *ff_context_provider(void)
{
	firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
	size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
	if (ntabs == 0) return NULL;

	char buf[640];
	size_t off = (size_t)snprintf(buf, sizeof(buf), "browser_tabs=%zu [", ntabs);
	for (size_t i = 0; i < ntabs && off + 48 < sizeof(buf); i++) {
		const char *label = tabs[i].title[0] ? tabs[i].title : tabs[i].url;
		off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s'%.48s'",
		                        i > 0 ? ", " : "", label);
	}
	snprintf(buf + off, sizeof(buf) - off, "]");
	return strdup(buf);
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("browser", "browser <inspect [filter] | tab <title_or_url_substring> | url <https://...> | search <query> | click <element_label> | send <element_label> | <text>>", tool_browser);
	ren_register_context_provider(ff_context_provider);
}
