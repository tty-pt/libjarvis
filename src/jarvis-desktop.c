#include "../include/ttypt/jarvis-desktop.h"

#if !defined(__wasm__) && !defined(_WIN32)
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

static void trim_trailing(char *s)
{
	if (!s) return;
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
	                   s[len - 1] == ' ' || s[len - 1] == '\t')) {
		s[--len] = '\0';
	}
}

static bool str_contains_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle) return false;
	return strcasestr(haystack, needle) != NULL;
}

void jarvis_desktop_run_cmd_capture(const char *cmd, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!cmd || !*cmd) return;
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	size_t rd = fread(out, 1, out_sz - 1, fp);
	out[rd] = '\0';
	trim_trailing(out);
	pclose(fp);
}

pid_t jarvis_desktop_spawn_detached_sh(const char *cmd)
{
	if (!cmd || !*cmd) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	return pid;
}

pid_t jarvis_desktop_spawn_detached_argv(char *const argv[])
{
	if (!argv || !argv[0]) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	return pid;
}

void jarvis_desktop_url_encode(const char *src, char *dst, size_t dst_sz)
{
	static const char hex[] = "0123456789ABCDEF";
	if (!dst || dst_sz == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && w + 4 < dst_sz; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			dst[w++] = (char)*p;
		} else if (*p == ' ') {
			dst[w++] = '+';
		} else {
			dst[w++] = '%';
			dst[w++] = hex[*p >> 4];
			dst[w++] = hex[*p & 0x0F];
		}
	}
	dst[w] = '\0';
}

void jarvis_desktop_json_escape(const char *src, char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && w + 3 < dst_sz; p++) {
		if (*p == '"' || *p == '\\') {
			dst[w++] = '\\';
			dst[w++] = (char)*p;
		} else if (*p == '\n') {
			dst[w++] = '\\';
			dst[w++] = 'n';
		} else if (*p == '\r') {
			dst[w++] = '\\';
			dst[w++] = 'r';
		} else if (*p == '\t') {
			dst[w++] = '\\';
			dst[w++] = 't';
		} else if (*p >= 32) {
			dst[w++] = (char)*p;
		}
	}
	dst[w] = '\0';
}

size_t jarvis_desktop_get_child_pids(unsigned long pid, unsigned long *out_pids, size_t max_pids)
{
	if (!pid || !out_pids || max_pids == 0) return 0;
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/task/%lu/children", pid, pid);
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	char buf[1024];
	size_t count = 0;
	if (fgets(buf, sizeof(buf), fp)) {
		char *saveptr = NULL;
		for (char *tok = strtok_r(buf, " \t\n", &saveptr); tok && count < max_pids; tok = strtok_r(NULL, " \t\n", &saveptr)) {
			unsigned long cp = strtoul(tok, NULL, 10);
			if (cp > 0) out_pids[count++] = cp;
		}
	}
	fclose(fp);
	return count;
}

void jarvis_desktop_read_proc_comm(unsigned long pid, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!pid) return;
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/comm", pid);
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	if (fgets(out, (int)out_sz, fp)) {
		trim_trailing(out);
	}
	fclose(fp);
}

void jarvis_desktop_read_proc_cmdline(unsigned long pid, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!pid) return;
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/cmdline", pid);
	FILE *fp = fopen(path, "rb");
	if (!fp) return;
	size_t rd = fread(out, 1, out_sz - 1, fp);
	fclose(fp);
	for (size_t i = 0; i < rd; i++) {
		if (out[i] == '\0') out[i] = ' ';
	}
	out[rd] = '\0';
	trim_trailing(out);
}

bool jarvis_desktop_is_shell_name(const char *comm)
{
	if (!comm || !*comm) return false;
	const char *c = (comm[0] == '-') ? (comm + 1) : comm;
	return strcmp(c, "bash") == 0 || strcmp(c, "zsh") == 0 ||
	       strcmp(c, "sh") == 0 || strcmp(c, "fish") == 0 || strcmp(c, "dash") == 0;
}

