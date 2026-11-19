#include "../include/ttypt/jarvis-types.h"
#include <ttypt/xy.h>
#include <ttypt/xy-mod.h>

#include <stdio.h>
#include <string.h>

XY_MODULE_API jarvis_dsl_t  g_executed[32];
XY_MODULE_API size_t        g_executed_count = 0;
XY_MODULE_API bool          g_allow_confirm  = true;
XY_MODULE_API jarvis_pane_t g_mock_panes[4];
XY_MODULE_API size_t        g_mock_pane_count = 0;
XY_MODULE_API char          g_mock_browser_page[JARVIS_OUTPUT_LEN] = {0};
XY_MODULE_API jarvis_tab_t  g_mock_tabs[4];
XY_MODULE_API size_t        g_mock_tab_count = 0;
XY_MODULE_API char          g_mock_other_term_tail[JARVIS_OUTPUT_LEN] = {0};

XY_MODULE_API void xy_install(void)
{
}

/* Mock Xylem Adapter Listeners */
XY_IMPL(int, jarvis_xy_observe, jarvis_world_t *, w)
{
	if (!w) return 0;
	snprintf(w->wm, sizeof(w->wm), "mock-dwm");
	snprintf(w->focused.id, sizeof(w->focused.id), "0x1001");
	snprintf(w->focused.wm_class, sizeof(w->focused.wm_class), "st-256color");
	snprintf(w->focused.title, sizeof(w->focused.title), "quirinpa@host:~/site");
	w->focused.tag = 1;
	w->focused.visible = true;
	snprintf(w->focused_cwd, sizeof(w->focused_cwd), "/home/quirinpa/site");

	snprintf(w->other.id, sizeof(w->other.id), "0x2002");
	snprintf(w->other.wm_class, sizeof(w->other.wm_class), "firefox");
	snprintf(w->other.title, sizeof(w->other.title), "Project Auren Status Dashboard - Mozilla Firefox");
	w->other.tag = 1;
	w->other.visible = true;

	/* Clients matching test_jarvis.py MockAdapter */
	w->client_count = 3;
	snprintf(w->clients[0].id, sizeof(w->clients[0].id), "0x1001");
	snprintf(w->clients[0].wm_class, sizeof(w->clients[0].wm_class), "st-256color");
	snprintf(w->clients[0].title, sizeof(w->clients[0].title), "tmux: main");
	w->clients[0].tag = 1;
	w->clients[0].visible = true;

	snprintf(w->clients[1].id, sizeof(w->clients[1].id), "0x2002");
	snprintf(w->clients[1].wm_class, sizeof(w->clients[1].wm_class), "firefox");
	snprintf(w->clients[1].title, sizeof(w->clients[1].title), "Project Auren Status Dashboard - Mozilla Firefox");
	w->clients[1].tag = 1;
	w->clients[1].visible = true;

	snprintf(w->clients[2].id, sizeof(w->clients[2].id), "0x3003");
	snprintf(w->clients[2].wm_class, sizeof(w->clients[2].wm_class), "discord");
	snprintf(w->clients[2].title, sizeof(w->clients[2].title), "Discord");
	w->clients[2].tag = 2;
	w->clients[2].visible = false;

	for (size_t i = 0; i < g_mock_pane_count && w->pane_count < JARVIS_MAX_PANES; i++) {
		w->panes[w->pane_count++] = g_mock_panes[i];
	}

	if (g_mock_browser_page[0]) {
		snprintf(w->browser_page, sizeof(w->browser_page), "%s", g_mock_browser_page);
	}
	for (size_t i = 0; i < g_mock_tab_count && w->tab_count < JARVIS_MAX_TABS; i++) {
		w->tabs[w->tab_count++] = g_mock_tabs[i];
	}

	static const struct { const char *type; const char *phrase; const char *id; } custom[] = {
		{"gui.application", "calculator", "gnome-calculator"},
		{"gui.application", "firefox web browser", "firefox"},
		{"gui.application", "pulseaudio volume control", "pavucontrol"},
		{"gui.application", "visual studio code", "code"},
		{"gui.application", "files", "thunar"},
		/* Custom domain entities: Music site & NeverDark & Filesystem */
		{"gui.application", "fado nocturnal poem", "poem:fado-nocturnal"},
		{"gui.application", "shadow goblin", "nd:mob:goblin"},
		{"fs.location", "site", "~/site"},
		{"fs.location", "site folder", "~/site"},
		{"fs.location", "site directory", "~/site"},
		{NULL, NULL, NULL}
	};
	for (size_t i = 0; custom[i].phrase && w->custom_entity_count < JARVIS_MAX_APPS; i++) {
		jarvis_live_entity_t *ce = &w->custom_entities[w->custom_entity_count++];
		snprintf(ce->type, sizeof(ce->type), "%s", custom[i].type);
		snprintf(ce->id, sizeof(ce->id), "%s", custom[i].id);
		snprintf(ce->phrase, sizeof(ce->phrase), "%s", custom[i].phrase);
	}
	return 0;
}

