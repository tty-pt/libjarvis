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
	snprintf(w->focused_cwd, sizeof(w->focused_cwd), "/home/quirinpa/site");

	snprintf(w->other.id, sizeof(w->other.id), "0x2002");
	snprintf(w->other.wm_class, sizeof(w->other.wm_class), "firefox");
	snprintf(w->other.title, sizeof(w->other.title), "GitHub - Mozilla Firefox");

	/* Clients matching test_jarvis.py MockAdapter */
	w->client_count = 3;
	snprintf(w->clients[0].id, sizeof(w->clients[0].id), "0x1001");
	snprintf(w->clients[0].wm_class, sizeof(w->clients[0].wm_class), "st-256color");
	snprintf(w->clients[0].title, sizeof(w->clients[0].title), "tmux: main");
	w->clients[0].tag = 1;
	w->clients[0].visible = true;

	snprintf(w->clients[1].id, sizeof(w->clients[1].id), "0x2002");
	snprintf(w->clients[1].wm_class, sizeof(w->clients[1].wm_class), "firefox");
	snprintf(w->clients[1].title, sizeof(w->clients[1].title), "GitHub - Mozilla Firefox");
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

	static const struct { const char *phrase; const char *bin; } apps[] = {
		{"calculator", "gnome-calculator"},
		{"firefox web browser", "firefox"},
		{"pulseaudio volume control", "pavucontrol"},
		{"visual studio code", "code"},
		{"files", "thunar"},
		/* Custom domain entities: Music site & NeverDark */
		{"fado nocturnal poem", "poem:fado-nocturnal"},
		{"shadow goblin", "nd:mob:goblin"},
		{NULL, NULL}
	};
	for (size_t i = 0; apps[i].phrase && w->custom_entity_count < JARVIS_MAX_APPS; i++) {
		jarvis_live_entity_t *ce = &w->custom_entities[w->custom_entity_count++];
		snprintf(ce->type, sizeof(ce->type), "gui.application");
		snprintf(ce->id, sizeof(ce->id), "%s", apps[i].bin);
		snprintf(ce->phrase, sizeof(ce->phrase), "%s", apps[i].phrase);
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
		snprintf(
			res->output, sizeof(res->output),
			"{\"window\":{\"id\":\"0x1001\",\"class\":\"st-256color\",\"title\":\"quirinpa@host:~/site\"},"
			"\"cwd\":\"/home/quirinpa/site\","
			"\"tmux_panes\":[{\"pane\":\"main:0.0\",\"command\":\"make\",\"cwd\":\"/home/quirinpa/site\",\"active\":true,"
			"\"tail\":\"cc -Wall -Werror -c src/audio.c\\nsrc/audio.c:88:12: error: expected ';' after expression 'pitch_init()'\\nmake: *** [Makefile:42: src/audio.o] Error 1\"}]}"
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

