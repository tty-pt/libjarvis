#define _GNU_SOURCE
#include "../include/ttypt/jarvis.h"
#include "../include/ttypt/desktop-core.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
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
		"  toggle                                    Toggle wake-word listener on/off\n"
		"  status                                    Print listener status ('on' or 'off')\n"
		"  lockdown | lock                           Disable listener and lock screen\n"
		"  menu                                      Open interactive voice & daemon settings menu\n"
		"  config [key] [value]                      Get or set ~/.jarvisrc configuration values\n"
		"  tts-on [voice]                            Enable neural TTS (optionally setting voice)\n"
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
		"  --wm <dwm|sway|hyprland>                  Window manager hint for ren session startup\n"
		"  --surface <any|terminal|browser|site|nd>  Accepted for CLI compatibility\n"
		"  --mod <name>                              Extra ren module name to load on session start\n"
		"  --dry-run                                 Show normalized/socket dispatch preview without running\n"
		"  --json                                    Emit JSON output\n"
		"  -q, --quiet                               Suppress desktop notifications\n",
		prog
	);
}

#ifndef _WIN32
static const char *get_cli_username(void)
{
	const char *u = getenv("USER");
	return (u && *u) ? u : "quirinpa";
}

static void resolve_rc_file(char *out, size_t out_sz)
{
	const char *env_rc = getenv("JARVISRC");
	if (env_rc && *env_rc) {
		snprintf(out, out_sz, "%s", env_rc);
		return;
	}
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";
	snprintf(out, out_sz, "%s/.jarvisrc", home);
	if (access(out, R_OK) != 0) {
		char alt[512];
		snprintf(alt, sizeof(alt), "%s/.config/jarvis/jarvisrc", home);
		if (access(alt, R_OK) == 0) {
			snprintf(out, out_sz, "%s", alt);
		}
	}
}

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

