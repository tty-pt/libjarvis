#define _GNU_SOURCE
#include "ttypt/libren.h"
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

	printf("  [PASS] URL match, offset pagination, and cooperative wait abort passed!\n");
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

	/* Verify ren-firefox rejects nonexistent tab cleanly without side effects */
	rc = ren_call_tool("browser", "tab __nonexistent_jarvis_tab_xyz__", &out);
	assert(rc != 0);
	assert(out != NULL && strstr(out, "no open browser tab matching") != NULL);
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
