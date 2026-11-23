#define _GNU_SOURCE
#include "ttypt/libren.h"
#include "ttypt/desktop-core.h"
#include "../mods/desktop/atspi_core.h"
#include "../mods/firefox/firefox_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_abort_cb(void *ud)
{
	int *flag = (int *)ud;
	if (*flag >= 1) return 1;
	(*flag)++;
	return 0;
}

static void test_firefox_core_url_read_and_interrupt(void)
{
	printf("Running test_firefox_core_url_read_and_interrupt...\n");

	/* 1. URL matching: exact normalized URL vs same-host different-path */
	assert(firefox_core_url_matches("https://www.github.com/torvalds/linux/", "http://github.com/torvalds/linux"));
	assert(!firefox_core_url_matches("https://github.com/torvalds/linux", "https://github.com/torvalds/test"));
	assert(firefox_core_url_host_matches("https://github.com/torvalds/linux", "https://github.com/torvalds/test"));
	assert(!firefox_core_url_matches("https://example.com/search?q=one", "https://example.com/search?q=two"));

	/* 2. HTML fallback readability & offset pagination (> FF_INSPECT_BUF_SZ) */
	char html_path[] = "/tmp/test_ff_page_XXXXXX.html";
	int hfd = mkstemps(html_path, 5);
	assert(hfd >= 0);
	dprintf(hfd, "<html><head><script>var secret_js_should_hide=1;</script></head><body>");
	dprintf(hfd, "<h1>Headline Alpha</h1>");
	for (int i = 0; i < 240; i++) {
		dprintf(hfd, "<p>Paragraph block %03d with middle text preserved cleanly.</p>", i);
	}
	dprintf(hfd, "<p>Final Tail Marker Omega</p></body></html>");
	close(hfd);

	char file_url[600];
	snprintf(file_url, sizeof(file_url), "file://%s", html_path);

	char buf[FF_INSPECT_BUF_SZ] = {0};
	size_t off = 0;
	bool trunc = false;
	ff_extract_html_fallback(file_url, 0, buf, sizeof(buf), &off, &trunc);
	assert(off > 7000);
	assert(trunc == true);
	assert(strstr(buf, "Headline Alpha") != NULL);
	assert(strstr(buf, "Paragraph block 050") != NULL);
	assert(strstr(buf, "secret_js_should_hide") == NULL);

	char page2[FF_INSPECT_BUF_SZ] = {0};
	size_t off2 = 0;
	bool trunc2 = false;
	ff_extract_html_fallback(file_url, 7500, page2, sizeof(page2), &off2, &trunc2);
	assert(off2 > 0);
	assert(strstr(page2, "Final Tail Marker Omega") != NULL);
	unlink(html_path);

	/* 3. wait_loaded aborts immediately on should_abort */
	int abort_ticks = 0;
	firefox_core_ops_t ops;
	memset(&ops, 0, sizeof(ops));
	ops.should_abort = test_abort_cb;
	ops.ud = &abort_ticks;

	ff_doc_state_t doc;
	char reason[64] = {0};
	int wrc = firefox_core_wait_loaded(&ops, NULL, "https://example.invalid/never", NULL, 10000, false, &doc, reason, sizeof(reason));
	assert(wrc == -2);
	assert(strcmp(reason, "interrupted") == 0);
	assert(abort_ticks == 1);

	/* 4. Viewport bounds clipping (prevents clicking off-screen scrolled elements) */
	assert(ff_point_in_viewport(400, 400, true, 0, 33, 1596, 863, true, 1600, 900));
	assert(!ff_point_in_viewport(400, 1086, true, 0, 33, 1596, 863, true, 1600, 900));
	assert(!ff_point_in_viewport(400, 1166, true, 0, 33, 1596, 863, true, 1600, 900));
	assert(!ff_point_in_viewport(400, 10, true, 0, 33, 1596, 863, true, 1600, 900));
	assert(!ff_point_in_viewport(311, 400, true, 800, 33, 796, 863, true, 1600, 900));
	assert(ff_point_in_viewport(1100, 400, true, 800, 33, 796, 863, true, 1600, 900));
	assert(!ff_point_in_viewport(-5, 100, false, 0, 0, 0, 0, true, 1600, 900));

	printf("  [PASS] URL match, offset pagination, cooperative wait abort, and viewport bounds passed!\n");
}

