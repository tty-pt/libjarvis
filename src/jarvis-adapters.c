#include "../include/ttypt/jarvis.h"

#if !defined(__wasm__) && !defined(_WIN32)
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
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

static void populate_native_system_health(char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';

	long mem_total_kb = 0, mem_avail_kb = 0;
#ifdef __linux__
	FILE *fp = fopen("/proc/meminfo", "r");
	if (fp) {
		char line[128];
		while (fgets(line, sizeof(line), fp)) {
			if (strncmp(line, "MemTotal:", 9) == 0) mem_total_kb = atol(line + 9);
			else if (strncmp(line, "MemAvailable:", 13) == 0) mem_avail_kb = atol(line + 13);
			if (mem_total_kb > 0 && mem_avail_kb > 0) break;
		}
		fclose(fp);
	}
#endif

	double disk_used_gb = 0.0, disk_tot_gb = 0.0, disk_avail_gb = 0.0;
	struct statvfs vfs;
	if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
		disk_tot_gb = (double)(vfs.f_blocks * vfs.f_frsize) / (1024.0 * 1024.0 * 1024.0);
		disk_avail_gb = (double)(vfs.f_bavail * vfs.f_frsize) / (1024.0 * 1024.0 * 1024.0);
		disk_used_gb = disk_tot_gb - disk_avail_gb;
	}

	if (mem_total_kb > 0) {
		double tot_gib = (double)mem_total_kb / (1024.0 * 1024.0);
		double avail_gib = (double)mem_avail_kb / (1024.0 * 1024.0);
		double used_gib = tot_gib - avail_gib;
		snprintf(out, out_sz,
		         "RAM: %.1f GiB used / %.1f GiB total (%.1f GiB free) | Disk /: %.1f GiB used / %.1f GiB total (%.1f GiB free)",
		         used_gib, tot_gib, avail_gib, disk_used_gb, disk_tot_gb, disk_avail_gb);
	} else if (disk_tot_gb > 0.0) {
		snprintf(out, out_sz,
		         "Disk /: %.1f GiB used / %.1f GiB total (%.1f GiB free)",
		         disk_used_gb, disk_tot_gb, disk_avail_gb);
	}
}

/**
 * @brief Built-in desktop observation listener (populates common desktop apps
 *        and native system health across Linux and macOS).
 */
void jarvis_desktop_populate_defaults(jarvis_world_t *w)
{
	if (!w) return;
	(void)add_custom_app;
	if (!w->system_health[0]) {
		populate_native_system_health(w->system_health, sizeof(w->system_health));
	}
}

int jarvis_desktop_execute_fallback(const jarvis_dsl_t *dsl, jarvis_adapter_result_t *res)
{
	if (!dsl || !res) return -1;
	const char *op = dsl->op;

	if (strcmp(op, "inspect_system") == 0) {
		populate_native_system_health(res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	if (strcmp(op, "inspect_clipboard") == 0) {
#ifdef __APPLE__
		run_capture("pbpaste 2>/dev/null", res->output, sizeof(res->output));
#else
		run_capture("xclip -o -selection primary 2>/dev/null || xsel -op 2>/dev/null || wl-paste -p 2>/dev/null || wl-paste 2>/dev/null || xclip -o -selection clipboard 2>/dev/null || xsel -ob 2>/dev/null",
		            res->output, sizeof(res->output));
#endif
		res->handled = true;
		res->success = true;
		return 0;
	}
	if (strcmp(op, "inspect_terminal") == 0) {
		run_capture("tmux capture-pane -p -S -25 2>/dev/null | tail -n 20",
		            res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	if (strcmp(op, "run_background_command") == 0 && dsl->target.command[0]) {
		if (jarvis_is_destructive_command(dsl->target.command) &&
		    jarvis_xy_confirm(dsl->target.command) != 1) {
			res->handled = true;
			res->success = false;
			snprintf(res->output, sizeof(res->output),
			         "Blocked destructive command: %.200s", dsl->target.command);
			return 0;
		}
		run_capture(dsl->target.command, res->output, sizeof(res->output));
		res->handled = true;
		res->success = true;
		return 0;
	}
	return -1;
}
#endif

