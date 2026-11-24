#define _GNU_SOURCE
#include "../include/ttypt/jarvis.h"
#include "../include/ttypt/desktop-core.h"
#include "jarvis-audio.h"
#include "jarvis-platform.h"
#include "jarvis-ren-ipc.h"

#include <ttypt/stele.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define getpid _getpid
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] <utterance | subcommand>\n\n"
		"Subcommands:\n"
		"  query <utterance>                         Send natural language command to ren\n"
		"  start | on                                Start jarvisd daemon and enable wake-word listener\n"
		"  stop | off                                Disable wake-word listener\n"
		"  restart                                   Restart jarvisd daemon to apply config changes\n"
		"  toggle                                    Toggle wake-word listener on/off\n"
		"  listen                                    Capture a single voice command now (no wake word) -> ren\n"
		"  status                                    Print listener status ('on' or 'off')\n"
		"  doctor                                    Check audio, Whisper, GPU, TTS, ren, and daemon health\n"
		"  setup                                     Install missing defaults (Whisper model, PipeWire AEC)\n"
		"  lockdown | lock                           Disable listener and lock screen\n"
		"  menu                                      Open interactive voice & daemon settings menu\n"
		"  ren                                       Open/focus the tmux 'jarvis' ren terminal session\n"
		"  config [key [value] | unset <key>]        Get, set, or unset ~/.jarvisrc configuration values\n"
		"  tts-on [voice]                            Enable neural TTS (optionally setting voice or 'auto')\n"
		"  tts-off                                   Disable neural TTS (0 KB idle RAM)\n"
		"  tts-interrupt | quiet                     Interrupt active TTS speech immediately\n"
		"  speak <text>                              Speak text via TTS\n"
		"  notify <text>                             Announce background completion via OSD + non-preemptive TTS\n"
		"  dmenu | omni                              Open interactive command bar (dmenu) -> ren\n"
		"  warmup                                    Pre-warm ontology and ren tmux session\n"
		"  clear-learned                             Reset ren session & clear legacy caches\n\n"
		"Options:\n"
		"  -p, --port <port>                         Override jarvisd TCP port (default from ~/.jarvisrc)\n"
		"  -c, --cmd <raw_cmd>                       Send raw control command to jarvisd socket\n"
		"  --wm <auto|dwm|sway|hyprland|x11|macos|win32> Window manager hint for ren session startup\n"
		"  --surface <any|terminal|browser|site|nd>  Accepted for CLI compatibility\n"
		"  --mod <name>                              Extra ren module name to load on session start\n"
		"  --dry-run                                 Preview query dispatch or setup actions without changes\n"
		"  --json                                    Emit JSON output\n"
		"  -q, --quiet                               Suppress desktop notifications\n",
		prog
	);
}

static const char *get_cli_home(void)
{
	static char cached[512];
	const char *h = getenv("HOME");
	if (h && *h) return h;
	stl_home_dir(cached, sizeof(cached));
	return cached[0] ? cached : ".";
}

static void resolve_rc_file(char *out, size_t out_sz)
{
	const char *env_rc = getenv("JARVISRC");
	if (env_rc && *env_rc) {
		snprintf(out, out_sz, "%s", env_rc);
		return;
	}
	const char *home = get_cli_home();
	snprintf(out, out_sz, "%s/.jarvisrc", home);
	if (!jarvis_plat_is_readable(out)) {
		char alt[512] = {0};
		if (stl_config_dir("jarvis", alt, sizeof(alt)) && alt[0]) {
			char cand[512];
			snprintf(cand, sizeof(cand), "%s/jarvisrc", alt);
			if (jarvis_plat_is_readable(cand)) {
				snprintf(out, out_sz, "%s", cand);
			}
		}
	}
}

static int run_argv_sync(char *const argv[])
{
	if (!argv || !argv[0]) return -1;
	stl_proc_t proc;
	if (stl_spawn((const char *const *)argv, 0, &proc) != 0)
		return -1;
	int ec = -1;
	stl_proc_wait(&proc, &ec, -1);
	stl_proc_close(&proc);
	return ec == 0 ? 0 : -1;
}

static void cli_notify(const char *title, const char *body)
{
	if (!title || !*title) return;
	jarvis_plat_notify_osd(title, body ? body : "", 3000);
}

#ifndef _WIN32
static void write_all_fd(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t w = write(fd, buf, len);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) break;
		buf += w;
		len -= (size_t)w;
	}
}