static int run_argv_sync(char *const argv[])
{
	if (!argv || !argv[0]) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
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
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void cli_notify(const char *title, const char *body)
{
	if (!title || !*title) return;
	char *const argv[] = {
		"notify-send",
		"-t", "3000",
		"-h", "string:x-dunst-stack-tag:jarvis",
		(char *)title,
		(char *)(body ? body : ""),
		NULL
	};
	desktop_core_spawn_detached_argv(argv);
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

static int cli_dmenu(const char *prompt, const char *items, char *out_sel, size_t out_sz)
{
	const char *p = (prompt && *prompt) ? prompt : "Jarvis:";
	char *const dmenu_argv[] = { "dmenu", "-i", "-p", (char *)p, NULL };
	int rc = run_menu_program(dmenu_argv, items, out_sel, out_sz);
	if (rc == -127 && getenv("WAYLAND_DISPLAY")) {
		char *const bemenu_argv[] = { "bemenu", "-i", "-p", (char *)p, NULL };
		rc = run_menu_program(bemenu_argv, items, out_sel, out_sz);
		if (rc == -127) {
			char *const wofi_argv[] = { "wofi", "--dmenu", "-i", "-p", (char *)p, NULL };
			rc = run_menu_program(wofi_argv, items, out_sel, out_sz);
		}
	}
	return rc;
}

static void notify_status_bar(void)
{
	const char *u = get_cli_username();
	char path[256];
	if (!jarvis_rc_get("status_notify_pid_file", NULL, path, sizeof(path)) || !path[0]) {
		snprintf(path, sizeof(path), "/tmp/dwm-bar-%s.pid", u);
	}
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	int pid = 0;
	if (fscanf(fp, "%d", &pid) == 1 && pid > 1) {
		kill((pid_t)pid, SIGUSR1);
	}
	fclose(fp);
}

static int g_cli_port_override = 0;

static bool daemon_cmd(const char *cmd, char *out_resp, size_t resp_sz)
{
	if (out_resp && resp_sz > 0) out_resp[0] = '\0';
	int port = (g_cli_port_override > 0) ? g_cli_port_override : jarvis_rc_get_int("port", 8179);
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return false;

	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return false;
	}
	char req[1024];
	int n = snprintf(req, sizeof(req), "%s\n", cmd);
	if (n > 0) {
		write_all_fd(s, req, (size_t)n);
	}
	if (out_resp && resp_sz > 0) {
		ssize_t rd = read(s, out_resp, resp_sz - 1);
		if (rd > 0) {
			out_resp[rd] = '\0';
			size_t l = strlen(out_resp);
			while (l > 0 && isspace((unsigned char)out_resp[l - 1])) out_resp[--l] = '\0';
		}
	}
	close(s);
	return true;
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

	const char *home = getenv("HOME");
	char bin_path[512];
	if (home && *home) {
		snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/jarvisd", home);
		if (access(bin_path, X_OK) != 0) {
			snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/dwm-whisperd", home);
		}
	} else {
		snprintf(bin_path, sizeof(bin_path), "jarvisd");
	}

	char *const start_svc[] = { "systemctl", "--user", "start", "jarvisd.service", NULL };
	bool started_systemd = (run_argv_sync(start_svc) == 0);
	if (!started_systemd) {
		if (activate_listener) {
			char *const fb[] = { bin_path, "-j", NULL };
			desktop_core_spawn_detached_argv(fb);
		} else {
			char *const fb[] = { bin_path, NULL };
			desktop_core_spawn_detached_argv(fb);
		}
	}

	int max_polls = started_systemd ? 120 : 60;
	for (int i = 0; i < max_polls; i++) {
		if (daemon_cmd("PING", resp, sizeof(resp))) {
			if (started_systemd) {
				daemon_cmd(activate_listener ? "JARVIS_ON" : "JARVIS_OFF", resp, sizeof(resp));
			}
			break;
		}
		usleep(100000);
	}
}

static void restart_daemon(bool was_active)
{
	char *const stop_svc[] = { "systemctl", "--user", "stop", "jarvisd.service", NULL };
	run_argv_sync(stop_svc);
	char *const pk1[] = { "pkill", "-x", "jarvisd", NULL };
	run_argv_sync(pk1);
	char *const pk2[] = { "pkill", "-x", "dwm-whisperd", NULL };
	run_argv_sync(pk2);
	usleep(120000);
	ensure_daemon_running(was_active);
	notify_status_bar();
}

static void ren_socket_path(char *out, size_t out_sz)
{
	snprintf(out, out_sz, "/tmp/ren-jarvis-%.48s.sock", get_cli_username());
}

static int connect_ren_socket(bool auto_start, const char *wm_override, const char *extra_mod)
{
	char sock_path[96];
	ren_socket_path(sock_path, sizeof(sock_path));

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		return fd;
	}
	close(fd);

	if (!auto_start) return -1;

	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";

	unlink(sock_path);

	char workdir[512];
	const char *env_wd = getenv("JARVIS_WORKDIR");
	if (env_wd && *env_wd && access(env_wd, X_OK) == 0) {
		snprintf(workdir, sizeof(workdir), "%s", env_wd);
	} else {
		snprintf(workdir, sizeof(workdir), "%s/site", home);
		if (access(workdir, X_OK) != 0)
			snprintf(workdir, sizeof(workdir), "%s", home);
	}

	char ren_mods[512] = {0};
	if (!jarvis_rc_get("ren_mods", NULL, ren_mods, sizeof(ren_mods)) || !ren_mods[0]) {
		char wm_buf[64] = {0}, wm_mod[80];
		if (wm_override && *wm_override) {
			snprintf(wm_buf, sizeof(wm_buf), "%s", wm_override);
		} else {
			jarvis_rc_get("wm", "dwm", wm_buf, sizeof(wm_buf));
		}
		snprintf(wm_mod, sizeof(wm_mod), "ren-%s", wm_buf[0] ? wm_buf : "dwm");
		const char *cands[] = {
			wm_mod, "ren-reflex", "ren-x11", "ren-firefox", "ren-desktop", "ren-jarvis", "ren-mm", NULL
		};
		size_t mo = 0;
		for (size_t i = 0; cands[i]; i++) {
			char sp[512];
			snprintf(sp, sizeof(sp), "%s/.local/lib/ren/%s.so", home, cands[i]);
			if (strcmp(cands[i], "ren-jarvis") == 0 || strcmp(cands[i], "ren-mm") == 0 ||
			    strcmp(cands[i], "ren-reflex") == 0 || access(sp, R_OK) == 0) {
				mo += (size_t)snprintf(ren_mods + mo, sizeof(ren_mods) - mo,
				                       "%s%s", mo > 0 ? ":" : "", cands[i]);
			}
		}
		if (!ren_mods[0]) snprintf(ren_mods, sizeof(ren_mods), "ren-jarvis:ren-reflex:ren-mm");
	}
	if (extra_mod && *extra_mod) {
		size_t cur = strlen(ren_mods);
		if (cur + strlen(extra_mod) + 2 < sizeof(ren_mods)) {
			snprintf(ren_mods + cur, sizeof(ren_mods) - cur, ":%s", extra_mod);
		}
	}

	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		setsid();
		pid_t gpid = fork();
		if (gpid == 0) {
			int devnull = open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				if (devnull > 2) close(devnull);
			}
			if (chdir(workdir) != 0) {
				/* best effort */
			}
			execlp("sh", "sh", "-c",
			       "tmux kill-session -t jarvis >/dev/null 2>&1 || true; "
			       "tmux new-session -d -s jarvis -x 120 -y 36 -c \"$1\" "
			       "ren --session jarvis --listen \"$2\" --max-turns 16 -m \"$3\" >/dev/null 2>&1 "
			       "|| exec ren --session jarvis --listen \"$2\" --max-turns 16 -m \"$3\"",
			       "sh", workdir, sock_path, ren_mods, (char *)NULL);
			_exit(127);
		}
		_exit(gpid < 0 ? 127 : 0);
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}

	for (int i = 0; i < 40; i++) {
		usleep(50000);
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				return fd;
			}
			close(fd);
		}
	}
	return -1;
}

