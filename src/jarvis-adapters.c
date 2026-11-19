#include "../include/ttypt/jarvis-types.h"
#include <ttypt/xy.h>

#if !defined(__wasm__) && !defined(_WIN32)
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void run_capture(const char *cmd, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	size_t rd = fread(out, 1, out_sz - 1, fp);
	out[rd] = '\0';
	while (rd > 0 && (out[rd - 1] == '\n' || out[rd - 1] == '\r')) {
		out[--rd] = '\0';
	}
	pclose(fp);
}

static void add_custom_app(jarvis_world_t *w, const char *phrase, const char *bin)
{
	if (w->custom_entity_count >= JARVIS_MAX_APPS) return;
	jarvis_live_entity_t *e = &w->custom_entities[w->custom_entity_count++];
	snprintf(e->type, sizeof(e->type), "gui.application");
	snprintf(e->id, sizeof(e->id), "%s", bin);
	snprintf(e->phrase, sizeof(e->phrase), "%s", phrase);
}

/**
 * @brief Built-in desktop observation listener (populates common desktop apps,
 *        system health, and dwm/sway/hyprland state if available).
 */
void jarvis_desktop_populate_defaults(jarvis_world_t *w)
{
	if (!w) return;
	add_custom_app(w, "calculator", "gnome-calculator");
	add_custom_app(w, "firefox web browser", "firefox");
	add_custom_app(w, "pulseaudio volume control", "pavucontrol");
	add_custom_app(w, "visual studio code", "code");
	add_custom_app(w, "files", "thunar");
}

int jarvis_desktop_execute_fallback(const jarvis_dsl_t *dsl, jarvis_adapter_result_t *res)
{
	if (!dsl || !res) return -1;
	const char *op = dsl->op;

	if (strcmp(op, "inspect_system") == 0) {
		run_capture("free -h | awk '/^Mem:/ {printf \"Mem: %s total, %s used, %s free\", $2, $3, $4}'",
		            res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	if (strcmp(op, "inspect_clipboard") == 0) {
		run_capture("xclip -o -selection primary 2>/dev/null || xclip -o -selection clipboard 2>/dev/null || wl-paste 2>/dev/null",
		            res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	if (strcmp(op, "inspect_terminal") == 0) {
		run_capture("tmux capture-pane -p -S -20 2>/dev/null | tail -n 15",
		            res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	if (strcmp(op, "run_background_command") == 0 && dsl->target.command[0]) {
		run_capture(dsl->target.command, res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	return -1;
}
#endif