static int run_menu_program(
	char *const argv[],
	const char *items,
	char       *out_sel,
	size_t      out_sz
)
{
	if (!argv || !argv[0] || !out_sel || out_sz == 0) return -1;
	out_sel[0] = '\0';

	int in_p[2], out_p[2];
	if (pipe(in_p) < 0) return -1;
	if (pipe(out_p) < 0) {
		close(in_p[0]);
		close(in_p[1]);
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(in_p[0]); close(in_p[1]);
		close(out_p[0]); close(out_p[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(in_p[0], STDIN_FILENO);
		dup2(out_p[1], STDOUT_FILENO);
		close(in_p[0]); close(in_p[1]);
		close(out_p[0]); close(out_p[1]);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(in_p[0]);
	close(out_p[1]);

	if (items && *items) {
		write_all_fd(in_p[1], items, strlen(items));
		size_t ilen = strlen(items);
		if (items[ilen - 1] != '\n') write_all_fd(in_p[1], "\n", 1);
	}
	close(in_p[1]);

	size_t total = 0;
	while (total + 1 < out_sz) {
		ssize_t r = read(out_p[0], out_sel + total, out_sz - 1 - total);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		total += (size_t)r;
	}
	close(out_p[0]);
	out_sel[total] = '\0';

	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		out_sel[0] = '\0';
		return (WIFEXITED(st) && WEXITSTATUS(st) == 127) ? -127 : 0;
	}
	desktop_core_trim_trailing(out_sel);
	return (int)strlen(out_sel);
}
#endif

static int cli_dmenu(const char *prompt, const char *items, char *out_sel, size_t out_sz)
{
	const char *p = (prompt && *prompt) ? prompt : "Jarvis:";
	if (!out_sel || out_sz == 0) return -1;
	out_sel[0] = '\0';
#if defined(_WIN32)
	(void)items;
	fprintf(stderr, "%s ", p);
	if (!fgets(out_sel, (int)out_sz, stdin)) return 0;
	desktop_core_trim_trailing(out_sel);
	return (int)strlen(out_sel);
#elif defined(__APPLE__)
	if (!items || !*items) return 0;
	size_t ilen = strlen(items);
	size_t plen = strlen(p);
	size_t ci_cap = ilen * 2 + 64 * 4 + 16;
	char *clean_items = (char *)malloc(ci_cap);
	char *esc_prompt = (char *)malloc(plen * 2 + 1);
	if (!clean_items || !esc_prompt) {
		free(clean_items);
		free(esc_prompt);
		return -1;
	}

	size_t ci = 0;
	size_t n_it = 0;
	const char *line = items;
	while (*line && n_it < 64 && ci + 4 < ci_cap) {
		const char *nl = strchr(line, '\n');
		size_t len = nl ? (size_t)(nl - line) : strlen(line);
		if (len > 0) {
			if (n_it > 0 && ci + 2 < ci_cap) {
				clean_items[ci++] = ',';
				clean_items[ci++] = ' ';
			}
			clean_items[ci++] = '"';
			for (size_t k = 0; k < len && ci + 4 < ci_cap; k++) {
				if (line[k] == '"' || line[k] == '\\') clean_items[ci++] = '\\';
				clean_items[ci++] = line[k];
			}
			clean_items[ci++] = '"';
			n_it++;
		}
		if (!nl) break;
		line = nl + 1;
	}
	clean_items[ci] = '\0';

	int rc = 0;
	if (clean_items[0]) {
		size_t ep = 0;
		for (size_t k = 0; k < plen; k++) {
			if (p[k] == '"' || p[k] == '\\') esc_prompt[ep++] = '\\';
			esc_prompt[ep++] = p[k];
		}
		esc_prompt[ep] = '\0';

		size_t sc_cap = ci + ep + 64;
		char *script = (char *)malloc(sc_cap);
		if (!script) {
			free(clean_items);
			free(esc_prompt);
			return -1;
		}
		snprintf(script, sc_cap,
		         "choose from list {%s} with prompt \"%s\"", clean_items, esc_prompt);
		char *const osa[] = { "osascript", "-e", script, NULL };
		rc = run_menu_program(osa, NULL, out_sel, out_sz);
		if (strcmp(out_sel, "false") == 0) out_sel[0] = '\0';
		free(script);
	}
	free(clean_items);
	free(esc_prompt);
	return rc;
#else
	char fn[128], nb[32], nf[32], sb[32], sf[32], lines_buf[16];
	jarvis_rc_get("dmenu_font", "Ubuntu Sans:style=Medium:size=12:antialias=true:autohint=true", fn, sizeof(fn));
	jarvis_rc_get("dmenu_nb", "#11111b", nb, sizeof(nb));
	jarvis_rc_get("dmenu_nf", "#a6adc8", nf, sizeof(nf));
	jarvis_rc_get("dmenu_sb", "#7c6ff2", sb, sizeof(sb));
	jarvis_rc_get("dmenu_sf", "#ffffff", sf, sizeof(sf));

	int max_lines = jarvis_rc_get_int("dmenu_lines", 12);
	if (max_lines < 4) max_lines = 4;
	if (max_lines > 24) max_lines = 24;
	int nlines = 0;
	if (items && *items) {
		nlines = 1;
		for (const char *c = items; *c; c++) {
			if (*c == '\n' && c[1] != '\0') nlines++;
		}
	}
	int lines = (nlines > 0 && nlines < max_lines) ? nlines : max_lines;
	if (lines < 4) lines = 4;
	snprintf(lines_buf, sizeof(lines_buf), "%d", lines);

	char *const dmenu_argv[] = {
		"dmenu", "-i", "-l", lines_buf,
		"-fn", fn, "-nb", nb, "-nf", nf, "-sb", sb, "-sf", sf,
		"-p", (char *)p, NULL
	};
	int rc = run_menu_program(dmenu_argv, items, out_sel, out_sz);
	if (rc == -127 && getenv("WAYLAND_DISPLAY")) {
		char *const bemenu_argv[] = { "bemenu", "-i", "-l", lines_buf, "-p", (char *)p, NULL };
		rc = run_menu_program(bemenu_argv, items, out_sel, out_sz);
		if (rc == -127) {
			char *const wofi_argv[] = { "wofi", "--dmenu", "-i", "-p", (char *)p, NULL };
			rc = run_menu_program(wofi_argv, items, out_sel, out_sz);
		}
	}
	return rc;
#endif
}

static void notify_status_bar(void)
{
#ifndef _WIN32
	char path[256] = {0};
	if (jarvis_rc_get("status_notify_pid_file", NULL, path, sizeof(path)) && path[0]) {
		FILE *fp = fopen(path, "r");
		if (fp) {
			int pid = 0;
			if (fscanf(fp, "%d", &pid) == 1 && pid > 1) {
#ifdef SIGUSR1
				kill((pid_t)pid, SIGUSR1);
#endif
			}
			fclose(fp);
			return;
		}
	}
#endif
	jarvis_plat_notify_bar();
}

static int g_cli_port_override = 0;

static bool daemon_cmd_timeout(const char *cmd, int timeout_sec, char *out_resp, size_t resp_sz)
{
	if (out_resp && resp_sz > 0) out_resp[0] = '\0';
	int port = (g_cli_port_override > 0) ? g_cli_port_override : jarvis_rc_get_int("port", 8179);
	stl_sock_init();
	stl_socket_t s = stl_sock_create(AF_INET, SOCK_STREAM, 0);
	if (s == STL_INVALID_SOCKET) return false;

	int tms = (timeout_sec > 0 ? timeout_sec : 2) * 1000;
	stl_sock_set_timeout(s, tms, tms);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (stl_sock_connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		stl_sock_close(s);
		return false;
	}
	char req[1024];
	int n = snprintf(req, sizeof(req), "%s\n", cmd);
	if (n > 0) {
		jarvis_plat_write_sock_all(s, req, (size_t)n);
	}
	if (out_resp && resp_sz > 0) {
		size_t total = 0;
		while (total + 1 < resp_sz) {
			ssize_t rd = stl_sock_recv(s, out_resp + total, resp_sz - 1 - total, 0);
			if (rd <= 0) break;
			total += (size_t)rd;
			if (memchr(out_resp, '\n', total) != NULL) break;
		}
		out_resp[total] = '\0';
		size_t l = total;
		while (l > 0 && isspace((unsigned char)out_resp[l - 1])) out_resp[--l] = '\0';
	}
	stl_sock_close(s);
	return true;
}

static bool daemon_cmd(const char *cmd, char *out_resp, size_t resp_sz)
{
	return daemon_cmd_timeout(cmd, 2, out_resp, resp_sz);
}

static bool is_daemon_listening(void)
{
	char resp[64];
	if (daemon_cmd("JARVIS_STATUS", resp, sizeof(resp)) && strcmp(resp, "ON") == 0) {
		return true;
	}
	return false;
}

static void ensure_daemon_running(bool activate_listener)
{
	char resp[64];
	if (daemon_cmd("PING", resp, sizeof(resp))) {
		daemon_cmd(activate_listener ? "JARVIS_ON" : "JARVIS_OFF", resp, sizeof(resp));
		return;
	}

	const char *home = get_cli_home();
	char bin_path[512];
#ifdef _WIN32
	char exedir[512] = {0};
	stl_exe_dir(exedir, sizeof(exedir));
	snprintf(bin_path, sizeof(bin_path), "%s\\jarvisd.exe", exedir[0] ? exedir : ".");
	if (!jarvis_plat_is_readable(bin_path)) {
		snprintf(bin_path, sizeof(bin_path), "jarvisd.exe");
	}
	(void)home;
#else
	snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/jarvisd", home);
	if (!jarvis_plat_is_exec(bin_path)) {
		snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/dwm-whisperd", home);
		if (!jarvis_plat_is_exec(bin_path)) {
			snprintf(bin_path, sizeof(bin_path), "jarvisd");
		}
	}
#endif

	bool started_svc = false;
#ifdef __linux__
	char *const start_svc[] = { "systemctl", "--user", "start", "jarvisd.service", NULL };
	started_svc = (run_argv_sync(start_svc) == 0);
#endif
	if (!started_svc) {
		if (activate_listener) {
			const char *fb[] = { bin_path, "-j", NULL };
			stl_spawn_detached(fb);
		} else {
			const char *fb[] = { bin_path, NULL };
			stl_spawn_detached(fb);
		}
	}

	int max_polls = started_svc ? 120 : 60;
	for (int i = 0; i < max_polls; i++) {
		if (daemon_cmd("PING", resp, sizeof(resp))) {
			if (started_svc) {
				daemon_cmd(activate_listener ? "JARVIS_ON" : "JARVIS_OFF", resp, sizeof(resp));
			}
			break;
		}
		stl_sleep_ms(100);
	}
}

static void restart_daemon(bool was_active)
{
#ifdef _WIN32
	char *const tk[] = { "taskkill", "/F", "/IM", "jarvisd.exe", NULL };
	run_argv_sync(tk);
#else
#ifdef __linux__
	char *const stop_svc[] = { "systemctl", "--user", "stop", "jarvisd.service", NULL };
	run_argv_sync(stop_svc);
#endif
	char *const pk1[] = { "pkill", "-x", "jarvisd", NULL };
	run_argv_sync(pk1);
	char *const pk2[] = { "pkill", "-x", "dwm-whisperd", NULL };
	run_argv_sync(pk2);
#endif
	stl_sleep_ms(120);
	ensure_daemon_running(was_active);
	notify_status_bar();
}

static stl_socket_t connect_ren_socket(bool auto_start, const char *wm_override, const char *extra_mod)
{
	stl_socket_t fd = jarvis_ren_connect(0);
	if (fd != STL_INVALID_SOCKET)
		return fd;
	if (!auto_start)
		return STL_INVALID_SOCKET;

	char rc_mods[384] = {0}, mods[512] = {0};
	jarvis_rc_get("ren_mods", NULL, rc_mods, sizeof(rc_mods));
	jarvis_ren_resolve_mods(wm_override, rc_mods[0] ? rc_mods : NULL, mods, sizeof(mods));
	if (extra_mod && *extra_mod) {
		size_t cur = strlen(mods);
		if (cur + strlen(extra_mod) + 2 < sizeof(mods)) {
			snprintf(mods + cur, sizeof(mods) - cur, "%s%s", cur > 0 ? ":" : "", extra_mod);
		}
	}
	return jarvis_ren_ensure_session(NULL, mods, wm_override);
}

static bool send_ren_control(const char *cmd)
{
	stl_socket_t fd = connect_ren_socket(false, NULL, NULL);
	if (fd == STL_INVALID_SOCKET) return false;
	stl_sock_set_timeout(fd, 2000, 2000);

	char msg[512];
	int n = snprintf(msg, sizeof(msg), "%s\n", cmd);
	if (n > 0) jarvis_plat_write_sock_all(fd, msg, (size_t)n);

	char buf[256];
	ssize_t r = stl_sock_recv(fd, buf, sizeof(buf) - 1, 0);
	(void)r;
	stl_sock_close(fd);
	return true;
}

static int dispatch_ren_query(
	const char *utterance,
	bool        as_json,
	bool        quiet,
	const char *wm_override,
	const char *extra_mod
)
{
	stl_socket_t fd = connect_ren_socket(true, wm_override, extra_mod);
	if (fd == STL_INVALID_SOCKET) {
		if (as_json) {
			char eu[512];
			desktop_core_json_escape(utterance, eu, sizeof(eu));
			printf("{\"ok\":false,\"utterance\":\"%s\",\"error\":\"ren socket unavailable\"}\n", eu);
		} else {
			fprintf(stderr, "Error: failed to connect to ren (%s)\n", utterance);
		}
		return 1;
	}

	char req[1024];
	int n = snprintf(req, sizeof(req), "%s\n", utterance);
	if (n > 0) jarvis_plat_write_sock_all(fd, req, (size_t)n);

	bool ok = true;
	bool any_event = false;
	bool printed_clause = false;
	char source[64] = "ren";
	char reply[4096] = {0};
	char err_msg[512] = {0};
	size_t rpos = 0;

	jarvis_ren_reader_t rr;
	jarvis_ren_reader_init(&rr);
	char line[4096];
	while (jarvis_ren_read_line(fd, &rr, line, sizeof(line)) > 0) {
		if (line[0] == '\0') continue;

		char *tab1 = strchr(line, '\t');
		if (!tab1) continue;
		*tab1 = '\0';
		const char *ev = line;
		char *arg1 = tab1 + 1;
		char *tab2 = strchr(arg1, '\t');
		const char *arg2 = "";
		if (tab2) {
			*tab2 = '\0';
			arg2 = tab2 + 1;
		}

		any_event = true;
		if (strcmp(ev, "TIER0") == 0) {
			snprintf(source, sizeof(source), "tier0:%s", arg1[0] ? arg1 : "action");
			if (arg2[0]) {
				snprintf(reply, sizeof(reply), "%s", arg2);
				rpos = strlen(reply);
			}
			if (!as_json && arg2[0]) {
				printf("[OK] %s: %s\n", source, arg2);
				printed_clause = true;
			}
		} else if (strcmp(ev, "TOOL") == 0) {
			snprintf(source, sizeof(source), "ren:%s", arg1[0] ? arg1 : "tool");
			if (!as_json && !quiet) {
				fprintf(stderr, "[TOOL] %s: %s\n", arg1, arg2);
			}
		} else if (strcmp(ev, "CLAUSE") == 0) {
			if (arg1[0]) {
				if (rpos > 0 && rpos + 2 < sizeof(reply)) {
					reply[rpos++] = ' ';
				}
				rpos += (size_t)snprintf(reply + rpos,
				                         rpos < sizeof(reply) ? sizeof(reply) - rpos : 0,
				                         "%s", arg1);
				if (!as_json) {
					printf("%s%s", printed_clause ? " " : "", arg1);
					printed_clause = true;
				}
			}
		} else if (strcmp(ev, "DONE") == 0) {
			ok = (strcmp(arg1, "1") == 0);
			if (!ok && arg2[0]) {
				snprintf(err_msg, sizeof(err_msg), "%s", arg2);
			}
			break;
		}
	}
	stl_sock_close(fd);

	if (!any_event) ok = false;

	if (as_json) {
		char eu[512], es[128], er[2048], ee[512];
		desktop_core_json_escape(utterance, eu, sizeof(eu));
		desktop_core_json_escape(source, es, sizeof(es));
		desktop_core_json_escape(reply, er, sizeof(er));
		desktop_core_json_escape(err_msg[0] ? err_msg : (any_event ? "request failed" : "ren disconnected"),
		                         ee, sizeof(ee));
		printf("{\"ok\":%s,\"utterance\":\"%s\",\"source\":\"%s\",\"reply\":\"%s\",\"error\":\"%s\"}\n",
		       ok ? "true" : "false", eu, es, er, ok ? "" : ee);
	} else {
		if (printed_clause && reply[0]) {
			if (strncmp(source, "tier0:", 6) != 0) printf("\n");
		} else if (!ok) {
			fprintf(stderr, "Error: %s\n", err_msg[0] ? err_msg : (any_event ? "request failed" : "ren disconnected"));
		}
		if (!quiet && !ok) {
			cli_notify("Jarvis — Error", err_msg[0] ? err_msg : utterance);
		}
	}
	return ok ? 0 : 1;
}

static jarvis_ontology_t *get_cli_ont(void)
{
	static int loaded = 0;
	static jarvis_ontology_t *s_ont = NULL;
	if (!loaded) {
		loaded = 1;
		char ont_path[512] = {0};
		const char *req = jarvis_rc_get("ontology_path", NULL, ont_path, sizeof(ont_path))
			? ont_path : NULL;
		s_ont = jarvis_ontology_create(req);
	}
	return s_ont;
}

static void get_cli_wake_word(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	jarvis_resolve_wake_word(ont, out, out_sz);
}

static void get_cli_default_lang(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	jarvis_resolve_lang(ont, out, out_sz);
}

static void get_cli_default_voice(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	jarvis_resolve_tts_voice(ont, out, out_sz);
}

static void do_jarvis_start(bool quiet)
{
	jarvis_audio_unmute_mic();
	ensure_daemon_running(true);
	notify_status_bar();
	if (!quiet) {
		const jarvis_ontology_t *ont = get_cli_ont();
		char wake[64], title[128], body[256];
		get_cli_wake_word(ont, wake, sizeof(wake));
		jarvis_ontology_format_ui(ont, "active_title", wake, "  {Wake} Active", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "active_body", wake, "Say “{wake}” anytime, or “{wake}, lockdown” to stop.", body, sizeof(body));
		cli_notify(title, body);
	}
}

static void do_jarvis_stop(bool quiet)
{
	char resp[64];
	daemon_cmd("JARVIS_OFF", resp, sizeof(resp));
	notify_status_bar();
	if (!quiet) {
		const jarvis_ontology_t *ont = get_cli_ont();
		char wake[64], title[128], body[256];
		get_cli_wake_word(ont, wake, sizeof(wake));
		jarvis_ontology_format_ui(ont, "disabled_title", wake, "  {Wake} Disabled", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "mic_disabled_body", wake, "Microphone listening disabled.", body, sizeof(body));
		cli_notify(title, body);
	}
}

static void write_voice_state(const char *state)
{
	char path[256], tmp[272];
	if (jarvis_plat_state_path("jarvis-voice", ".state", path, sizeof(path)) != 0)
		return;
	if (!state || !*state) {
		remove(path);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *fp = fopen(tmp, "w");
	if (fp) {
		fprintf(fp, "%s\n", state);
		fclose(fp);
#ifdef _WIN32
		remove(path);
#endif
		rename(tmp, path);
	}
}

static int do_jarvis_listen(bool as_json, bool quiet, const char *wm_override, const char *extra_mod)
{
	char pid_path[256];
	jarvis_plat_state_path("jarvis-listen", ".pid", pid_path, sizeof(pid_path));

	FILE *pfp = fopen(pid_path, "r");
	if (pfp) {
		int old_pid = 0;
		if (fscanf(pfp, "%d", &old_pid) == 1 && old_pid > 1 && stl_pid_alive((pid_t)old_pid)) {
			fclose(pfp);
			char cresp[64];
			daemon_cmd("CANCEL", cresp, sizeof(cresp));
			remove(pid_path);
			write_voice_state(is_daemon_listening() ? "jarvis-idle" : NULL);
			notify_status_bar();
			if (as_json) printf("{\"ok\":false,\"cancelled\":true}\n");
			return 0;
		}
		fclose(pfp);
	}

	pfp = fopen(pid_path, "w");
	if (pfp) {
		fprintf(pfp, "%d\n", (int)getpid());
		fclose(pfp);
	}

	jarvis_audio_unmute_mic();

	bool was_listening = is_daemon_listening();
	ensure_daemon_running(was_listening);

	char resp[4300];
	daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));

	const jarvis_ontology_t *ont = get_cli_ont();
	char lang[32], wake[64], state_path[256], stream_cmd[512];
	get_cli_default_lang(ont, lang, sizeof(lang));
	get_cli_wake_word(ont, wake, sizeof(wake));
	jarvis_plat_state_path("jarvis-voice", ".state", state_path, sizeof(state_path));

	int silence_ms = jarvis_rc_get_int("cmd_silence_ms", 750);
	int wait_ms = jarvis_rc_get_int("listen_wait_ms", 6000);
	if (silence_ms < 400) silence_ms = 400;
	if (wait_ms < 1000) wait_ms = 1000;

	write_voice_state("jarvis-cmd");
	notify_status_bar();

	char chime_wav[256];
	jarvis_plat_state_path("jarvis-chime", ".wav", chime_wav, sizeof(chime_wav));
	if (!jarvis_plat_is_readable(chime_wav)) {
		jarvis_voice_write_earcon_wav(chime_wav, false);
	}
	if (jarvis_plat_is_readable(chime_wav)) {
		jarvis_audio_play_chime_async(chime_wav);
	}

	if (!quiet) {
		char ltitle[128], lbody[256];
		jarvis_ontology_format_ui(ont, "listening_title", wake, "  {Wake} Listening...", ltitle, sizeof(ltitle));
		jarvis_ontology_format_ui(ont, "listening_body", wake, "Speak your command now.", lbody, sizeof(lbody));
		cli_notify(ltitle, lbody);
	}

	snprintf(stream_cmd, sizeof(stream_cmd), "STREAM|cmd|%s|-36|%d|%d|%s",
	         lang[0] ? lang : "auto", silence_ms, wait_ms, state_path);

	bool ok = daemon_cmd_timeout(stream_cmd, 25, resp, sizeof(resp));
	if (ok && resp[0] == '\0' && was_listening) {
		char cancel_resp[64];
		daemon_cmd("CANCEL", cancel_resp, sizeof(cancel_resp));
		stl_sleep_ms(80);
		ok = daemon_cmd_timeout(stream_cmd, 25, resp, sizeof(resp));
	}

	remove(pid_path);

	if (ok && strncmp(resp, "OK|", 3) == 0) {
		char *transcript = resp + 3;
		desktop_core_trim_trailing(transcript);
		while (*transcript && isspace((unsigned char)*transcript)) transcript++;
		if (*transcript) {
			write_voice_state("processing");
			notify_status_bar();
			if (!quiet && !as_json) {
				char hdr[128];
				jarvis_ontology_format_ui(ont, "notify_title", wake, "  {Wake}", hdr, sizeof(hdr));
				cli_notify(hdr, transcript);
			}
			int rc = dispatch_ren_query(transcript, as_json, quiet, wm_override, extra_mod);
			write_voice_state(was_listening ? "jarvis-idle" : NULL);
			notify_status_bar();
			return rc;
		}
	}

	write_voice_state(was_listening ? "jarvis-idle" : NULL);
	notify_status_bar();

	const char *reason = (!ok) ? "jarvisd unavailable" : (resp[0] ? resp : "EMPTY");
	if (as_json) {
		char er[128];
		desktop_core_json_escape(reason, er, sizeof(er));
		printf("{\"ok\":false,\"error\":\"%s\"}\n", er);
	} else if (!quiet && strcmp(reason, "CANCELLED") != 0) {
		char ntitle[128];
		jarvis_ontology_format_ui(ont, "notify_title", wake, "  {Wake}", ntitle, sizeof(ntitle));
		cli_notify(ntitle, !ok ? "Voice daemon unavailable." : "No speech detected.");
	}
	return 1;
}

static void do_jarvis_lockdown(bool quiet)
{
	do_jarvis_stop(true);
	if (!quiet) {
		const jarvis_ontology_t *ont = get_cli_ont();
		char wake[64], title[128], body[256];
		get_cli_wake_word(ont, wake, sizeof(wake));
		jarvis_ontology_format_ui(ont, "lockdown_short_title", wake, "  Lockdown", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "lockdown_short_body", wake, "Listening disabled and locking screen...", body, sizeof(body));
		cli_notify(title, body);
	}
	char lock_cmd[256] = {0};
	jarvis_rc_get("lock_cmd", "", lock_cmd, sizeof(lock_cmd));
	stl_sleep_ms(150);
	jarvis_plat_lock_screen(lock_cmd[0] ? lock_cmd : NULL);
}

static void do_clear_learned(bool quiet)
{
	const char *home = get_cli_home();
	char p[512];

	if (jarvis_plat_state_path("jarvis", ".context.json", p, sizeof(p)) == 0)
		remove(p);
	if (home && *home) {
		snprintf(p, sizeof(p), "%s/.cache/jarvis/session.json", home);
		remove(p);
		snprintf(p, sizeof(p), "%s/.cache/jarvis/embeddings.json", home);
		remove(p);
		snprintf(p, sizeof(p), "%s/.ren/sessions/jarvis", home);
		remove(p);
	}
	send_ren_control("/reset");

	if (!quiet) {
		const jarvis_ontology_t *ont = get_cli_ont();
		char wake[64], ctitle[128], cbody[256];
		get_cli_wake_word(ont, wake, sizeof(wake));
		jarvis_ontology_format_ui(ont, "cache_cleared_title", wake, "  Cache Cleared", ctitle, sizeof(ctitle));
		jarvis_ontology_format_ui(ont, "cache_cleared_body", wake, "Session and context caches cleared.", cbody, sizeof(cbody));
		cli_notify(ctitle, cbody);
	}
	printf("Cleared caches\n");
}

static void build_prompt_candidates(const char *fs_path, char *out, size_t out_sz)
{
	out[0] = '\0';
	if (fs_path && *fs_path) {
		FILE *fp = fopen(fs_path, "rb");
		if (fp) {
			fseek(fp, 0, SEEK_END);
			long sz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (sz > 0 && sz <= 256 * 1024) {
				char *buf = (char *)malloc((size_t)sz + 1);
				if (buf) {
					size_t rd = fread(buf, 1, (size_t)sz, fp);
					buf[rd] = '\0';
					size_t pos = 0, count = 0;
					const char *p = buf;
					while ((p = strstr(p, "\"utterance\"")) != NULL && count < 16) {
						p = strchr(p + 11, '"');
						if (!p) break;
						p++;
						const char *u_end = strchr(p, '"');
						if (!u_end) break;
						size_t u_len = (size_t)(u_end - p);
						const char *op_p = strstr(u_end, "\"op\"");
						if (!op_p) break;
						op_p = strchr(op_p + 4, '"');
						if (!op_p) break;
						op_p++;
						const char *op_end = strchr(op_p, '"');
						if (!op_end) break;
						size_t op_len = (size_t)(op_end - op_p);
						if (u_len > 0 && u_len < 120 && op_len > 0 && op_len < 64) {
							pos += (size_t)snprintf(out + pos, pos < out_sz ? out_sz - pos : 0,
							                        "%s%-38.*s ▸ %.*s",
							                        count ? "\n" : "",
							                        (int)u_len, p,
							                        (int)op_len, op_p);
							count++;
						}
						p = op_end + 1;
					}
					free(buf);
				}
			}
			fclose(fp);
		}
	}
	if (!out[0]) {
		snprintf(out, out_sz,
			"open a terminal\n"
			"switch to workspace 2\n"
			"play lofi chill beats\n"
			"pause the music\n"
			"show memory and disk usage\n"
			"open firefox\n");
	}
}

static int do_dmenu(bool as_json, bool quiet, const char *wm_override, const char *extra_mod)
{
	const jarvis_ontology_t *ont = get_cli_ont();
	char wake[64], dprompt[128], fs_path[512] = {0};
	const char *home = get_cli_home();
	if (home && *home) {
		char def_fs[512];
		snprintf(def_fs, sizeof(def_fs), "%s/.config/jarvis/few_shots.json", home);
		jarvis_rc_get("few_shot_path", def_fs, fs_path, sizeof(fs_path));
	}
	get_cli_wake_word(ont, wake, sizeof(wake));
	jarvis_ontology_format_ui(ont, "dmenu_prompt", wake, " {Wake}:", dprompt, sizeof(dprompt));

	char candidates[4096];
	build_prompt_candidates(fs_path, candidates, sizeof(candidates));

	char selection[512] = {0};
	if (cli_dmenu(dprompt, candidates, selection, sizeof(selection)) <= 0 || !selection[0]) {
		return 0;
	}
	char *arrow = strstr(selection, "▸");
	if (arrow) *arrow = '\0';
	desktop_core_trim_trailing(selection);
	if (!selection[0]) return 0;

	return dispatch_ren_query(selection, as_json, quiet, wm_override, extra_mod);
}

static void open_ren_terminal(const char *wm_override, const char *extra_mod)
{
	stl_socket_t fd = connect_ren_socket(true, wm_override, extra_mod);
	if (fd != STL_INVALID_SOCKET) stl_sock_close(fd);

#if defined(__APPLE__)
	const char *argv[] = {
		"osascript", "-e",
		"tell application \"Terminal\" to do script \"tmux attach -t jarvis || ren --interactive\"\ntell application \"Terminal\" to activate",
		NULL
	};
	stl_spawn_detached(argv);
#elif defined(_WIN32)
	const char *argv_wt[] = { "wt.exe", "ren", "--interactive", NULL };
	if (stl_spawn_detached(argv_wt) != 0) {
		const char *argv_cmd[] = { "cmd.exe", "/c", "start", "Jarvis (ren)", "ren", "--interactive", NULL };
		stl_spawn_detached(argv_cmd);
	}
#else
	char *const search[] = { "xdotool", "search", "--class", "jarvis-ren", NULL };
	if (run_argv_sync(search) == 0) {
		char *const pull[] = { "dwm-msg", "pull-class", "jarvis-ren", NULL };
		if (run_argv_sync(pull) == 0) return;
	}

	char *const term_argv[] = {
		"st", "-c", "jarvis-ren", "-n", "jarvis-ren",
		"-t", "Jarvis (ren tmux session)",
		"-e", "tmux", "attach", "-t", "jarvis", NULL
	};
	desktop_core_spawn_detached_argv(term_argv);
#endif
}

static void do_jarvis_menu(const char *wm_override, const char *extra_mod)
{
	const jarvis_ontology_t *ont = get_cli_ont();
	bool running = is_daemon_listening();
	int tts_on = jarvis_rc_get_int("tts_enabled", 1);
	char voice[64], lang[32], model_path[512], wake[64];
	get_cli_default_voice(ont, voice, sizeof(voice));
	get_cli_default_lang(ont, lang, sizeof(lang));
	if (!jarvis_rc_get("whisper_model", NULL, model_path, sizeof(model_path)) || !model_path[0]) {
		const char *home = get_cli_home();
		snprintf(model_path, sizeof(model_path), "%s/.local/share/whisper/ggml-large-v3-turbo-q5_0.bin",
		         home ? home : "");
		if (!jarvis_plat_is_readable(model_path)) {
			if (jarvis_plat_data_file("whisper", "ggml-large-v3-turbo-q5_0.bin", model_path, sizeof(model_path)) != 0 ||
			    !jarvis_plat_is_readable(model_path)) {
				snprintf(model_path, sizeof(model_path), "ggml-large-v3-turbo-q5_0.bin");
			}
		}
	}
	get_cli_wake_word(ont, wake, sizeof(wake));
	int fms = jarvis_rc_get_int("followup_ms", 8000);

	const char *model_base = strrchr(model_path, '/');
	const char *model_bs = strrchr(model_path, '\\');
	if (model_bs && (!model_base || model_bs > model_base)) model_base = model_bs;
	model_base = model_base ? (model_base + 1) : model_path;

	char st_on[64], st_off[64], fw_always[96], fw_conv[64], fw_strict[64];
	char lbl_wake[64], lbl_voice[64];
	char it_toggle[128], it_tts[128], it_fw[128], it_lock[128], it_lang[128];
	char it_model[128], it_dmenu[128], it_clear[128], it_ren[128], it_rc[128];
	jarvis_ontology_format_ui(ont, "menu_state_on", wake, "ON", st_on, sizeof(st_on));
	jarvis_ontology_format_ui(ont, "menu_state_off", wake, "OFF", st_off, sizeof(st_off));
	jarvis_ontology_format_ui(ont, "menu_followup_always", wake, "Always-On", fw_always, sizeof(fw_always));
	jarvis_ontology_format_ui(ont, "menu_followup_conv", wake, "Conversational", fw_conv, sizeof(fw_conv));
	jarvis_ontology_format_ui(ont, "menu_followup_strict", wake, "Strict", fw_strict, sizeof(fw_strict));
	jarvis_ontology_format_ui(ont, "menu_wake_label", wake, "wake", lbl_wake, sizeof(lbl_wake));
	jarvis_ontology_format_ui(ont, "menu_voice_label", wake, "voice", lbl_voice, sizeof(lbl_voice));
	jarvis_ontology_format_ui(ont, "menu_item_toggle", wake, "1. Toggle Listening", it_toggle, sizeof(it_toggle));
	jarvis_ontology_format_ui(ont, "menu_item_tts", wake, "2. Neural TTS Voice", it_tts, sizeof(it_tts));
	jarvis_ontology_format_ui(ont, "menu_item_followup", wake, "3. Follow-up Window", it_fw, sizeof(it_fw));
	jarvis_ontology_format_ui(ont, "menu_item_lock", wake, "4. Lockdown (Disable Listening + Lock Screen)", it_lock, sizeof(it_lock));
	jarvis_ontology_format_ui(ont, "menu_item_lang", wake, "5. Whisper / TTS Language", it_lang, sizeof(it_lang));
	jarvis_ontology_format_ui(ont, "menu_item_model", wake, "6. Whisper Model", it_model, sizeof(it_model));
	jarvis_ontology_format_ui(ont, "menu_item_dmenu", wake, "7. Semantic Command Bar", it_dmenu, sizeof(it_dmenu));
	jarvis_ontology_format_ui(ont, "menu_item_clear", wake, "8. Clear Learned Shortcuts & Cache", it_clear, sizeof(it_clear));
	jarvis_ontology_format_ui(ont, "menu_item_ren", wake, "9. Open ren Terminal (jarvis)", it_ren, sizeof(it_ren));
	jarvis_ontology_format_ui(ont, "menu_item_rc", wake, "10. Edit ~/.jarvisrc", it_rc, sizeof(it_rc));

	char items[4096];
	snprintf(items, sizeof(items),
		"%s (%s — %s: “%s”)\n"
		"%s: %s (%s: %s)\n"
		"%s: %s (%d ms)\n"
		"%s\n"
		"%s: %s\n"
		"%s: %s\n"
		"%s\n"
		"%s\n"
		"%s\n"
		"%s",
		it_toggle, running ? st_on : st_off, lbl_wake, wake,
		it_tts, tts_on ? st_on : st_off, lbl_voice, voice,
		it_fw, fms >= 3600000 ? fw_always : (fms > 0 ? fw_conv : fw_strict), fms,
		it_lock,
		it_lang, lang,
		it_model, model_base,
		it_dmenu,
		it_clear,
		it_ren,
		it_rc);

	char mprompt[128], choice[512] = {0};
	jarvis_ontology_format_ui(ont, "menu_prompt", wake, " {Wake}:", mprompt, sizeof(mprompt));
	if (cli_dmenu(mprompt, items, choice, sizeof(choice)) <= 0 || !choice[0]) {
		return;
	}

	if (strncmp(choice, "1.", 2) == 0) {
		if (running) do_jarvis_stop(false);
		else do_jarvis_start(false);
	} else if (strncmp(choice, "2.", 2) == 0) {
		char v_auto[128], v_off[128], vprompt[128], vitems[2048];
		jarvis_ontology_format_ui(ont, "menu_voice_auto", wake, "auto                         (Automatic neural voice for active language)", v_auto, sizeof(v_auto));
		jarvis_ontology_format_ui(ont, "menu_voice_off", wake, "off                          (Disable Neural TTS Voice)", v_off, sizeof(v_off));
		jarvis_ontology_format_ui(ont, "voice_prompt", wake, "🗣 TTS Voice:", vprompt, sizeof(vprompt));
		snprintf(vitems, sizeof(vitems),
			"%s\n"
			"edge:pt-PT-RaquelNeural      (Português de Portugal — Neural, Raquel)\n"
			"edge:pt-PT-DuarteNeural      (Português de Portugal — Neural, Duarte)\n"
			"pm_tuga                      (Português de Portugal — Male, Tuga)\n"
			"pm_alex                      (Português — Male, Alex)\n"
			"pm_santa                     (Português — Male, Santa)\n"
			"pf_dora                      (Português — Female, Dora)\n"
			"bm_daniel                    (English — British Male, Daniel)\n"
			"bm_george                    (English — British Male, George)\n"
			"bm_lewis                     (English — British Male, Lewis)\n"
			"bf_emma                      (English — British Female, Emma)\n"
			"am_michael                   (English — American Male, Michael)\n"
			"af_heart                     (English — American Female, Heart)\n"
			"%s", v_auto, v_off);
		char vchoice[256] = {0};
		if (cli_dmenu(vprompt, vitems, vchoice, sizeof(vchoice)) > 0 && vchoice[0]) {
			char vid[64] = {0};
			if (sscanf(vchoice, "%63s", vid) == 1 && vid[0]) {
				if (strcmp(vid, "off") == 0) {
					jarvis_rc_set("tts_enabled", "0");
					char resp[64], ttitle[128], tbody[256];
					daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));
					jarvis_ontology_format_ui(ont, "tts_off_title", wake, "🗣  Neural Voice Disabled", ttitle, sizeof(ttitle));
					jarvis_ontology_format_ui(ont, "tts_off_body", wake, "Silent mode active.", tbody, sizeof(tbody));
					cli_notify(ttitle, tbody);
				} else {
					jarvis_rc_set("tts_enabled", "1");
					jarvis_rc_set("tts_voice", vid);
					char resolved_v[64] = {0};
					if (strcmp(vid, "auto") == 0) {
						jarvis_rc_unset("tts_lang");
						jarvis_resolve_tts_voice(ont, resolved_v, sizeof(resolved_v));
					} else {
						const char *tlang = (strncmp(vid, "edge:pt", 7) == 0 || vid[0] == 'p') ? "pt-pt"
							: ((strncmp(vid, "edge:en-US", 10) == 0 || vid[0] == 'a') ? "en-us" : "en-gb-x-rp");
						jarvis_rc_set("tts_lang", tlang);
						snprintf(resolved_v, sizeof(resolved_v), "%s", vid);
					}
					char ttitle[128], greet[128], cmd_sp[192], resp[64];
					jarvis_ontology_format_ui(ont, "tts_on_title", wake, "🗣  Neural Voice Active", ttitle, sizeof(ttitle));
					cli_notify(ttitle, resolved_v[0] ? resolved_v : vid);
					jarvis_ontology_format_ui(ont, "tts_greeting", wake, "Ready.", greet, sizeof(greet));
					snprintf(cmd_sp, sizeof(cmd_sp), "TTS_SPEAK|%s", greet);
					daemon_cmd(cmd_sp, resp, sizeof(resp));
				}
			}
		}
	} else if (strncmp(choice, "3.", 2) == 0) {
		char f8[160], f15[160], fc[160], fs[160], fprompt[128], fitems[768];
		jarvis_ontology_format_ui(ont, "menu_followup_8s", wake, "8000     (8s Follow-up Window — no wake word needed)", f8, sizeof(f8));
		jarvis_ontology_format_ui(ont, "menu_followup_15s", wake, "15000    (15s Extended Follow-up Window)", f15, sizeof(f15));
		jarvis_ontology_format_ui(ont, "menu_followup_cont", wake, "3600000  (Continuous Mode — speak directly without wake word)", fc, sizeof(fc));
		jarvis_ontology_format_ui(ont, "menu_followup_strict_opt", wake, "0        (Strict Mode — always require wake word)", fs, sizeof(fs));
		jarvis_ontology_format_ui(ont, "followup_prompt", wake, "🎙 Follow-up:", fprompt, sizeof(fprompt));
		snprintf(fitems, sizeof(fitems), "%s\n%s\n%s\n%s", f8, f15, fc, fs);
		char fchoice[256] = {0};
		if (cli_dmenu(fprompt, fitems, fchoice, sizeof(fchoice)) > 0 && fchoice[0]) {
			char fval[32] = {0};
			if (sscanf(fchoice, "%31s", fval) == 1 && fval[0]) {
				char ftitle[128];
				jarvis_rc_set("followup_ms", fval);
				jarvis_ontology_format_ui(ont, "followup_title", wake, "🎙  Follow-up Window", ftitle, sizeof(ftitle));
				cli_notify(ftitle, fval);
			}
		}
	} else if (strncmp(choice, "4.", 2) == 0) {
		do_jarvis_lockdown(false);
	} else if (strncmp(choice, "5.", 2) == 0) {
		char l_kbd[128], l_auto[128], lprompt[128], lang_items[512];
		jarvis_ontology_format_ui(ont, "menu_lang_kbd", wake, "kbd   (Follow keyboard layout automatically)", l_kbd, sizeof(l_kbd));
		jarvis_ontology_format_ui(ont, "menu_lang_auto", wake, "auto  (Whisper auto-detect)", l_auto, sizeof(l_auto));
		jarvis_ontology_format_ui(ont, "lang_prompt", wake, "🌐 Language:", lprompt, sizeof(lprompt));
		snprintf(lang_items, sizeof(lang_items),
			"en    (English)\n"
			"pt-pt (Português Europeu)\n"
			"es    (Español)\n"
			"fr    (Français)\n"
			"de    (Deutsch)\n"
			"%s\n%s", l_kbd, l_auto);
		char gchoice[256] = {0};
		if (cli_dmenu(lprompt, lang_items, gchoice, sizeof(gchoice)) > 0 && gchoice[0]) {
			char gval[32] = {0};
			if (sscanf(gchoice, "%31s", gval) == 1 && gval[0]) {
				char ltitle[128], resp[64];
				jarvis_rc_set("lang", gval);
				jarvis_rc_unset("tts_lang");
				if (daemon_cmd("PING", resp, sizeof(resp))) {
					restart_daemon(running);
				}
				jarvis_ontology_t *new_ont = jarvis_ontology_create(NULL);
				jarvis_ontology_format_ui(new_ont ? new_ont : ont, "lang_updated_title", wake, "🌐  Language Updated", ltitle, sizeof(ltitle));
				cli_notify(ltitle, gval);
				if (new_ont) jarvis_ontology_destroy(new_ont);
			}
		}
	} else if (strncmp(choice, "6.", 2) == 0) {
		char m0[128], m1[128], m2[128], m3[128], m4[128], wprompt[128], mitems[768];
		jarvis_ontology_format_ui(ont, "menu_model_large_turbo", wake, "ggml-large-v3-turbo-q5_0.bin (548 MB - Large v3 Turbo Multilingual Q5)", m0, sizeof(m0));
		jarvis_ontology_format_ui(ont, "menu_model_base_q8", wake, "ggml-base-q8_0.bin           (78 MB  - Fast Multilingual Q8)", m1, sizeof(m1));
		jarvis_ontology_format_ui(ont, "menu_model_small_q8", wake, "ggml-small-q8_0.bin          (252 MB - High-Accuracy Multilingual Q8)", m2, sizeof(m2));
		jarvis_ontology_format_ui(ont, "menu_model_small_q5", wake, "ggml-small-q5_1.bin          (182 MB - Multilingual Q5)", m3, sizeof(m3));
		jarvis_ontology_format_ui(ont, "menu_model_en_q8", wake, "ggml-base.en-q8_0.bin        (78 MB  - English-Only Fast Q8)", m4, sizeof(m4));
		jarvis_ontology_format_ui(ont, "whisper_prompt", wake, "🧠 Whisper Model:", wprompt, sizeof(wprompt));
		snprintf(mitems, sizeof(mitems), "%s\n%s\n%s\n%s\n%s", m0, m1, m2, m3, m4);
		char mchoice[256] = {0};
		if (cli_dmenu(wprompt, mitems, mchoice, sizeof(mchoice)) > 0 && mchoice[0]) {
			char mfile[128] = {0};
			if (sscanf(mchoice, "%127s", mfile) == 1 && mfile[0]) {
				const char *home = get_cli_home();
				char full_m[512], wtitle[128];
				snprintf(full_m, sizeof(full_m), "%s/.local/share/whisper/%s", home ? home : "", mfile);
				jarvis_rc_set("whisper_model", full_m);
				restart_daemon(running);
				jarvis_ontology_format_ui(ont, "whisper_updated_title", wake, "🧠  Whisper Model Active", wtitle, sizeof(wtitle));
				cli_notify(wtitle, mfile);
			}
		}
	} else if (strncmp(choice, "7.", 2) == 0) {
		do_dmenu(false, false, wm_override, extra_mod);
	} else if (strncmp(choice, "8.", 2) == 0) {
		do_clear_learned(false);
	} else if (strncmp(choice, "9.", 2) == 0) {
		open_ren_terminal(wm_override, extra_mod);
	} else if (strncmp(choice, "10.", 3) == 0) {
		char rc_path[512];
		resolve_rc_file(rc_path, sizeof(rc_path));
#if defined(__APPLE__)
		const char *argv[] = { "open", "-t", rc_path, NULL };
		stl_spawn_detached(argv);
#elif defined(_WIN32)
		const char *argv[] = { "notepad.exe", rc_path, NULL };
		stl_spawn_detached(argv);
#else
		const char *ed = getenv("EDITOR");
		if (!ed || !*ed) ed = "nvim";
		char *const argv[] = { "st", "-e", (char *)ed, rc_path, NULL };
		desktop_core_spawn_detached_argv(argv);
#endif
	}
}

