/**
 * @file ren-firefox.c
 * @brief Standalone ren extension for Firefox/Chromium AT-SPI2 + MozLZ4 browser automation.
 *
 * ZERO libjarvis or libX11 dependency — delegates window/input actions via ren_call_tool().
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#endif

#include <ttypt/stele.h>
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

static int ff_call_window_tool(const char *arg, char **out)
{
	char *res = NULL;
	int rc = ren_call_tool("x11_windows", arg, &res);
	if (rc != 0 && (!res || strcmp(res, "Error: unknown tool") == 0)) {
		free(res);
		res = NULL;
		rc = ren_call_tool("window", arg, &res);
	}
	if (out)
		*out = res;
	else
		free(res);
	return rc;
}

static bool ren_ff_is_browser_focused(void *ud)
{
	(void)ud;
	char *out = NULL;
	int rc = ff_call_window_tool("focused", &out);
	if (rc != 0 || !out) {
		free(out);
		/* If no window query tool is loaded (e.g. Wayland), allow a11y inspection */
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

	static const char *const BROWSERS[] = { "firefox", "chromium", "chrome", "brave", "safari", "edge", "msedge", "qutebrowser", NULL };
	for (size_t i = 0; BROWSERS[i]; i++) {
		char arg[64], *out = NULL;
		snprintf(arg, sizeof(arg), "focus %s", BROWSERS[i]);
		int rc = ff_call_window_tool(arg, &out);
		free(out);
		if (rc == 0) return true;
	}
	return false;
}