XY_IMPL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, res)
{
	if (!dsl || !res) return -1;
	if (g_executed_count < 32) {
		g_executed[g_executed_count++] = *dsl;
	}
	res->handled = true;
	res->success = true;

	if (strcmp(dsl->op, "inspect_terminal") == 0) {
		if (strcmp(dsl->target.relation, "other") == 0 && g_mock_other_term_tail[0]) {
			snprintf(
				res->output, sizeof(res->output),
				"{\"window\":{\"id\":\"0x1002\",\"class\":\"st-256color\",\"title\":\"worker-node\"},"
				"\"cwd\":\"/home/quirinpa/site\","
				"\"pane_tail\":\"%.1800s\"}",
				g_mock_other_term_tail
			);
			return 0;
		}
		if (g_mock_pane_count > 0 && g_mock_panes[0].tail[0]) {
			snprintf(
				res->output, sizeof(res->output),
				"{\"window\":{\"id\":\"0x1001\",\"class\":\"st-256color\",\"title\":\"quirinpa@host:~/site\"},"
				"\"cwd\":\"/home/quirinpa/site\","
				"\"pane_tail\":\"%.1800s\"}",
				g_mock_panes[0].tail
			);
			return 0;
		}
		snprintf(
			res->output, sizeof(res->output),
			"{\"window\":{\"id\":\"0x1001\",\"class\":\"st-256color\",\"title\":\"quirinpa@host:~/site\"},"
			"\"cwd\":\"/home/quirinpa/site\","
			"\"tmux_panes\":[{\"pane\":\"main:0.0\",\"command\":\"make\",\"cwd\":\"/home/quirinpa/site\",\"active\":true,"
			"\"tail\":\"cc -Wall -Werror -c src/audio.c\\nsrc/audio.c:88:12: error: expected ';' after expression 'pitch_init()'\\nmake: *** [Makefile:42: src/audio.o] Error 1\"}]}"
		);
		return 0;
	}
	if (strcmp(dsl->op, "inspect_browser") == 0) {
		const char *a11y = g_mock_browser_page[0] ? g_mock_browser_page :
			"[frame] Project Auren Status Dashboard - Mozilla Firefox | "
			"[document web] Project Auren Status Dashboard | "
			"[heading] Project Auren Status Dashboard | "
			"[paragraph] Active deployment node: auren-node-07 | "
			"[paragraph] Build pipeline status: ALL_TESTS_GREEN (42 suites passed in 0.18s) | "
			"[link] Open Architecture Documentation -> https://tty.pt/docs/architecture";
		snprintf(
			res->output, sizeof(res->output),
			"{\"accessibility_tree\":\"%.1750s\","
			"\"open_tabs\":[{\"index\":1,\"title\":\"Project Auren Status Dashboard\",\"url\":\"http://localhost:8080/status\"},"
			"{\"index\":2,\"title\":\"TTY.PT Architecture Docs\",\"url\":\"https://tty.pt/docs/architecture\"}]}",
			a11y
		);
		return 0;
	}
	if (strcmp(dsl->op, "inspect_clipboard") == 0) {
		snprintf(
			res->output, sizeof(res->output),
			"{\"primary_selection\":\"https://news.ycombinator.com\",\"clipboard\":\"https://news.ycombinator.com\"}"
		);
		return 0;
	}
	if (strcmp(dsl->op, "inspect_system") == 0) {
		snprintf(
			res->output, sizeof(res->output),
			"{\"memory\":\"Mem: 32Gi total, 28Gi used, 4.0Gi free\","
			"\"top_mem\":[\"PID COMMAND %%CPU %%MEM\",\"4812 blender 92.1 64.5\",\"2010 firefox 8.2 12.1\"]}"
		);
		return 0;
	}
	if (strcmp(dsl->op, "run_background_command") == 0) {
		snprintf(res->output, sizeof(res->output), "probe-output(%s): ok", dsl->target.command);
		return 0;
	}
	if (strcmp(dsl->op, "answer_question") == 0) {
		snprintf(res->output, sizeof(res->output), "%s",
		         dsl->args.answer[0] ? dsl->args.answer : "answered");
		return 0;
	}

	snprintf(res->output, sizeof(res->output), "mock-executed:%s", dsl->op);
	return 0;
}

XY_IMPL(int, jarvis_xy_confirm, const char *, desc)
{
	(void)desc;
	return g_allow_confirm ? 1 : 0;
}