void jarvis_desktop_resolve_pid_cwd_fg(unsigned long pid, char *out_cwd, size_t cwd_sz, char *out_fg, size_t fg_sz)
{
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";
	if (out_cwd && cwd_sz > 0) snprintf(out_cwd, cwd_sz, "%s", home);
	if (out_fg && fg_sz > 0) out_fg[0] = '\0';
	if (!pid) return;

	unsigned long sh_pids[16];
	size_t n_sh = jarvis_desktop_get_child_pids(pid, sh_pids, 16);
	unsigned long sh_pid = n_sh > 0 ? sh_pids[n_sh - 1] : pid;

	char cwd_link[128], resolved[512];
	snprintf(cwd_link, sizeof(cwd_link), "/proc/%lu/cwd", sh_pid);
	ssize_t rlen = readlink(cwd_link, resolved, sizeof(resolved) - 1);
	if (rlen > 0 && out_cwd && cwd_sz > 0) {
		resolved[rlen] = '\0';
		snprintf(out_cwd, cwd_sz, "%.*s", (int)(cwd_sz - 1), resolved);
	}

	if (out_fg && fg_sz > 0) {
		unsigned long sub_pids[16];
		size_t n_sub = jarvis_desktop_get_child_pids(sh_pid, sub_pids, 16);
		unsigned long target_p = n_sub > 0 ? sub_pids[n_sub - 1] : sh_pid;
		jarvis_desktop_read_proc_cmdline(target_p, out_fg, fg_sz);
		if (!out_fg[0]) {
			jarvis_desktop_read_proc_comm(target_p, out_fg, fg_sz);
		}
	}
}

