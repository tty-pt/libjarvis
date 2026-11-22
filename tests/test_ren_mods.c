#define _GNU_SOURCE
#include "ttypt/libren.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	test_load_all_ren_modules();
	return 0;
}