static bool cmd_in_path(const char *cmd)
{
	if (!cmd || !*cmd) return false;
	char exedir[512] = {0};
	if (stl_exe_dir(exedir, sizeof(exedir)) && exedir[0]) {
		char ep[768];
#ifdef _WIN32
		snprintf(ep, sizeof(ep), "%s\\%s.exe", exedir, cmd);
		if (jarvis_plat_is_exec(ep)) return true;
		snprintf(ep, sizeof(ep), "%s\\%s", exedir, cmd);
		if (jarvis_plat_is_exec(ep)) return true;
#else
		snprintf(ep, sizeof(ep), "%s/%s", exedir, cmd);
		if (jarvis_plat_is_exec(ep)) return true;
#endif
	}
	const char *home = get_cli_home();
	if (home && *home) {
		char lp[768];
		snprintf(lp, sizeof(lp), "%s/.local/bin/%s", home, cmd);
		if (jarvis_plat_is_exec(lp)) return true;
#ifdef _WIN32
		snprintf(lp, sizeof(lp), "%s\\.local\\bin\\%s.exe", home, cmd);
		if (jarvis_plat_is_exec(lp)) return true;
#endif
	}
	return jarvis_plat_find_in_path(cmd, NULL, 0) == 0;
}

static bool probe_http_health(uint16_t port, const char *req_path)
{
	stl_sock_init();
	stl_socket_t s = stl_sock_create(AF_INET, SOCK_STREAM, 0);
	if (s == STL_INVALID_SOCKET) return false;
	stl_sock_set_timeout(s, 1000, 1000);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (stl_sock_connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		stl_sock_close(s);
		return false;
	}
	char req[256];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", req_path);
	if (n > 0) jarvis_plat_write_sock_all(s, req, (size_t)n);
	char buf[512] = {0};
	ssize_t rd = stl_sock_recv(s, buf, sizeof(buf) - 1, 0);
	stl_sock_close(s);
	return rd > 0 && strstr(buf, "200") != NULL;
}