bool jarvis_desktop_is_idle_shell_pid(unsigned long pid)
{
	if (!pid) return false;
	unsigned long sh_pids[16];
	size_t n_sh = jarvis_desktop_get_child_pids(pid, sh_pids, 16);
	if (n_sh == 0) return false;
	unsigned long sh_pid = sh_pids[n_sh - 1];

	unsigned long sub_pids[16];
	size_t n_sub = jarvis_desktop_get_child_pids(sh_pid, sub_pids, 16);
	if (n_sub == 0) return true;

	char tmux_out[1024];
	jarvis_desktop_run_cmd_capture(
		"tmux list-clients -F '#{client_pid}\t#{pane_current_command}\t#{pane_pid}' 2>/dev/null",
		tmux_out, sizeof(tmux_out));
	if (tmux_out[0]) {
		char *save_line = NULL;
		for (char *line = strtok_r(tmux_out, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
			char *t1 = strchr(line, '\t');
			char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
			if (!t1 || !t2) continue;
			*t1 = '\0';
			*t2 = '\0';
			unsigned long cpid = strtoul(line, NULL, 10);
			const char *pane_cmd = t1 + 1;
			unsigned long pane_pid = strtoul(t2 + 1, NULL, 10);
			for (size_t i = 0; i < n_sub; i++) {
				if (sub_pids[i] == cpid) {
					if (!jarvis_desktop_is_shell_name(pane_cmd)) return false;
					unsigned long pane_children[8];
					return jarvis_desktop_get_child_pids(pane_pid, pane_children, 8) == 0;
				}
			}
		}
	}
	return false;
}

bool jarvis_desktop_is_ssh_pid(unsigned long pid)
{
	if (!pid) return false;
	unsigned long sh_pids[16];
	size_t n_sh = jarvis_desktop_get_child_pids(pid, sh_pids, 16);
	if (n_sh == 0) return false;
	unsigned long sh_pid = sh_pids[n_sh - 1];

	unsigned long sub_pids[16];
	size_t n_sub = jarvis_desktop_get_child_pids(sh_pid, sub_pids, 16);
	for (size_t i = 0; i < n_sub; i++) {
		char comm[64];
		jarvis_desktop_read_proc_comm(sub_pids[i], comm, sizeof(comm));
		if (strcasecmp(comm, "ssh") == 0) return true;
	}
	return false;
}

void jarvis_desktop_populate_system_health(char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	int pos = 0;

	struct statvfs st;
	if (statvfs("/", &st) == 0 && st.f_blocks > 0) {
		double total_gb = ((double)st.f_blocks * (double)st.f_frsize) / (1024.0 * 1024.0 * 1024.0);
		double free_gb  = ((double)st.f_bavail * (double)st.f_frsize) / (1024.0 * 1024.0 * 1024.0);
		int used_pct = (int)(100.0 * (1.0 - (free_gb / (total_gb > 0.1 ? total_gb : 0.1))) + 0.5);
		pos += snprintf(out + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0,
		                "Disk /: %.1f GB free / %.0f GB (%d%% used)", free_gb, total_gb, used_pct);
	}

#ifdef __linux__
	FILE *mf = fopen("/proc/meminfo", "r");
	if (mf) {
		unsigned long mem_total_kb = 0, mem_avail_kb = 0;
		char line[128];
		while (fgets(line, sizeof(line), mf)) {
			if (strncmp(line, "MemTotal:", 9) == 0) mem_total_kb = strtoul(line + 9, NULL, 10);
			else if (strncmp(line, "MemAvailable:", 13) == 0) mem_avail_kb = strtoul(line + 13, NULL, 10);
			if (mem_total_kb && mem_avail_kb) break;
		}
		fclose(mf);
		if (mem_total_kb > 0) {
			double tot_g = (double)mem_total_kb / (1024.0 * 1024.0);
			double avail_g = (double)mem_avail_kb / (1024.0 * 1024.0);
			double used_g = tot_g - avail_g;
			pos += snprintf(out + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0,
			                "%sRAM: %.1f GB used / %.1f GB (%.1f GB free)",
			                pos > 0 ? " | " : "", used_g, tot_g, avail_g);
		}
	}

	FILE *lf = fopen("/proc/loadavg", "r");
	if (lf) {
		char a[32] = {0}, b[32] = {0}, c[32] = {0};
		if (fscanf(lf, "%31s %31s %31s", a, b, c) == 3) {
			pos += snprintf(out + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0,
			                "%sCPU Load: %s %s %s", pos > 0 ? " | " : "", a, b, c);
		}
		fclose(lf);
	}
#endif
}

void jarvis_desktop_populate_tmux_panes(jarvis_world_t *w, bool active_only, int lines)
{
	if (!w) return;
	char cmd[256], list_out[2048];
	snprintf(cmd, sizeof(cmd),
		"tmux list-panes -a -F '#{session_name}:#{window_index}.#{pane_index}\t#{pane_current_command}\t#{pane_current_path}\t#{pane_active}\t#{window_active}' 2>/dev/null");
	jarvis_desktop_run_cmd_capture(cmd, list_out, sizeof(list_out));
	if (!list_out[0]) return;

	char *saveptr = NULL;
	for (char *line = strtok_r(list_out, "\n", &saveptr);
	     line && w->pane_count < JARVIS_MAX_PANES;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t1 = strchr(line, '\t');
		char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
		char *t4 = t3 ? strchr(t3 + 1, '\t') : NULL;
		if (!t1 || !t2 || !t3 || !t4) continue;
		*t1 = *t2 = *t3 = *t4 = '\0';
		const char *pid_str = line;
		const char *p_cmd   = t1 + 1;
		const char *p_cwd   = t2 + 1;
		bool p_act = (t3[1] == '1');
		bool w_act = (t4[1] == '1');
		if (active_only && !(p_act && w_act)) continue;

		jarvis_pane_t *pn = &w->panes[w->pane_count++];
		memset(pn, 0, sizeof(*pn));
		snprintf(pn->pane, sizeof(pn->pane), "%s", pid_str);
		snprintf(pn->command, sizeof(pn->command), "%s", p_cmd);
		snprintf(pn->cwd, sizeof(pn->cwd), "%s", p_cwd);
		pn->active = (p_act && w_act);

		char cap_cmd[256];
		snprintf(cap_cmd, sizeof(cap_cmd), "tmux capture-pane -t '%s' -p -S -%d 2>/dev/null | tail -n %d",
		         pid_str, lines > 0 ? lines : 20, lines > 0 ? lines : 20);
		jarvis_desktop_run_cmd_capture(cap_cmd, pn->tail, sizeof(pn->tail));
	}
}

typedef struct {
	char phrase[128];
	char exec_cmd[256];
} desktop_app_entry_t;

#define MAX_CACHED_DESKTOP_APPS 256

static desktop_app_entry_t g_desktop_apps[MAX_CACHED_DESKTOP_APPS];
static size_t              g_desktop_app_count = 0;
static bool                g_desktop_apps_loaded = false;

static void scan_desktop_dir(const char *dir_path)
{
	DIR *d = opendir(dir_path);
	if (!d) return;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && g_desktop_app_count < MAX_CACHED_DESKTOP_APPS) {
		size_t nlen = strlen(ent->d_name);
		if (nlen <= 8 || strcmp(ent->d_name + nlen - 8, ".desktop") != 0) continue;

		char full[512];
		snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
		FILE *fp = fopen(full, "r");
		if (!fp) continue;

		bool in_entry = false;
		bool hidden = false;
		char name[128] = {0}, exec_str[256] = {0}, line[512];

		while (fgets(line, sizeof(line), fp)) {
			trim_trailing(line);
			if (line[0] == '[') {
				in_entry = (strcmp(line, "[Desktop Entry]") == 0);
				if (!in_entry && name[0]) break;
				continue;
			}
			if (!in_entry) continue;
			if (strncmp(line, "NoDisplay=true", 14) == 0 || strncmp(line, "Hidden=true", 11) == 0) {
				hidden = true;
				break;
			}
			if (!name[0] && strncmp(line, "Name=", 5) == 0) {
				snprintf(name, sizeof(name), "%.*s", (int)(sizeof(name) - 1), line + 5);
			} else if (!exec_str[0] && strncmp(line, "Exec=", 5) == 0) {
				snprintf(exec_str, sizeof(exec_str), "%.*s", (int)(sizeof(exec_str) - 1), line + 5);
			}
		}
		fclose(fp);
		if (hidden || !name[0] || !exec_str[0]) continue;

		char clean_exec[256];
		size_t w = 0;
		for (size_t i = 0; exec_str[i] && w + 1 < sizeof(clean_exec); i++) {
			if (exec_str[i] == '%' && exec_str[i + 1]) {
				i++;
				continue;
			}
			clean_exec[w++] = exec_str[i];
		}
		clean_exec[w] = '\0';
		trim_trailing(clean_exec);
		if (!clean_exec[0]) continue;

		desktop_app_entry_t *ae = &g_desktop_apps[g_desktop_app_count++];
		for (size_t i = 0; name[i] && i + 1 < sizeof(ae->phrase); i++) {
			ae->phrase[i] = (char)tolower((unsigned char)name[i]);
			ae->phrase[i + 1] = '\0';
		}
		snprintf(ae->exec_cmd, sizeof(ae->exec_cmd), "%s", clean_exec);
	}
	closedir(d);
}