static bool send_ren_control(const char *cmd)
{
	int fd = connect_ren_socket(false, NULL, NULL);
	if (fd < 0) return false;

	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	char msg[512];
	int n = snprintf(msg, sizeof(msg), "%s\n", cmd);
	if (n > 0) write_all_fd(fd, msg, (size_t)n);

	char buf[256];
	ssize_t r = read(fd, buf, sizeof(buf) - 1);
	(void)r;
	close(fd);
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
	int fd = connect_ren_socket(true, wm_override, extra_mod);
	if (fd < 0) {
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
	if (n > 0) write_all_fd(fd, req, (size_t)n);

	FILE *fp = fdopen(fd, "r");
	if (!fp) {
		close(fd);
		return 1;
	}

	bool ok = true;
	bool any_event = false;
	bool printed_clause = false;
	char source[64] = "ren";
	char reply[4096] = {0};
	char err_msg[512] = {0};
	size_t rpos = 0;

	char line[4096];
	while (fgets(line, sizeof(line), fp)) {
		size_t l = strlen(line);
		while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
		if (l == 0) continue;

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
	fclose(fp);

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
			cli_notify("  Jarvis — Error", err_msg[0] ? err_msg : utterance);
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
		const char *home = getenv("HOME");
		char def_ont[512] = {0};
		if (home && *home)
			snprintf(def_ont, sizeof(def_ont), "%s/.config/jarvis/ontology.json", home);
		char ont_path[512] = {0};
		jarvis_rc_get("ontology_path", def_ont, ont_path, sizeof(ont_path));
		if (ont_path[0])
			s_ont = jarvis_ontology_create(ont_path);
	}
	return s_ont;
}

static void get_cli_wake_word(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	const char *ont_wake = ont ? jarvis_ontology_get_wake_word(ont) : NULL;
	if (!ont_wake || !*ont_wake) ont_wake = "assistant";
	jarvis_rc_get("wake_word", ont_wake, out, out_sz);
	if (!out[0]) snprintf(out, out_sz, "%s", ont_wake);
}

static void get_cli_default_lang(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	const char *ont_lang = ont ? jarvis_ontology_get_lang(ont) : NULL;
	if (!ont_lang || !*ont_lang) ont_lang = "en";
	jarvis_rc_get("lang", ont_lang, out, out_sz);
	if (!out[0]) snprintf(out, out_sz, "%s", ont_lang);
}

static void get_cli_default_voice(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	char lang[32];
	get_cli_default_lang(ont, lang, sizeof(lang));
	const char *def_voice = (strncmp(lang, "pt", 2) == 0) ? "pm_tuga" : "bm_daniel";
	jarvis_rc_get("tts_voice", def_voice, out, out_sz);
	if (!out[0]) snprintf(out, out_sz, "%s", def_voice);
}

static void do_jarvis_start(bool quiet)
{
	char *const unmute[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "0", NULL };
	run_argv_sync(unmute);
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
	char lock_cmd[256];
	jarvis_rc_get("lock_cmd", "slock", lock_cmd, sizeof(lock_cmd));
	if (lock_cmd[0]) {
		usleep(150000);
		desktop_core_spawn_detached_sh(lock_cmd);
	}
}

static void do_clear_learned(bool quiet)
{
	const char *home = getenv("HOME");
	const char *user = get_cli_username();
	char p[512];

	snprintf(p, sizeof(p), "/tmp/jarvis-%s.context.json", user);
	unlink(p);
	if (home && *home) {
		snprintf(p, sizeof(p), "%s/.cache/jarvis/session.json", home);
		unlink(p);
		snprintf(p, sizeof(p), "%s/.cache/jarvis/embeddings.json", home);
		unlink(p);
		snprintf(p, sizeof(p), "%s/.ren/sessions/jarvis", home);
		unlink(p);
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
	const char *home = getenv("HOME");
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
	int fd = connect_ren_socket(true, wm_override, extra_mod);
	if (fd >= 0) close(fd);

	char *const search[] = { "xdotool", "search", "--class", "jarvis-ren", NULL };
	if (run_argv_sync(search) == 0) {
		char *const focus[] = { "dwm-msg", "focus-class", "jarvis-ren", NULL };
		if (run_argv_sync(focus) == 0) return;
	}

	char *const term_argv[] = {
		"st", "-c", "jarvis-ren", "-n", "jarvis-ren",
		"-t", "Jarvis (ren tmux session)",
		"-e", "tmux", "attach", "-t", "jarvis", NULL
	};
	desktop_core_spawn_detached_argv(term_argv);
}

static void do_jarvis_menu(const char *wm_override, const char *extra_mod)
{
	const jarvis_ontology_t *ont = get_cli_ont();
	bool running = is_daemon_listening();
	int tts_on = jarvis_rc_get_int("tts_enabled", 1);
	char voice[64], lang[32], model_path[512], wake[64];
	get_cli_default_voice(ont, voice, sizeof(voice));
	get_cli_default_lang(ont, lang, sizeof(lang));
	jarvis_rc_get("whisper_model", "ggml-base-q8_0.bin", model_path, sizeof(model_path));
	get_cli_wake_word(ont, wake, sizeof(wake));
	int fms = jarvis_rc_get_int("followup_ms", 8000);

	const char *model_base = strrchr(model_path, '/');
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
		char v_off[128], vprompt[128], vitems[1536];
		jarvis_ontology_format_ui(ont, "menu_voice_off", wake, "off                          (Disable Neural TTS Voice)", v_off, sizeof(v_off));
		jarvis_ontology_format_ui(ont, "voice_prompt", wake, "🗣 TTS Voice:", vprompt, sizeof(vprompt));
		snprintf(vitems, sizeof(vitems),
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
			"%s", v_off);
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
					const char *tlang = (vid[0] == 'p') ? "pt-pt" : ((vid[0] == 'a') ? "en-us" : "en-gb-x-rp");
					jarvis_rc_set("tts_lang", tlang);
					char ttitle[128], greet[128], cmd_sp[192], resp[64];
					jarvis_ontology_format_ui(ont, "tts_on_title", wake, "🗣  Neural Voice Active", ttitle, sizeof(ttitle));
					cli_notify(ttitle, vid);
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
				char ltitle[128];
				jarvis_rc_set("lang", gval);
				if (strncmp(gval, "pt", 2) == 0) jarvis_rc_set("tts_lang", "pt-pt");
				else if (strncmp(gval, "en", 2) == 0) jarvis_rc_set("tts_lang", "en-gb-x-rp");
				jarvis_ontology_format_ui(ont, "lang_updated_title", wake, "🌐  Language Updated", ltitle, sizeof(ltitle));
				cli_notify(ltitle, gval);
			}
		}
	} else if (strncmp(choice, "6.", 2) == 0) {
		char m1[128], m2[128], m3[128], m4[128], wprompt[128], mitems[640];
		jarvis_ontology_format_ui(ont, "menu_model_base_q8", wake, "ggml-base-q8_0.bin           (78 MB  - Fast Multilingual Q8)", m1, sizeof(m1));
		jarvis_ontology_format_ui(ont, "menu_model_small_q8", wake, "ggml-small-q8_0.bin          (252 MB - High-Accuracy Multilingual Q8)", m2, sizeof(m2));
		jarvis_ontology_format_ui(ont, "menu_model_small_q5", wake, "ggml-small-q5_1.bin          (182 MB - Multilingual Q5)", m3, sizeof(m3));
		jarvis_ontology_format_ui(ont, "menu_model_en_q8", wake, "ggml-base.en-q8_0.bin        (78 MB  - English-Only Fast Q8)", m4, sizeof(m4));
		jarvis_ontology_format_ui(ont, "whisper_prompt", wake, "🧠 Whisper Model:", wprompt, sizeof(wprompt));
		snprintf(mitems, sizeof(mitems), "%s\n%s\n%s\n%s", m1, m2, m3, m4);
		char mchoice[256] = {0};
		if (cli_dmenu(wprompt, mitems, mchoice, sizeof(mchoice)) > 0 && mchoice[0]) {
			char mfile[128] = {0};
			if (sscanf(mchoice, "%127s", mfile) == 1 && mfile[0]) {
				const char *home = getenv("HOME");
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
		const char *ed = getenv("EDITOR");
		if (!ed || !*ed) ed = "nvim";
		char *const argv[] = { "st", "-e", (char *)ed, rc_path, NULL };
		desktop_core_spawn_detached_argv(argv);
	}
}
#endif /* !_WIN32 */

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
#ifndef _WIN32
				g_cli_port_override = atoi(argv[++i]);
#else
				++i;
#endif
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
			    strcmp(argv[i], "jarvis-stop") == 0 ||
			    strcmp(argv[i], "toggle") == 0 || strcmp(argv[i], "jarvis-toggle") == 0 ||
			    strcmp(argv[i], "status") == 0 ||
			    strcmp(argv[i], "lockdown") == 0 || strcmp(argv[i], "lock") == 0 ||
			    strcmp(argv[i], "menu") == 0 || strcmp(argv[i], "tts-off") == 0) {
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

#ifndef _WIN32
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
		if (strcmp(subcmd, "toggle") == 0 || strcmp(subcmd, "jarvis-toggle") == 0) {
			if (is_daemon_listening()) do_jarvis_stop(quiet);
			else do_jarvis_start(quiet);
			return 0;
		}
		if (strcmp(subcmd, "status") == 0) {
			bool on = is_daemon_listening();
			if (as_json) printf("{\"listening\":%s}\n", on ? "true" : "false");
			else printf("%s\n", on ? "on" : "off");
			return 0;
		}
		if (strcmp(subcmd, "lockdown") == 0 || strcmp(subcmd, "lock") == 0) {
			do_jarvis_lockdown(quiet);
			return 0;
		}
		if (strcmp(subcmd, "menu") == 0) {
			do_jarvis_menu(wm_override, extra_mod);
			return 0;
		}
		if (strcmp(subcmd, "tts-on") == 0) {
			const jarvis_ontology_t *ont = get_cli_ont();
			jarvis_rc_set("tts_enabled", "1");
			if (subarg && *subarg) {
				jarvis_rc_set("tts_voice", subarg);
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
			return jarvis_rc_set(subarg, subarg2) ? 0 : 1;
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
			int fd = connect_ren_socket(true, wm_override, extra_mod);
			if (fd >= 0) close(fd);
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
		char sock_path[96];
		ren_socket_path(sock_path, sizeof(sock_path));
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
#else
	(void)wm_override;
	(void)extra_mod;
	(void)dry_run;
	(void)as_json;
	(void)quiet;
	(void)subcmd;
	(void)subarg;
	(void)subarg2;
	(void)raw_daemon_cmd;
	if (!utterance[0]) {
		print_usage(argv[0]);
		return 1;
	}
	fprintf(stderr, "ren socket client is only supported on POSIX\n");
	return 1;
#endif
}