static bool find_file_with_ext(const char *dir, const char *ext, char *out, size_t out_sz)
{
	if (!dir || !*dir || !ext || !*ext) return false;
	size_t elen = strlen(ext);
#ifdef _WIN32
	char pat[768];
	WIN32_FIND_DATAA fd;
	snprintf(pat, sizeof(pat), "%s\\*%s", dir, ext);
	HANDLE h = FindFirstFileA(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	bool found = false;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		size_t nl = strlen(fd.cFileName);
		if (nl > elen && _stricmp(fd.cFileName + nl - elen, ext) == 0) {
			if (out && out_sz > 0)
				snprintf(out, out_sz, "%s/%s", dir, fd.cFileName);
			found = true;
			break;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return found;
#else
	DIR *d = opendir(dir);
	if (!d) return false;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		size_t nl = strlen(de->d_name);
		if (nl > elen && strcmp(de->d_name + nl - elen, ext) == 0) {
			if (out && out_sz > 0)
				snprintf(out, out_sz, "%s/%s", dir, de->d_name);
			closedir(d);
			return true;
		}
	}
	closedir(d);
	return false;
#endif
}

static bool find_any_whisper_model(const char *home, bool allow_en_only, char *out, size_t out_sz)
{
	out[0] = '\0';
	if (jarvis_rc_get("whisper_model", NULL, out, out_sz) && out[0] && jarvis_plat_is_readable(out)) {
		return true;
	}
	out[0] = '\0';
	static const char *const multi[] = {
		"ggml-large-v3-turbo-q5_0.bin", "ggml-large-v3-turbo-q8_0.bin",
		"ggml-large-v3-turbo.bin",      "ggml-large-v3-q5_0.bin",
		"ggml-large-v3.bin",            "ggml-small-q8_0.bin",
		"ggml-small-q5_1.bin",          "ggml-base-q8_0.bin",
		"ggml-base-q5_1.bin",           "ggml-base.bin", NULL
	};
	static const char *const en[] = {
		"ggml-base.en-q8_0.bin", "ggml-small.en-q5_1.bin", NULL
	};
	char ddir[512] = {0};
	stl_data_dir("whisper", ddir, sizeof(ddir));
	for (size_t i = 0; multi[i]; i++) {
		char cand[768];
		snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, multi[i]);
		if (jarvis_plat_is_readable(cand)) {
			snprintf(out, out_sz, "%s", cand);
			return true;
		}
		if (ddir[0]) {
			snprintf(cand, sizeof(cand), "%s/%s", ddir, multi[i]);
			if (jarvis_plat_is_readable(cand)) {
				snprintf(out, out_sz, "%s", cand);
				return true;
			}
		}
	}
	if (allow_en_only) {
		for (size_t i = 0; en[i]; i++) {
			char cand[768];
			snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, en[i]);
			if (jarvis_plat_is_readable(cand)) {
				snprintf(out, out_sz, "%s", cand);
				return true;
			}
			if (ddir[0]) {
				snprintf(cand, sizeof(cand), "%s/%s", ddir, en[i]);
				if (jarvis_plat_is_readable(cand)) {
					snprintf(out, out_sz, "%s", cand);
					return true;
				}
			}
		}
	}
	char wdir[512];
	snprintf(wdir, sizeof(wdir), "%s/.local/share/whisper", home);
	if (find_file_with_ext(wdir, ".bin", out, out_sz)) return true;
	if (ddir[0] && find_file_with_ext(ddir, ".bin", out, out_sz)) return true;
	return false;
}