static void ensure_desktop_apps_loaded(void)
{
	if (g_desktop_apps_loaded) return;
	g_desktop_apps_loaded = true;
	const char *home = getenv("HOME");
	if (home && *home) {
		char user_apps[512];
		snprintf(user_apps, sizeof(user_apps), "%s/.local/share/applications", home);
		scan_desktop_dir(user_apps);
	}
	scan_desktop_dir("/usr/share/applications");
	scan_desktop_dir("/var/lib/snapd/desktop/applications");
}

void jarvis_desktop_populate_apps(jarvis_world_t *w)
{
	if (!w) return;
	ensure_desktop_apps_loaded();
	for (size_t i = 0; i < g_desktop_app_count && w->custom_entity_count < JARVIS_MAX_APPS; i++) {
		bool dup = false;
		for (size_t j = 0; j < w->custom_entity_count; j++) {
			if (strcmp(w->custom_entities[j].type, "gui.application") == 0 &&
			    strcasecmp(w->custom_entities[j].phrase, g_desktop_apps[i].phrase) == 0) {
				dup = true;
				break;
			}
		}
		if (dup) continue;
		jarvis_live_entity_t *ce = &w->custom_entities[w->custom_entity_count++];
		snprintf(ce->type, sizeof(ce->type), "gui.application");
		snprintf(ce->id, sizeof(ce->id), "%.*s", (int)(sizeof(ce->id) - 1), g_desktop_apps[i].exec_cmd);
		snprintf(ce->phrase, sizeof(ce->phrase), "%.*s", (int)(sizeof(ce->phrase) - 1), g_desktop_apps[i].phrase);
	}
}

const char *jarvis_desktop_resolve_app_exec(const char *identifier)
{
	if (!identifier || !*identifier) return identifier;
	ensure_desktop_apps_loaded();
	for (size_t i = 0; i < g_desktop_app_count; i++) {
		if (strcasecmp(g_desktop_apps[i].phrase, identifier) == 0) {
			return g_desktop_apps[i].exec_cmd;
		}
	}
	for (size_t i = 0; i < g_desktop_app_count; i++) {
		if (str_contains_ci(g_desktop_apps[i].phrase, identifier)) {
			return g_desktop_apps[i].exec_cmd;
		}
	}
	return identifier;
}

bool jarvis_desktop_mpris_control(const char *action)
{
	if (!action || !*action) return false;
	char cmd[512], out[256];
	snprintf(cmd, sizeof(cmd),
		"for bus in $(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus "
		"--method org.freedesktop.DBus.ListNames 2>/dev/null | tr \"',[]\" ' ' | grep -o 'org\\.mpris\\.MediaPlayer2\\.[^ ]*'); do "
		"gdbus call --session --dest \"$bus\" --object-path /org/mpris/MediaPlayer2 "
		"--method org.mpris.MediaPlayer2.Player.%s 2>/dev/null && echo OK; done",
		action);
	jarvis_desktop_run_cmd_capture(cmd, out, sizeof(out));
	return strstr(out, "OK") != NULL;
}
#endif