static void test_load_all_ren_modules(void)
{
	printf("Running test_load_all_ren_modules...\n");

	/* Isolate ~/.jarvisrc for ren-jarvis */
	char tmp_rc[] = "/tmp/test_ren_jarvisrc_XXXXXX";
	int fd = mkstemp(tmp_rc);
	assert(fd >= 0);
	dprintf(fd, "wake_word = Jarbas\nmodel = fast\n");
	close(fd);
	setenv("JARVISRC", tmp_rc, 1);

	const char *mods[] = {
		"../mods/desktop/ren-desktop.so",
		"../mods/ren-jarvis/ren-jarvis.so",
		"../mods/x11/ren-x11.so",
		"../mods/firefox/ren-firefox.so",
		"../mods/sway/ren-sway.so",
		"../mods/hyprland/ren-hyprland.so",
		NULL
	};

	for (size_t i = 0; mods[i]; i++) {
		int rc = ren_load_module(mods[i]);
		if (rc != 0) {
			fprintf(stderr, "FAIL: ren_load_module(%s) returned %d\n", mods[i], rc);
			assert(rc == 0);
		}
	}

	/* Verify ren-desktop open_app validation (reject unknown non-app phrases) */
	char *out = NULL;
	int rc = ren_call_tool("open_app", "my dev server", &out);
	assert(rc != 0);
	assert(out != NULL && strstr(out, "unknown application") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("open_app", "", &out);
	assert(rc != 0);
	free(out);
	out = NULL;

	/* Verify ren-desktop system tool emits valid JSON summary */
	rc = ren_call_tool("system", "disk", &out);
	assert(rc == 0);
	assert(out != NULL && strstr(out, "\"health\"") != NULL && strstr(out, "\"disk\"") != NULL);
	free(out);
	out = NULL;

	/* Verify ren-x11 focused window reports screen bounds and pid */
	rc = ren_call_tool("x11_windows", "focused", &out);
	assert(rc == 0 && out != NULL);
	assert(strstr(out, "\"pid\":") != NULL);
	if (strstr(out, "\"w\":0") == NULL) {
		assert(strstr(out, "\"sw\":") != NULL && strstr(out, "\"sh\":") != NULL);
	}
	free(out);
	out = NULL;

	/* Verify ren-desktop terminal & read_window tools execute cleanly and never leak jarvis tmux session */
	rc = ren_call_tool("terminal", "10", &out);
	assert(rc == 0 && out != NULL && out[0] != '\0');
	assert(strstr(out, "[tmux jarvis:") == NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("read_window", "", &out);
	assert(rc == 0 && out != NULL && out[0] != '\0');
	free(out);
	out = NULL;

	rc = ren_call_tool("read_window", "inspect", &out);
	assert(rc == 0 && out != NULL && out[0] != '\0');
	free(out);
	out = NULL;

	/* Verify desktop-core & atspi-core standalone helpers */
	assert(desktop_core_is_terminal_class("st-256color"));
	assert(desktop_core_is_terminal_class("Alacritty"));
	assert(!desktop_core_is_terminal_class("firefox_firefox"));
	assert(desktop_core_is_browser_name("firefox"));
	assert(desktop_core_is_browser_name("Google Chrome"));
	assert(desktop_core_is_browser_name("msedge"));
	assert(desktop_core_is_browser_name("Safari"));
	assert(!desktop_core_is_browser_name("Alacritty"));
	assert(!desktop_core_is_browser_name(""));
	assert(!desktop_core_is_browser_name(NULL));
	{
		char cap_lines[64];
		snprintf(cap_lines, sizeof(cap_lines), "l1\nl2\nl3\n");
		desktop_core_cap_lines(cap_lines, 2);
		assert(strcmp(cap_lines, "l1\nl2") == 0);
		desktop_core_cap_lines(cap_lines, 5);
		assert(strcmp(cap_lines, "l1\nl2") == 0);
		desktop_core_cap_lines(cap_lines, 0);
		assert(strcmp(cap_lines, "") == 0);
	}
	{
		char cap_buf[64] = {0};
		char *const argv_echo[] = { "/bin/echo", "ok-capture", NULL };
		assert(desktop_core_exec_capture(argv_echo, cap_buf, sizeof(cap_buf)) == 0);
		assert(strcmp(cap_buf, "ok-capture") == 0);
	}
	{
		char clean[64] = {0};
		atspi_core_clean_text("Hello\xEF\xBF\xBC\xC2\xA0World\nLine2 ", clean, sizeof(clean));
		assert(strcmp(clean, "Hello World Line2") == 0);
	}
	assert(atspi_is_interactive_role("tree item"));
	assert(atspi_is_editable_role("spin button"));
	assert(atspi_score_text_field("Save As", "save as", 100, 70, 45) == 100);
	assert(atspi_score_text_field("Save As", "save", 100, 70, 45) == 70);
	assert(atspi_score_text_field("File Save", "save", 100, 70, 45) == 45);
	assert(atspi_score_text_field("Cancel", "save", 100, 70, 45) == 0);

	/* Verify ren-desktop ui tool validates args, @win, and missing targets cleanly */
	rc = ren_call_tool("ui", "", &out);
	assert(rc != 0 && out != NULL && strstr(out, "usage:") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "bogus", &out);
	assert(rc != 0 && out != NULL && strstr(out, "usage:") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "type target_only |   ", &out);
	assert(rc != 0 && out != NULL && strstr(out, "usage:") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "submit | hello", &out);
	assert(rc != 0 && out != NULL && strstr(out, "usage:") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "set missing_pipe", &out);
	assert(rc != 0 && out != NULL && strstr(out, "usage:") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "@__nonexistent_jarvis_win_xyz__ click Save", &out);
	assert(rc != 0 && out != NULL && strstr(out, "not found") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("read_window", "@__nonexistent_jarvis_win_xyz__ inspect", &out);
	assert(rc != 0 && out != NULL && strstr(out, "not found") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "click __nonexistent_jarvis_ctrl_xyz__", &out);
	assert(rc != 0 && out != NULL && strstr(out, "not found") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "focus __nonexistent_jarvis_ctrl_xyz__", &out);
	assert(rc != 0 && out != NULL && strstr(out, "not found") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("ui", "set __nonexistent_jarvis_ctrl_xyz__ | 42", &out);
	assert(rc != 0 && out != NULL && strstr(out, "not found") != NULL);
	free(out);
	out = NULL;

	/* Verify ren-firefox rejects nonexistent tab cleanly without side effects */
	rc = ren_call_tool("browser", "tab __nonexistent_jarvis_tab_xyz__", &out);
	assert(rc != 0);
	assert(out != NULL && strstr(out, "no open browser tab matching") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("browser", "close __nonexistent_jarvis_tab_xyz__", &out);
	assert(rc != 0);
	assert(out != NULL &&
	       (strstr(out, "no open browser tab matching") != NULL ||
	        strstr(out, "no focused browser window to close tab") != NULL));
	free(out);
	out = NULL;

	/* Verify ren-firefox send rejects empty text and nonexistent target cleanly */
	rc = ren_call_tool("browser", "send target_only |   ", &out);
	assert(rc != 0);
	assert(out != NULL && strstr(out, "usage:") != NULL);
	free(out);
	out = NULL;

	rc = ren_call_tool("browser", "send __nonexistent_target_xyz__ | hello", &out);
	assert(rc != 0);
	assert(out != NULL && strstr(out, "not found") != NULL);
	free(out);
	out = NULL;

	/* Verify ren-firefox read and wait commands handle safely */
	rc = ren_call_tool("browser", "read", &out);
	assert(rc == 0);
	assert(out != NULL && out[0] != '\0');
	free(out);
	out = NULL;

	rc = ren_call_tool("browser", "wait 120 __nonexistent_wait_marker_xyz__", &out);
	assert(rc != 0);
	assert(out != NULL && strstr(out, "wait timeout") != NULL);
	free(out);
	out = NULL;

	/* Verify ren-x11 / ren-sway / ren-hyprland workspace tool validates args */
	rc = ren_call_tool("workspace", "invalid", &out);
	assert(rc != 0);
	free(out);

	unsetenv("JARVISRC");
	unlink(tmp_rc);
	printf("  [PASS] All 6 ren-*.so plugins load cleanly and validate tool calls!\n");
}

int main(void)
{
	test_firefox_core_url_read_and_interrupt();
	test_load_all_ren_modules();
	return 0;
}