static bool has_piper_or_kokoro_model(const char *home)
{
	char kokoro[768];
	snprintf(kokoro, sizeof(kokoro), "%s/.local/share/kokoro/kokoro-v1.0.onnx", home);
	if (jarvis_plat_is_readable(kokoro)) return true;
	if (jarvis_plat_data_file("kokoro", "kokoro-v1.0.onnx", kokoro, sizeof(kokoro)) == 0 &&
	    jarvis_plat_is_readable(kokoro)) {
		return true;
	}
	char pdir[768];
	snprintf(pdir, sizeof(pdir), "%s/.local/share/piper", home);
	if (find_file_with_ext(pdir, ".onnx", NULL, 0)) return true;
	if (stl_data_dir("piper", pdir, sizeof(pdir)) && pdir[0] &&
	    find_file_with_ext(pdir, ".onnx", NULL, 0)) {
		return true;
	}
	return false;
}

#ifdef __linux__
static bool copy_file_atomic(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb");
	if (!in) return false;
	char tmp[1056];
	snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
	FILE *out = fopen(tmp, "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	char buf[4096];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	}
	if (ferror(in)) ok = false;
	fclose(in);
	if (fclose(out) != 0) ok = false;
	if (!ok) {
		remove(tmp);
		return false;
	}
	return rename(tmp, dst) == 0;
}
#endif

static int do_jarvis_doctor(bool as_json)
{
	const char *home = get_cli_home();

	const jarvis_ontology_t *ont = get_cli_ont();
	char rc_path[512];
	resolve_rc_file(rc_path, sizeof(rc_path));
	bool has_rc = (jarvis_plat_is_readable(rc_path) != 0);

	char lang[32] = {0}, wake[64] = {0}, voice[64] = {0}, wm[64] = {0};
	jarvis_resolve_lang(ont, lang, sizeof(lang));
	jarvis_resolve_wake_word(ont, wake, sizeof(wake));
	jarvis_resolve_tts_voice(ont, voice, sizeof(voice));
	jarvis_resolve_wm(wm, sizeof(wm));

	const char *audio_backend = jarvis_audio_backend_name();
	bool has_capture_audio = jarvis_audio_has_capture_backend();
	bool has_pw_rec = false, has_pw_cat = false, has_wpctl = false;
	bool has_aec_conf = false, has_aec_node = false;

#ifdef __linux__
	char aec_conf[768] = {0};
	has_pw_rec = cmd_in_path("pw-record");
	has_pw_cat = cmd_in_path("pw-cat");
	has_wpctl  = cmd_in_path("wpctl");

	snprintf(aec_conf, sizeof(aec_conf), "%s/.config/pipewire/pipewire.conf.d/99-jarvis-aec.conf", home);
	has_aec_conf = (jarvis_plat_is_readable(aec_conf) != 0);

	if (has_wpctl) {
		char wp_out[8192];
		char *const wp_argv[] = { "wpctl", "status", NULL };
		if (desktop_core_exec_capture(wp_argv, wp_out, sizeof(wp_out)) == 0) {
			has_aec_node = (strstr(wp_out, "jarvis_aec_source") != NULL);
		}
	}
#endif

	bool allow_en = (strncasecmp(lang, "en", 2) == 0);
	char model_path[1024] = {0};
	bool has_model = find_any_whisper_model(home, allow_en, model_path, sizeof(model_path));
	bool en_model_mismatch = false;
	if (has_model && !allow_en &&
	    (strstr(model_path, ".en.") != NULL || strstr(model_path, ".en-") != NULL)) {
		en_model_mismatch = true;
	}
#ifdef __APPLE__
	bool has_vulkan = true; /* Apple Metal / CoreML backend */
#elif defined(_WIN32)
	HMODULE vk_dll = LoadLibraryA("vulkan-1.dll");
	bool has_vulkan = (vk_dll != NULL);
	if (vk_dll) FreeLibrary(vk_dll);
#else
	bool has_vulkan = (jarvis_plat_is_readable("/dev/dri/renderD128") != 0);
#endif

	bool has_edge_pcm = cmd_in_path("edge-tts-pcm");
	bool has_ffmpeg   = cmd_in_path("ffmpeg");
	bool has_espeak   = cmd_in_path("espeak-ng");
#if defined(__APPLE__) || defined(_WIN32)
	bool has_native_tts = true; /* macOS 'say' / Windows SAPI5 always available */
#else
	bool has_native_tts = false;
#endif
	bool has_edge_py  = false;
	{
		char venv1[768], venv2[768];
#ifdef _WIN32
		snprintf(venv1, sizeof(venv1), "%s/.local/share/dwm-voice/venv/Scripts/python.exe", home);
		snprintf(venv2, sizeof(venv2), "%s/.venv/Scripts/python.exe", home);
#else
		snprintf(venv1, sizeof(venv1), "%s/.local/share/dwm-voice/venv/bin/python", home);
		snprintf(venv2, sizeof(venv2), "%s/.venv/bin/python", home);
#endif
		const char *py_cands[5] = {
			cmd_in_path("python3") ? "python3" : NULL,
			cmd_in_path("python")  ? "python"  : NULL,
#ifdef _WIN32
			cmd_in_path("py")      ? "py"      : NULL,
#else
			NULL,
#endif
			jarvis_plat_is_exec(venv1) ? venv1 : NULL,
			jarvis_plat_is_exec(venv2) ? venv2 : NULL
		};
		for (int i = 0; i < 5 && !has_edge_py; i++) {
			if (!py_cands[i]) continue;
			char *const py_argv[] = { (char *)py_cands[i], "-c", "import edge_tts", NULL };
			has_edge_py = (run_argv_sync(py_argv) == 0);
		}
	}
	bool has_local_onnx = has_piper_or_kokoro_model(home);

	bool has_ren = cmd_in_path("ren");
	const char *so_ext = stl_so_ext();
	char ren_wm_so[768], ren_jarvis_so[768];
	snprintf(ren_wm_so, sizeof(ren_wm_so), "%s/.local/lib/ren/ren-%s.%s", home, wm, so_ext);
	snprintf(ren_jarvis_so, sizeof(ren_jarvis_so), "%s/.local/lib/ren/ren-jarvis.%s", home, so_ext);
	bool has_ren_wm = (jarvis_plat_is_readable(ren_wm_so) != 0);
	bool has_ren_jarvis = (jarvis_plat_is_readable(ren_jarvis_so) != 0);

	bool auren_ok = probe_http_health(4242, "/v1/models");
	char ping_resp[64] = {0}, st_resp[64] = {0};
	bool daemon_up = daemon_cmd("PING", ping_resp, sizeof(ping_resp));
	bool daemon_listening = daemon_up && daemon_cmd("JARVIS_STATUS", st_resp, sizeof(st_resp)) &&
	                        (strcmp(st_resp, "ON") == 0);

	bool healthy = (ont != NULL) && has_capture_audio && has_model &&
	               !en_model_mismatch &&
	               (has_edge_pcm || has_local_onnx || has_espeak || has_native_tts) &&
	               has_ren;

	if (as_json) {
		char e_rc[640], e_lang[64], e_wake[128], e_voice[128], e_wm[64], e_mod[1024], e_ab[64];
		desktop_core_json_escape(has_rc ? rc_path : "none (zero-config defaults)", e_rc, sizeof(e_rc));
		desktop_core_json_escape(lang, e_lang, sizeof(e_lang));
		desktop_core_json_escape(wake, e_wake, sizeof(e_wake));
		desktop_core_json_escape(voice, e_voice, sizeof(e_voice));
		desktop_core_json_escape(wm, e_wm, sizeof(e_wm));
		desktop_core_json_escape(has_model ? model_path : "", e_mod, sizeof(e_mod));
		desktop_core_json_escape(audio_backend, e_ab, sizeof(e_ab));
		printf("{\"healthy\":%s,"
		       "\"config\":{\"rc_file\":\"%s\",\"has_rc\":%s,\"lang\":\"%s\",\"wake_word\":\"%s\",\"tts_voice\":\"%s\",\"wm\":\"%s\",\"ontology_loaded\":%s},"
		       "\"audio\":{\"backend\":\"%s\",\"capture_ready\":%s,\"pw_record\":%s,\"pw_cat\":%s,\"wpctl\":%s,\"aec_conf\":%s,\"aec_source_active\":%s},"
		       "\"whisper\":{\"model\":\"%s\",\"model_found\":%s,\"en_model_mismatch\":%s,\"vulkan_render_node\":%s},"
		       "\"tts\":{\"edge_tts_pcm\":%s,\"edge_tts_python\":%s,\"ffmpeg\":%s,\"local_onnx\":%s,\"espeak_ng\":%s,\"native_tts\":%s},"
		       "\"ren\":{\"ren_bin\":%s,\"ren_jarvis_so\":%s,\"ren_wm_so\":%s,\"axil_auren_health\":%s},"
		       "\"daemon\":{\"running\":%s,\"listening\":%s}}\n",
		       healthy ? "true" : "false",
		       e_rc, has_rc ? "true" : "false", e_lang, e_wake, e_voice, e_wm, ont ? "true" : "false",
		       e_ab, has_capture_audio ? "true" : "false",
		       has_pw_rec ? "true" : "false", has_pw_cat ? "true" : "false",
		       has_wpctl ? "true" : "false", has_aec_conf ? "true" : "false",
		       has_aec_node ? "true" : "false",
		       e_mod, has_model ? "true" : "false", en_model_mismatch ? "true" : "false",
		       has_vulkan ? "true" : "false",
		       has_edge_pcm ? "true" : "false", has_edge_py ? "true" : "false",
		       has_ffmpeg ? "true" : "false", has_local_onnx ? "true" : "false",
		       has_espeak ? "true" : "false", has_native_tts ? "true" : "false",
		       has_ren ? "true" : "false", has_ren_jarvis ? "true" : "false",
		       has_ren_wm ? "true" : "false", auren_ok ? "true" : "false",
		       daemon_up ? "true" : "false", daemon_listening ? "true" : "false");
	} else {
		printf("Jarvis Doctor (%s)\n", healthy ? "READY" : "NEEDS ATTENTION");
		printf("  [Config]\n");
		printf("    %-18s %s\n", "rc file:", has_rc ? rc_path : "none (zero-config defaults)");
		printf("    %-18s %s (wake=%s, tts=%s, wm=%s)\n", "resolved:", lang, wake, voice, wm);
		printf("    %-18s %s\n", "ontology:", ont ? "loaded" : "MISSING");
		printf("  [Audio & %s]\n", audio_backend);
#ifdef __linux__
		printf("    %-18s pw-record=%s pw-cat=%s wpctl=%s\n", "tools:",
		       has_pw_rec ? "OK" : "MISSING", has_pw_cat ? "OK" : "MISSING", has_wpctl ? "OK" : "MISSING");
		printf("    %-18s conf=%s node(jarvis_aec_source)=%s\n", "AEC:",
		       has_aec_conf ? "OK" : "missing (run 'jarvis setup')",
		       has_aec_node ? "ACTIVE" : "inactive");
#else
		printf("    %-18s %s (backend=%s)\n", "capture/play:",
		       has_capture_audio ? "OK (native)" : "MISSING", audio_backend);
#endif
		printf("  [Whisper & GPU]\n");
		printf("    %-18s %s%s\n", "model:", has_model ? model_path : "MISSING (run 'jarvis setup')",
		       en_model_mismatch ? " [WARN: .en model with non-en lang]" : "");
#ifdef __APPLE__
		printf("    %-18s %s\n", "GPU backend:", "Apple Metal / CoreML");
#elif defined(_WIN32)
		printf("    %-18s %s\n", "GPU backend:",
		       has_vulkan ? "Vulkan GPU (vulkan-1.dll)" : "CPU / Win32 (vulkan-1.dll not found)");
#else
		printf("    %-18s %s\n", "vulkan GPU:", has_vulkan ? "/dev/dri/renderD128 OK" : "not detected (CPU fallback)");
#endif
		printf("  [TTS Backends]\n");
		printf("    %-18s edge-tts-pcm=%s python-edge_tts=%s ffmpeg=%s\n", "neural:",
		       has_edge_pcm ? "OK" : "missing", has_edge_py ? "OK" : "missing", has_ffmpeg ? "OK" : "missing");
		printf("    %-18s local-onnx=%s espeak-ng=%s native=%s\n", "offline:",
		       has_local_onnx ? "OK" : "none", has_espeak ? "OK" : "missing",
		       has_native_tts ? "OK" : "n/a");
		printf("  [Agent & Services]\n");
		printf("    %-18s bin=%s ren-jarvis=%s ren-%s=%s\n", "ren:",
		       has_ren ? "OK" : "MISSING", has_ren_jarvis ? "OK" : "missing",
		       wm, has_ren_wm ? "OK" : "missing");
		printf("    %-18s %s\n", "axil-auren:", auren_ok ? "UP (:4242)" : "unreachable (:4242)");
		printf("    %-18s %s\n", "jarvisd:",
		       daemon_up ? (daemon_listening ? "RUNNING (listening: on)" : "RUNNING (listening: off)") : "stopped");
	}
	return 0;
}

static int do_jarvis_setup(bool dry_run, bool as_json)
{
	const char *home = get_cli_home();

	char wdir[512], pwdir[512], cfgdir[512];
	snprintf(wdir, sizeof(wdir), "%s/.local/share/whisper", home);
#ifdef _WIN32
	if (stl_data_dir("whisper", cfgdir, sizeof(cfgdir)) && cfgdir[0])
		snprintf(wdir, sizeof(wdir), "%s", cfgdir);
#endif
	snprintf(pwdir, sizeof(pwdir), "%s/.config/pipewire/pipewire.conf.d", home);
	if (!stl_config_dir("jarvis", cfgdir, sizeof(cfgdir)) || !cfgdir[0])
		snprintf(cfgdir, sizeof(cfgdir), "%s/.config/jarvis", home);

	if (!dry_run) {
		stl_mkdir_p(wdir);
#ifdef __linux__
		stl_mkdir_p(pwdir);
#endif
		stl_mkdir_p(cfgdir);
	}

	char lang[32] = {0}, wake[64] = {0}, voice[64] = {0}, wm[64] = {0};
	const jarvis_ontology_t *ont = get_cli_ont();
	jarvis_resolve_lang(ont, lang, sizeof(lang));
	jarvis_resolve_wake_word(ont, wake, sizeof(wake));
	jarvis_resolve_tts_voice(ont, voice, sizeof(voice));
	jarvis_resolve_wm(wm, sizeof(wm));

	char model_path[1024] = {0};
	bool has_model = find_any_whisper_model(home, strncasecmp(lang, "en", 2) == 0,
	                                        model_path, sizeof(model_path));
	const char *model_action = "present";
	if (!has_model) {
		snprintf(model_path, sizeof(model_path), "%s/ggml-large-v3-turbo-q5_0.bin", wdir);
		static const char *const url =
			"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin";
		if (dry_run) {
			model_action = "would-download";
		} else {
			char tmp_m[1056];
			snprintf(tmp_m, sizeof(tmp_m), "%s.part", model_path);
			int dl_rc = -1;
			if (cmd_in_path("curl")) {
				char *const argv[] = { "curl", "-fL", "-o", tmp_m, (char *)url, NULL };
				dl_rc = run_argv_sync(argv);
			} else if (cmd_in_path("wget")) {
				char *const argv[] = { "wget", "-O", tmp_m, (char *)url, NULL };
				dl_rc = run_argv_sync(argv);
			}
#ifdef _WIN32
			if (dl_rc == 0) remove(model_path);
#endif
			if (dl_rc == 0 && rename(tmp_m, model_path) == 0) {
				has_model = true;
				model_action = "downloaded";
			} else {
				remove(tmp_m);
				model_action = "download-failed";
			}
		}
	}

	char aec_dst[1024] = {0};
	const char *aec_action = "not-applicable";
#ifdef __linux__
	snprintf(aec_dst, sizeof(aec_dst), "%s/99-jarvis-aec.conf", pwdir);
	bool has_aec = (jarvis_plat_is_readable(aec_dst) != 0);
	aec_action = "present";
	if (!has_aec) {
		char aec_src_home[768];
		snprintf(aec_src_home, sizeof(aec_src_home), "%s/.local/share/jarvis/99-jarvis-aec.conf", home);
		const char *cands[] = {
			aec_src_home,
			"/usr/local/share/jarvis/99-jarvis-aec.conf",
			"/usr/share/jarvis/99-jarvis-aec.conf",
			"data/99-jarvis-aec.conf",
			NULL
		};
		const char *found_src = NULL;
		for (size_t i = 0; cands[i]; i++) {
			if (jarvis_plat_is_readable(cands[i])) {
				found_src = cands[i];
				break;
			}
		}
		if (!found_src) {
			aec_action = "source-missing";
		} else if (dry_run) {
			aec_action = "would-install";
		} else if (copy_file_atomic(found_src, aec_dst)) {
			has_aec = true;
			aec_action = "installed";
		} else {
			aec_action = "install-failed";
		}
	}
#endif

	if (as_json) {
		char e_mod[1024], e_aec[1024];
		desktop_core_json_escape(model_path, e_mod, sizeof(e_mod));
		desktop_core_json_escape(aec_dst, e_aec, sizeof(e_aec));
		printf("{\"dry_run\":%s,\"whisper_model\":\"%s\",\"model_status\":\"%s\","
		       "\"aec_conf\":\"%s\",\"aec_status\":\"%s\","
		       "\"defaults\":{\"lang\":\"%s\",\"wake_word\":\"%s\",\"tts_voice\":\"%s\",\"wm\":\"%s\"}}\n",
		       dry_run ? "true" : "false",
		       e_mod, model_action, e_aec, aec_action,
		       lang, wake, voice, wm);
	} else {
		printf("Jarvis Setup%s\n", dry_run ? " (dry-run)" : "");
		printf("  Whisper model: %s (%s)\n", model_path, model_action);
#ifdef __linux__
		printf("  PipeWire AEC : %s (%s)\n", aec_dst, aec_action);
		if (strcmp(aec_action, "installed") == 0) {
			printf("    Note: run 'systemctl --user restart pipewire pipewire-pulse' to activate jarvis_aec_source.\n");
		}
#else
		printf("  Audio backend: %s (native)\n", jarvis_audio_backend_name());
#endif
		printf("  Defaults     : lang=%s wake=%s voice=%s wm=%s (no ~/.jarvisrc required)\n",
		       lang, wake, voice, wm);
		printf("  Switch lang  : jarvis config lang pt   (or 'jarvis config lang en')\n");
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *wm_override = getenv("JARVIS_WM");
	const char *extra_mod = NULL;
	bool dry_run = false;
	bool as_json = false;
	bool quiet = false;
	char utterance[512] = {0};
	size_t u_pos = 0;
	const char *subcmd = NULL;
	const char *subarg = NULL;
	const char *subarg2 = NULL;

	bool end_of_opts = false;
	const char *raw_daemon_cmd = NULL;

	for (int i = 1; i < argc; i++) {
		if (!end_of_opts) {
			if (strcmp(argv[i], "--") == 0) {
				end_of_opts = true;
				continue;
			}
			if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
				print_usage(argv[0]);
				return 0;
			}
			if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
				g_cli_port_override = atoi(argv[++i]);
				continue;
			}
			if ((strcmp(argv[i], "--cmd") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
				raw_daemon_cmd = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "--wm") == 0 && i + 1 < argc) {
				wm_override = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "--surface") == 0 && i + 1 < argc) {
				++i;
				continue;
			}
			if (strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
				extra_mod = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "--dry-run") == 0) {
				dry_run = true;
				continue;
			}
			if (strcmp(argv[i], "--json") == 0) {
				as_json = true;
				continue;
			}
			if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
				quiet = true;
				continue;
			}
			if (u_pos == 0 && !subcmd && argv[i][0] == '-' && argv[i][1] != '\0') {
				fprintf(stderr, "Unknown option: %s\n", argv[i]);
				print_usage(argv[0]);
				return 1;
			}
		}
		if (u_pos == 0 && !subcmd && !end_of_opts) {
			if (strcmp(argv[i], "dmenu") == 0 || strcmp(argv[i], "omni") == 0 ||
			    strcmp(argv[i], "warmup") == 0 || strcmp(argv[i], "clear-learned") == 0 ||
			    strcmp(argv[i], "tts-interrupt") == 0 || strcmp(argv[i], "interrupt") == 0 ||
			    strcmp(argv[i], "quiet") == 0 || strcmp(argv[i], "speak") == 0 ||
			    strcmp(argv[i], "notify") == 0 ||
			    strcmp(argv[i], "start") == 0 || strcmp(argv[i], "on") == 0 ||
			    strcmp(argv[i], "jarvis-start") == 0 || strcmp(argv[i], "server-start") == 0 ||
			    strcmp(argv[i], "stop") == 0 || strcmp(argv[i], "off") == 0 ||
			    strcmp(argv[i], "jarvis-stop") == 0 || strcmp(argv[i], "restart") == 0 ||
			    strcmp(argv[i], "toggle") == 0 || strcmp(argv[i], "jarvis-toggle") == 0 ||
			    strcmp(argv[i], "listen") == 0 ||
			    strcmp(argv[i], "status") == 0 || strcmp(argv[i], "doctor") == 0 ||
			    strcmp(argv[i], "setup") == 0 ||
			    strcmp(argv[i], "lockdown") == 0 || strcmp(argv[i], "lock") == 0 ||
			    strcmp(argv[i], "menu") == 0 || strcmp(argv[i], "ren") == 0 ||
			    strcmp(argv[i], "ren-show") == 0 || strcmp(argv[i], "tts-off") == 0) {
				subcmd = argv[i];
				continue;
			}
			if (strcmp(argv[i], "tts-on") == 0) {
				subcmd = "tts-on";
				if (i + 1 < argc) subarg = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "config") == 0) {
				subcmd = "config";
				if (i + 1 < argc) subarg = argv[++i];
				if (i + 1 < argc) subarg2 = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "query") == 0 || strcmp(argv[i], "exec") == 0 || strcmp(argv[i], "parse") == 0) {
				continue;
			}
			if (strcmp(argv[i], "dry-run") == 0) {
				dry_run = true;
				continue;
			}
		}
		u_pos += (size_t)snprintf(utterance + u_pos,
		                          u_pos < sizeof(utterance) ? sizeof(utterance) - u_pos : 0,
		                          "%s%s", u_pos ? " " : "", argv[i]);
	}

	if (raw_daemon_cmd) {
		char resp[4096];
		if (!daemon_cmd(raw_daemon_cmd, resp, sizeof(resp))) {
			if (!quiet) fprintf(stderr, "Failed to contact jarvisd\n");
			return 1;
		}
		if (resp[0]) printf("%s\n", resp);
		return 0;
	}

	if (subcmd) {
		if (strcmp(subcmd, "start") == 0 || strcmp(subcmd, "on") == 0 || strcmp(subcmd, "jarvis-start") == 0) {
			do_jarvis_start(quiet);
			return 0;
		}
		if (strcmp(subcmd, "server-start") == 0) {
			ensure_daemon_running(false);
			return 0;
		}
		if (strcmp(subcmd, "stop") == 0 || strcmp(subcmd, "off") == 0 || strcmp(subcmd, "jarvis-stop") == 0) {
			do_jarvis_stop(quiet);
			return 0;
		}
		if (strcmp(subcmd, "restart") == 0) {
			bool was_active = is_daemon_listening();
			restart_daemon(was_active);
			if (as_json) {
				printf("{\"restarted\":true,\"listening\":%s}\n", was_active ? "true" : "false");
			} else if (!quiet) {
				printf("Restarted jarvisd (listening: %s)\n", was_active ? "on" : "off");
			}
			return 0;
		}
		if (strcmp(subcmd, "toggle") == 0 || strcmp(subcmd, "jarvis-toggle") == 0) {
			if (is_daemon_listening()) do_jarvis_stop(quiet);
			else do_jarvis_start(quiet);
			return 0;
		}
		if (strcmp(subcmd, "listen") == 0) {
			return do_jarvis_listen(as_json, quiet, wm_override, extra_mod);
		}
		if (strcmp(subcmd, "status") == 0) {
			bool on = is_daemon_listening();
			if (as_json) printf("{\"listening\":%s}\n", on ? "true" : "false");
			else printf("%s\n", on ? "on" : "off");
			return 0;
		}
		if (strcmp(subcmd, "doctor") == 0) {
			return do_jarvis_doctor(as_json);
		}
		if (strcmp(subcmd, "setup") == 0) {
			return do_jarvis_setup(dry_run, as_json);
		}
		if (strcmp(subcmd, "lockdown") == 0 || strcmp(subcmd, "lock") == 0) {
			do_jarvis_lockdown(quiet);
			return 0;
		}
		if (strcmp(subcmd, "menu") == 0) {
			do_jarvis_menu(wm_override, extra_mod);
			return 0;
		}
		if (strcmp(subcmd, "ren") == 0 || strcmp(subcmd, "ren-show") == 0) {
			open_ren_terminal(wm_override, extra_mod);
			return 0;
		}
		if (strcmp(subcmd, "tts-on") == 0) {
			const jarvis_ontology_t *ont = get_cli_ont();
			jarvis_rc_set("tts_enabled", "1");
			if (subarg && *subarg) {
				jarvis_rc_set("tts_voice", subarg);
				if (strcasecmp(subarg, "auto") == 0) {
					jarvis_rc_unset("tts_lang");
				}
			}
			char v[64], wake[64], ttitle[128], greet[128], cmd_sp[192], resp[64];
			get_cli_default_voice(ont, v, sizeof(v));
			get_cli_wake_word(ont, wake, sizeof(wake));
			jarvis_ontology_format_ui(ont, "tts_on_title", wake, "🗣  Neural Voice Active", ttitle, sizeof(ttitle));
			if (!quiet) cli_notify(ttitle, v);
			jarvis_ontology_format_ui(ont, "tts_greeting", wake, "Ready.", greet, sizeof(greet));
			snprintf(cmd_sp, sizeof(cmd_sp), "TTS_SPEAK|%s", greet);
			daemon_cmd(cmd_sp, resp, sizeof(resp));
			return 0;
		}
		if (strcmp(subcmd, "tts-off") == 0) {
			const jarvis_ontology_t *ont = get_cli_ont();
			jarvis_rc_set("tts_enabled", "0");
			char resp[64], wake[64], ttitle[128], tbody[256];
			get_cli_wake_word(ont, wake, sizeof(wake));
			daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));
			jarvis_ontology_format_ui(ont, "tts_off_title", wake, "🗣  Neural Voice Disabled", ttitle, sizeof(ttitle));
			jarvis_ontology_format_ui(ont, "tts_off_body", wake, "Silent mode active.", tbody, sizeof(tbody));
			if (!quiet) cli_notify(ttitle, tbody);
			return 0;
		}
		if (strcmp(subcmd, "config") == 0) {
			if (!subarg) {
				char rc_p[512];
				resolve_rc_file(rc_p, sizeof(rc_p));
				FILE *fp = fopen(rc_p, "r");
				if (fp) {
					char line[512];
					while (fgets(line, sizeof(line), fp)) fputs(line, stdout);
					fclose(fp);
				}
				return 0;
			}
			if (!subarg2) {
				char val[512];
				if (jarvis_rc_get(subarg, "", val, sizeof(val))) {
					printf("%s\n", val);
					return 0;
				}
				return 1;
			}
			if (strcmp(subarg, "unset") == 0) {
				return jarvis_rc_unset(subarg2) ? 0 : 1;
			}
			bool ok = jarvis_rc_set(subarg, subarg2);
			if (ok && strcasecmp(subarg, "lang") == 0) {
				jarvis_rc_unset("tts_lang");
			}
			return ok ? 0 : 1;
		}
		if (strcmp(subcmd, "tts-interrupt") == 0 || strcmp(subcmd, "interrupt") == 0 || strcmp(subcmd, "quiet") == 0) {
			char resp[64];
			daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));
			return 0;
		}
		if (strcmp(subcmd, "speak") == 0) {
			if (utterance[0]) {
				char cmd[1024], resp[64];
				snprintf(cmd, sizeof(cmd), "TTS_SPEAK|%s", utterance);
				if (!daemon_cmd(cmd, resp, sizeof(resp))) {
					fprintf(stderr, "Error: jarvisd unavailable for TTS\n");
					return 1;
				}
			}
			return 0;
		}
		if (strcmp(subcmd, "notify") == 0) {
			if (utterance[0]) {
				const jarvis_ontology_t *ont = get_cli_ont();
				char wake[64], ntitle[128];
				get_cli_wake_word(ont, wake, sizeof(wake));
				jarvis_ontology_format_ui(ont, "notify_title", wake, "  {Wake}", ntitle, sizeof(ntitle));
				char cmd[1024], resp[64];
				snprintf(cmd, sizeof(cmd), "TTS_NOTIFY|%s|%s", ntitle, utterance);
				if (!daemon_cmd(cmd, resp, sizeof(resp)) && !quiet) {
					cli_notify(ntitle, utterance);
				}
			}
			return 0;
		}
		if (strcmp(subcmd, "clear-learned") == 0) {
			do_clear_learned(quiet);
			return 0;
		}
		if (strcmp(subcmd, "warmup") == 0) {
			(void)get_cli_ont();
			stl_socket_t fd = connect_ren_socket(true, wm_override, extra_mod);
			if (fd != STL_INVALID_SOCKET) stl_sock_close(fd);
			return 0;
		}
		if (strcmp(subcmd, "dmenu") == 0 || strcmp(subcmd, "omni") == 0) {
			return do_dmenu(as_json, quiet, wm_override, extra_mod);
		}
	}

	if (!utterance[0]) {
		print_usage(argv[0]);
		return 1;
	}

	if (dry_run) {
		const char *sock_path = jarvis_ren_endpoint();
		if (as_json) {
			char eu[512], es[128];
			desktop_core_json_escape(utterance, eu, sizeof(eu));
			desktop_core_json_escape(sock_path, es, sizeof(es));
			printf("{\"dry_run\":true,\"utterance\":\"%s\",\"target\":\"%s\"}\n", eu, es);
		} else {
			printf("DRY_RUN: target=%s utterance=\"%s\"\n", sock_path, utterance);
		}
		return 0;
	}

	return dispatch_ren_query(utterance, as_json, quiet, wm_override, extra_mod);
}