static bool ren_ff_get_geometry(void *ud, int *x, int *y, int *w, int *h)
{
	(void)ud;
	char *out = NULL;
	int rc = ff_call_window_tool("focused", &out);
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

static bool ren_ff_get_screen_bounds(void *ud, int *sw, int *sh)
{
	(void)ud;
	char *out = NULL;
	int rc = ff_call_window_tool("focused", &out);
	if (rc != 0 || !out) {
		free(out);
		return false;
	}
	int gsw = 0, gsh = 0;
	bool ok = extract_json_int(out, "sw", &gsw) &&
	          extract_json_int(out, "sh", &gsh) &&
	          gsw > 0 && gsh > 0;
	free(out);
	if (!ok) return false;
	*sw = gsw;
	*sh = gsh;
	return true;
}

static void ren_ff_click_xy(void *ud, int x, int y, int btn)
{
	(void)ud;
	char arg[64];
	snprintf(arg, sizeof(arg), "click %d %d %d", x, y, btn > 0 ? btn : 1);
	ren_call_tool("input", arg, NULL);
}

static int ren_ff_should_abort(void *ud)
{
	(void)ud;
	return ren_is_interrupted();
}

static firefox_core_ops_t ren_ff_ops(void)
{
	firefox_core_ops_t ops;
	memset(&ops, 0, sizeof(ops));
	ops.focus_browser        = ren_ff_focus_browser;
	ops.is_browser_focused   = ren_ff_is_browser_focused;
	ops.get_focused_geometry = ren_ff_get_geometry;
	ops.get_screen_bounds    = ren_ff_get_screen_bounds;
	ops.click_xy             = ren_ff_click_xy;
	ops.should_abort         = ren_ff_should_abort;
	ops.ud                   = NULL;
	return ops;
}

static void spawn_browser_url(const char *url)
{
#ifdef __APPLE__
	char *const argv[] = { "open", (char *)url, NULL };
	desktop_core_spawn_detached_argv(argv);
#elif defined(_WIN32)
	char *const argv[] = { "cmd.exe", "/c", "start", "", (char *)url, NULL };
	desktop_core_spawn_detached_argv(argv);
#else
	char *const argv[] = { "firefox", "--new-tab", (char *)url, NULL };
	if (desktop_core_spawn_detached_argv(argv) <= 0) {
		char *const xdg[] = { "xdg-open", (char *)url, NULL };
		desktop_core_spawn_detached_argv(xdg);
	}
#endif
}

static void ff_trim_trailing_ws(char *s)
{
	if (!s) return;
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int tool_browser(const char *args, char **out_result)
{
	firefox_core_ops_t ops = ren_ff_ops();
	while (args && *args == ' ') args++;

	if (!args || !*args || strcasecmp(args, "inspect") == 0 || strncasecmp(args, "inspect ", 8) == 0) {
		const char *filter_q = (args && strlen(args) > 8) ? (args + 8) : NULL;
		while (filter_q && *filter_q == ' ') filter_q++;
		if (filter_q && !*filter_q) filter_q = NULL;

		ren_ff_focus_browser(NULL);
		if (filter_q) {
			firefox_core_switch_tab(&ops, filter_q, NULL, 0);
		}
		firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
		size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
		char out[FF_INSPECT_BUF_SZ] = {0};
		firefox_core_inspect_a11y(&ops, tabs, ntabs, filter_q, out, sizeof(out));
		*out_result = strdup(out[0] ? out : "(no browser accessibility tree found)");
		return 0;
	}

	if (strcasecmp(args, "read") == 0 || strncasecmp(args, "read ", 5) == 0) {
		const char *arg = (strlen(args) > 5) ? (args + 5) : "";
		while (*arg == ' ') arg++;
		size_t skip_chars = 0;
		const char *filter_q = NULL;
		if (strncmp(arg, "offset=", 7) == 0 && isdigit((unsigned char)arg[7])) {
			skip_chars = (size_t)strtoul(arg + 7, NULL, 10);
		} else if (*arg && isdigit((unsigned char)*arg)) {
			char *end = NULL;
			unsigned long v = strtoul(arg, &end, 10);
			while (end && *end == ' ') end++;
			skip_chars = (size_t)v;
			if (end && *end) filter_q = end;
		} else if (*arg) {
			filter_q = arg;
		}

		ren_ff_focus_browser(NULL);
		firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
		size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
		char out[FF_INSPECT_BUF_SZ] = {0};
		firefox_core_read_a11y(&ops, tabs, ntabs, filter_q, skip_chars, out, sizeof(out));
		*out_result = strdup(out[0] ? out : "(no readable page text found)");
		return 0;
	}

	if (strcasecmp(args, "wait") == 0 || strncasecmp(args, "wait ", 5) == 0) {
		const char *arg = (strlen(args) > 5) ? (args + 5) : "";
		while (*arg == ' ') arg++;
		int timeout_ms = 6000;
		const char *wait_text = NULL;
		if (*arg && isdigit((unsigned char)*arg)) {
			char *end = NULL;
			long v = strtol(arg, &end, 10);
			if (v > 0) timeout_ms = (int)v;
			while (end && *end == ' ') end++;
			if (end && *end) wait_text = end;
		} else if (*arg) {
			wait_text = arg;
		}

		ren_ff_focus_browser(NULL);
		ff_doc_state_t doc;
		char reason[64] = {0};
		int wrc = firefox_core_wait_loaded(&ops, NULL, NULL, wait_text, timeout_ms, false, &doc, reason, sizeof(reason));
		if (wrc == -2) {
			*out_result = strdup("Error: interrupted while waiting for browser page");
			return -1;
		}
		firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
		size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
		char page[FF_INSPECT_BUF_SZ] = {0};
		firefox_core_read_a11y(&ops, tabs, ntabs, NULL, 0, page, sizeof(page));
		char *out = (char *)malloc(FF_INSPECT_BUF_SZ + 512);
		if (!out) return -1;
		snprintf(out, FF_INSPECT_BUF_SZ + 512, "wait %s (url=%s):\n%s",
		         reason, doc.url[0] ? doc.url : "(unknown)", page);
		*out_result = out;
		return (wrc == 1) ? 0 : -1;
	}

	if (strcasecmp(args, "close") == 0 || strncasecmp(args, "close ", 6) == 0) {
		const char *q = (strlen(args) > 6) ? (args + 6) : "";
		while (*q == ' ') q++;
		if (!ren_ff_focus_browser(NULL)) {
			*out_result = strdup("Error: no focused browser window to close tab");
			return -1;
		}
		char tab_title[128] = {0};
		if (*q && !firefox_core_switch_tab(&ops, q, tab_title, sizeof(tab_title))) {
			char err[300];
			snprintf(err, sizeof(err), "Error: no open browser tab matching: %.200s", q);
			*out_result = strdup(err);
			return -1;
		}
		char *in_out = NULL;
		int irc = ren_call_tool("input", "key ctrl+w", &in_out);
		if (irc != 0) {
			*out_result = in_out ? in_out : strdup("Error: input tool unavailable to close browser tab");
			return -1;
		}
		free(in_out);
		char out[256];
		if (tab_title[0])
			snprintf(out, sizeof(out), "closed tab '%s'", tab_title);
		else
			snprintf(out, sizeof(out), "closed current browser tab");
		*out_result = strdup(out);
		return 0;
	}

	if (strncasecmp(args, "tab ", 4) == 0) {
		const char *q = args + 4;
		while (*q == ' ') q++;
		ren_ff_focus_browser(NULL);
		char tab_title[128] = {0};
		if (firefox_core_switch_tab(&ops, q, tab_title, sizeof(tab_title))) {
			ff_doc_state_t doc;
			firefox_core_probe_doc(&doc);
			if (!(doc.found && doc.showing && !doc.defunct && !doc.busy &&
			      doc.url[0] != '\0' && strcmp(doc.url, "about:blank") != 0 &&
			      doc.child_count > 0)) {
				char reason[64] = {0};
				int wrc = firefox_core_wait_loaded(&ops, NULL, NULL, NULL, 1200, false, &doc, reason, sizeof(reason));
				if (wrc == -2) {
					*out_result = strdup("Error: interrupted after switching browser tab");
					return -1;
				}
			}
			firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
			size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
			char page[FF_INSPECT_BUF_SZ] = {0};
			firefox_core_read_a11y(&ops, tabs, ntabs, NULL, 0, page, sizeof(page));
			char *out = (char *)malloc(FF_INSPECT_BUF_SZ + 256);
			if (!out) return -1;
			snprintf(out, FF_INSPECT_BUF_SZ + 256, "switched to tab '%s':\n%s", tab_title, page);
			*out_result = out;
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
		ff_doc_state_t prev_doc;
		firefox_core_probe_doc(&prev_doc);

		char role[64] = {0}, msg[512] = {0};
		if (firefox_core_click_element(&ops, q, 1, role, sizeof(role), msg, sizeof(msg))) {
			char *out = (char *)malloc(FF_INSPECT_BUF_SZ + 1024);
			if (!out) return -1;
			if (strcmp(role, "table row") == 0 || strcmp(role, "page tab") == 0 || strcmp(role, "list item") == 0 ||
			    strcmp(role, "TabItem") == 0 || strcmp(role, "ListItem") == 0 || strcmp(role, "DataItem") == 0 ||
			    strcmp(role, "AXRow") == 0 || strcmp(role, "AXTab") == 0 || strcmp(role, "AXRadioButton") == 0) {
				stl_sleep_ms(160);
				firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
				size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
				char page[FF_INSPECT_BUF_SZ] = {0};
				firefox_core_inspect_a11y(&ops, tabs, ntabs, NULL, page, sizeof(page));
				ff_focused_input_t foc;
				if (firefox_core_get_focused_input(&ops, &foc)) {
					snprintf(out, FF_INSPECT_BUF_SZ + 1024, "%s (focused: [%s '%.48s'])%s%s",
					         msg, foc.role, foc.label, page[0] ? "\n" : "", page);
				} else {
					snprintf(out, FF_INSPECT_BUF_SZ + 1024, "%s%s%s", msg, page[0] ? "\n" : "", page);
				}
			} else {
				ff_doc_state_t after_doc;
				char reason[64] = {0};
				int wrc = firefox_core_wait_loaded(
					&ops, prev_doc.found ? prev_doc.url : NULL, NULL, NULL,
					2500, true, &after_doc, reason, sizeof(reason));
				if (wrc == -2) {
					free(out);
					*out_result = strdup("Error: interrupted after browser click");
					return -1;
				}
				firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
				size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
				char page[FF_INSPECT_BUF_SZ] = {0};
				firefox_core_read_a11y(&ops, tabs, ntabs, NULL, 0, page, sizeof(page));
				ff_focused_input_t foc;
				if (firefox_core_get_focused_input(&ops, &foc)) {
					snprintf(out, FF_INSPECT_BUF_SZ + 1024, "%s (%s; focused: [%s '%.48s'])%s%s",
					         msg, reason, foc.role, foc.label, page[0] ? "\n" : "", page);
				} else {
					snprintf(out, FF_INSPECT_BUF_SZ + 1024, "%s (%s)%s%s",
					         msg, reason, page[0] ? "\n" : "", page);
				}
			}
			*out_result = out;
			return 0;
		}
		if (msg[0]) {
			*out_result = strdup(msg);
		} else {
			char err[300];
			snprintf(err, sizeof(err), "Error: UI element not found in accessibility tree: %.200s", q);
			*out_result = strdup(err);
		}
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
		if (!text[0]) {
			*out_result = strdup("Error: usage: browser send [target |] <text>");
			return -1;
		}

		char click_msg[512] = {0}, role[64] = {0};
		if (target[0]) {
			if (!firefox_core_click_element(&ops, target, 1, role, sizeof(role), click_msg, sizeof(click_msg))) {
				if (click_msg[0]) {
					*out_result = strdup(click_msg);
				} else {
					char err[300];
					snprintf(err, sizeof(err), "Error: UI element not found: %.200s", target);
					*out_result = strdup(err);
				}
				return -1;
			}
			if (strcmp(role, "entry") != 0 && strcmp(role, "combo box") != 0 &&
			    strcmp(role, "Edit") != 0 && strcmp(role, "ComboBox") != 0 &&
			    strcmp(role, "AXTextField") != 0 && strcmp(role, "AXTextArea") != 0 &&
			    strcmp(role, "AXComboBox") != 0) {
				stl_sleep_ms(160);
			}
		}

		if (!ren_ff_is_browser_focused(NULL)) {
			ren_ff_focus_browser(NULL);
			stl_sleep_ms(40);
			if (!ren_ff_is_browser_focused(NULL)) {
				*out_result = strdup("Error: browser window is not focused; refusing to type");
				return -1;
			}
		}

		ff_focused_input_t foc;
		char ferr[512] = {0};
		if (!firefox_core_ensure_editable_focused(&ops, &foc, ferr, sizeof(ferr))) {
			*out_result = strdup(ferr[0] ? ferr : "Error: no editable input focused in browser");
			return -1;
		}

		if (!ren_ff_is_browser_focused(NULL)) {
			*out_result = strdup("Error: browser window lost focus before typing");
			return -1;
		}

		char input_arg[600], *input_out = NULL;
		snprintf(input_arg, sizeof(input_arg), "submit %s", text);
		if (ren_call_tool("input", input_arg, &input_out) != 0) {
			*out_result = input_out ? input_out : strdup("Error: input tool unavailable to type text");
			return -1;
		}
		free(input_out);
		char out[768];
		snprintf(out, sizeof(out), "%s%styped+enter into [%s '%.64s']: %.120s",
		         click_msg, click_msg[0] ? " -> " : "",
		         foc.found ? foc.role : "input",
		         foc.found ? foc.label : "",
		         text);
		*out_result = strdup(out);
		return 0;
	}

	bool is_url_cmd = false;
	bool is_search_cmd = false;
	char url[768] = {0};
	if (strncasecmp(args, "url ", 4) == 0) {
		is_url_cmd = true;
		const char *u = args + 4;
		while (*u == ' ') u++;
		if (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0 || strncmp(u, "file://", 7) == 0) {
			snprintf(url, sizeof(url), "%s", u);
		} else {
			snprintf(url, sizeof(url), "https://%s", u);
		}
		ff_trim_trailing_ws(url);
	} else if (strncmp(args, "http://", 7) == 0 || strncmp(args, "https://", 8) == 0 || strncmp(args, "file://", 7) == 0) {
		is_url_cmd = true;
		snprintf(url, sizeof(url), "%s", args);
		ff_trim_trailing_ws(url);
	} else if (strncasecmp(args, "search ", 7) == 0) {
		is_search_cmd = true;
		const char *q = args + 7;
		while (*q == ' ') q++;
		char enc[512];
		ff_url_encode(q, enc, sizeof(enc));
		snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", enc);
	} else {
		is_search_cmd = true;
		char enc[512];
		ff_url_encode(args, enc, sizeof(enc));
		snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", enc);
	}

	ff_doc_state_t prev_doc;
	firefox_core_probe_doc(&prev_doc);

	if (is_url_cmd && url[0]) {
		if (prev_doc.found && firefox_core_url_matches(prev_doc.url, url) && ren_ff_focus_browser(NULL)) {
			ff_doc_state_t doc;
			char reason[64] = {0};
			int wrc = firefox_core_wait_loaded(&ops, NULL, url, NULL, 2500, false, &doc, reason, sizeof(reason));
			if (wrc == -2) {
				*out_result = strdup("Error: interrupted while reading active URL");
				return -1;
			}
			firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
			size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
			char page[FF_INSPECT_BUF_SZ] = {0};
			firefox_core_read_a11y(&ops, tabs, ntabs, NULL, 0, page, sizeof(page));
			char *out = (char *)malloc(FF_INSPECT_BUF_SZ + 1024);
			if (!out) return -1;
			snprintf(out, FF_INSPECT_BUF_SZ + 1024, "focused existing tab '%s' (%s):\n%s",
			         doc.title[0] ? doc.title : url, reason, page);
			*out_result = out;
			return 0;
		}
		firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
		size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
		for (size_t i = 0; i < ntabs; i++) {
			if (firefox_core_url_matches(tabs[i].url, url) && tabs[i].title[0] && ren_ff_focus_browser(NULL)) {
				char tab_title[128] = {0};
				if (firefox_core_switch_tab(&ops, tabs[i].title, tab_title, sizeof(tab_title))) {
					ff_doc_state_t doc;
					char reason[64] = {0};
					int wrc = firefox_core_wait_loaded(&ops, NULL, url, NULL, 2500, false, &doc, reason, sizeof(reason));
					if (wrc == -2) {
						*out_result = strdup("Error: interrupted while switching to existing tab");
						return -1;
					}
					if (!doc.found || firefox_core_url_matches(doc.url, url)) {
						char page[FF_INSPECT_BUF_SZ] = {0};
						firefox_core_read_a11y(&ops, tabs, ntabs, NULL, 0, page, sizeof(page));
						char *out = (char *)malloc(FF_INSPECT_BUF_SZ + 1024);
						if (!out) return -1;
						snprintf(out, FF_INSPECT_BUF_SZ + 1024, "focused existing tab '%s' (%s):\n%s",
						         tab_title[0] ? tab_title : tabs[i].title, reason, page);
						*out_result = out;
						return 0;
					}
				}
				break;
			}
		}
	}

	spawn_browser_url(url);
	ren_ff_focus_browser(NULL);
	ff_doc_state_t doc;
	char reason[64] = {0};
	int wrc = firefox_core_wait_loaded(
		&ops, prev_doc.found ? prev_doc.url : NULL, url, NULL, 6000, false, &doc, reason, sizeof(reason));
	if (wrc == -2) {
		*out_result = strdup("Error: interrupted while loading URL");
		return -1;
	}
	firefox_tab_t tabs[FIREFOX_CORE_MAX_TABS];
	size_t ntabs = firefox_core_populate_tabs(tabs, FIREFOX_CORE_MAX_TABS);
	char page[FF_INSPECT_BUF_SZ] = {0};
	firefox_core_read_a11y(&ops, tabs, ntabs, NULL, 0, page, sizeof(page));
	char *out = (char *)malloc(FF_INSPECT_BUF_SZ + 1024);
	if (!out) return -1;
	snprintf(out, FF_INSPECT_BUF_SZ + 1024, "%s: %s (%s)%s%s",
	         is_search_cmd ? "searched" : "opened URL",
	         url, reason, page[0] ? "\n" : "", page);
	*out_result = out;
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
	ren_register_tool("browser", "browser <inspect [filter] | read [offset|filter] | wait [ms|text] | tab <title_or_url_substring> | close [tab_query] | url <https://...> | search <query> | click <element_label> | send <element_label> | <text>>", tool_browser);
	ren_register_context_provider(ff_context_provider);
}
