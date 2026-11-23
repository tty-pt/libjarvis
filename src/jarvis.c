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
#include <dirent.h>
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
		"  --wm <auto|dwm|sway|hyprland|x11>         Window manager hint for ren session startup\n"
		"  --surface <any|terminal|browser|site|nd>  Accepted for CLI compatibility\n"
		"  --mod <name>                              Extra ren module name to load on session start\n"
		"  --dry-run                                 Preview query dispatch or setup actions without changes\n"
		"  --json                                    Emit JSON output\n"
		"  -q, --quiet                               Suppress desktop notifications\n",
		prog
	);
}

#ifndef _WIN32
#include <pwd.h>
static const char *get_cli_username(void)
{
	static char cached[64];
	const char *u = getenv("USER");
	if (u && *u) return u;
	struct passwd pw, *res = NULL;
	char buf[512];
	if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &res) == 0 && res && res->pw_name && res->pw_name[0]) {
		snprintf(cached, sizeof(cached), "%s", res->pw_name);
		return cached;
	}
	return "user";
}

static void resolve_rc_file(char *out, size_t out_sz)
{
	const char *env_rc = getenv("JARVISRC");
	if (env_rc && *env_rc) {
		snprintf(out, out_sz, "%s", env_rc);
		return;
	}
	char hbuf[512] = {0};
	const char *home = getenv("HOME");
	if (!home || !*home) {
		struct passwd pw, *res = NULL;
		char pbuf[512];
		if (getpwuid_r(getuid(), &pw, pbuf, sizeof(pbuf), &res) == 0 && res && res->pw_dir && res->pw_dir[0]) {
			snprintf(hbuf, sizeof(hbuf), "%s", res->pw_dir);
			home = hbuf;
		} else {
			home = "/tmp";
		}
	}
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

static bool daemon_cmd_timeout(const char *cmd, int timeout_sec, char *out_resp, size_t resp_sz)
{
	if (out_resp && resp_sz > 0) out_resp[0] = '\0';
	int port = (g_cli_port_override > 0) ? g_cli_port_override : jarvis_rc_get_int("port", 8179);
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return false;

	struct timeval tv = { .tv_sec = timeout_sec > 0 ? timeout_sec : 2, .tv_usec = 0 };
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
		size_t total = 0;
		while (total + 1 < resp_sz) {
			ssize_t rd = read(s, out_resp + total, resp_sz - 1 - total);
			if (rd < 0 && errno == EINTR) continue;
			if (rd <= 0) break;
			total += (size_t)rd;
			if (memchr(out_resp, '\n', total) != NULL) break;
		}
		out_resp[total] = '\0';
		size_t l = total;
		while (l > 0 && isspace((unsigned char)out_resp[l - 1])) out_resp[--l] = '\0';
	}
	close(s);
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
		if (wm_override && *wm_override && strcasecmp(wm_override, "auto") != 0) {
			snprintf(wm_buf, sizeof(wm_buf), "%s", wm_override);
		} else {
			jarvis_resolve_wm(wm_buf, sizeof(wm_buf));
		}
		snprintf(wm_mod, sizeof(wm_mod), "ren-%s", wm_buf[0] ? wm_buf : "desktop");
		const char *cands[] = {
			wm_mod, "ren-reflex", "ren-x11", "ren-firefox", "ren-desktop", "ren-jarvis", "ren-mm", NULL
		};
		size_t mo = 0;
		for (size_t i = 0; cands[i]; i++) {
			if (i > 0 && strcmp(cands[i], wm_mod) == 0) continue;
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

static void write_voice_state(const char *state)
{
	const char *u = get_cli_username();
	char path[256], tmp[272];
	snprintf(path, sizeof(path), "/tmp/jarvis-voice-%s.state", u);
	if (!state || !*state) {
		unlink(path);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *fp = fopen(tmp, "w");
	if (fp) {
		fprintf(fp, "%s\n", state);
		fclose(fp);
		rename(tmp, path);
	}
}

static int do_jarvis_listen(bool as_json, bool quiet, const char *wm_override, const char *extra_mod)
{
	const char *u = get_cli_username();
	char pid_path[256];
	snprintf(pid_path, sizeof(pid_path), "/tmp/jarvis-listen-%s.pid", u);

	FILE *pfp = fopen(pid_path, "r");
	if (pfp) {
		int old_pid = 0;
		if (fscanf(pfp, "%d", &old_pid) == 1 && old_pid > 1 && kill((pid_t)old_pid, 0) == 0) {
			fclose(pfp);
			char cresp[64];
			daemon_cmd("CANCEL", cresp, sizeof(cresp));
			unlink(pid_path);
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

	char *const unmute[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "0", NULL };
	run_argv_sync(unmute);

	bool was_listening = is_daemon_listening();
	ensure_daemon_running(was_listening);

	char resp[4300];
	daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));

	const jarvis_ontology_t *ont = get_cli_ont();
	char lang[32], wake[64], state_path[256], stream_cmd[512];
	get_cli_default_lang(ont, lang, sizeof(lang));
	get_cli_wake_word(ont, wake, sizeof(wake));
	snprintf(state_path, sizeof(state_path), "/tmp/jarvis-voice-%s.state", u);

	int silence_ms = jarvis_rc_get_int("cmd_silence_ms", 750);
	int wait_ms = jarvis_rc_get_int("listen_wait_ms", 6000);
	if (silence_ms < 400) silence_ms = 400;
	if (wait_ms < 1000) wait_ms = 1000;

	write_voice_state("jarvis-cmd");
	notify_status_bar();

	char chime_wav[256];
	snprintf(chime_wav, sizeof(chime_wav), "/tmp/jarvis-chime-%s.wav", u);
	if (access(chime_wav, R_OK) != 0) {
		jarvis_voice_write_earcon_wav(chime_wav, false);
	}
	if (access(chime_wav, R_OK) == 0) {
		char *const chime_argv[] = { "pw-play", "--volume=0.75", chime_wav, NULL };
		desktop_core_spawn_detached_argv(chime_argv);
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
		usleep(80000);
		ok = daemon_cmd_timeout(stream_cmd, 25, resp, sizeof(resp));
	}

	unlink(pid_path);

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
		char *const pull[] = { "dwm-msg", "pull-class", "jarvis-ren", NULL };
		if (run_argv_sync(pull) == 0) return;
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
	if (!jarvis_rc_get("whisper_model", NULL, model_path, sizeof(model_path)) || !model_path[0]) {
		const char *home = getenv("HOME");
		snprintf(model_path, sizeof(model_path), "%s/.local/share/whisper/ggml-large-v3-turbo-q5_0.bin",
		         home ? home : "");
		if (access(model_path, R_OK) != 0)
			snprintf(model_path, sizeof(model_path), "ggml-large-v3-turbo-q5_0.bin");
	}
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

static bool cmd_in_path(const char *cmd)
{
	if (!cmd || !*cmd) return false;
	const char *home = getenv("HOME");
	if (home && *home) {
		char lp[768];
		snprintf(lp, sizeof(lp), "%s/.local/bin/%s", home, cmd);
		if (access(lp, X_OK) == 0) return true;
	}
	const char *path_env = getenv("PATH");
	if (!path_env || !*path_env) return false;
	char copy[4096];
	snprintf(copy, sizeof(copy), "%s", path_env);
	char *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		if (!*dir) continue;
		char cand[1024];
		snprintf(cand, sizeof(cand), "%s/%s", dir, cmd);
		if (access(cand, X_OK) == 0) return true;
	}
	return false;
}

static int run_argv_capture(char *const argv[], char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return -1;
	out[0] = '\0';
	if (!argv || !argv[0]) return -1;

	int pfd[2];
	if (pipe(pfd) < 0) return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pfd[1], STDOUT_FILENO);
		int dn = open("/dev/null", O_RDWR);
		if (dn >= 0) {
			dup2(dn, STDIN_FILENO);
			dup2(dn, STDERR_FILENO);
			if (dn > STDERR_FILENO) close(dn);
		}
		close(pfd[0]);
		close(pfd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pfd[1]);
	size_t tot = 0;
	while (tot + 1 < out_sz) {
		ssize_t r = read(pfd[0], out + tot, out_sz - 1 - tot);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		tot += (size_t)r;
	}
	close(pfd[0]);
	out[tot] = '\0';
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static bool probe_http_health(uint16_t port, const char *req_path)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return false;
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return false;
	}
	char req[256];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", req_path);
	if (n > 0) write_all_fd(s, req, (size_t)n);
	char buf[512] = {0};
	ssize_t rd = read(s, buf, sizeof(buf) - 1);
	close(s);
	return rd > 0 && strstr(buf, "200") != NULL;
}

static bool find_any_whisper_model(const char *home, bool allow_en_only, char *out, size_t out_sz)
{
	out[0] = '\0';
	if (jarvis_rc_get("whisper_model", NULL, out, out_sz) && out[0] && access(out, R_OK) == 0) {
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
	for (size_t i = 0; multi[i]; i++) {
		char cand[768];
		snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, multi[i]);
		if (access(cand, R_OK) == 0) {
			snprintf(out, out_sz, "%s", cand);
			return true;
		}
	}
	if (allow_en_only) {
		for (size_t i = 0; en[i]; i++) {
			char cand[768];
			snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, en[i]);
			if (access(cand, R_OK) == 0) {
				snprintf(out, out_sz, "%s", cand);
				return true;
			}
		}
	}
	char wdir[512];
	snprintf(wdir, sizeof(wdir), "%s/.local/share/whisper", home);
	DIR *d = opendir(wdir);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t nl = strlen(de->d_name);
			if (nl > 4 && strcmp(de->d_name + nl - 4, ".bin") == 0) {
				char full[1024];
				snprintf(full, sizeof(full), "%s/%s", wdir, de->d_name);
				snprintf(out, out_sz, "%s", full);
				closedir(d);
				return true;
			}
		}
		closedir(d);
	}
	return false;
}

static bool has_piper_or_kokoro_model(const char *home)
{
	char kokoro[768];
	snprintf(kokoro, sizeof(kokoro), "%s/.local/share/kokoro/kokoro-v1.0.onnx", home);
	if (access(kokoro, R_OK) == 0) return true;
	char pdir[768];
	snprintf(pdir, sizeof(pdir), "%s/.local/share/piper", home);
	DIR *d = opendir(pdir);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			size_t nl = strlen(de->d_name);
			if (nl > 5 && strcmp(de->d_name + nl - 5, ".onnx") == 0) {
				closedir(d);
				return true;
			}
		}
		closedir(d);
	}
	return false;
}

static bool mkdir_p_path(const char *path)
{
	char tmp[768];
	size_t len = strlen(path);
	if (len == 0 || len >= sizeof(tmp)) return false;
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
			*p = '/';
		}
	}
	return (mkdir(tmp, 0755) == 0 || errno == EEXIST);
}

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
		unlink(tmp);
		return false;
	}
	return rename(tmp, dst) == 0;
}

static int do_jarvis_doctor(bool as_json)
{
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";

	const jarvis_ontology_t *ont = get_cli_ont();
	char rc_path[512];
	resolve_rc_file(rc_path, sizeof(rc_path));
	bool has_rc = (access(rc_path, R_OK) == 0);

	char lang[32] = {0}, wake[64] = {0}, voice[64] = {0}, wm[64] = {0};
	jarvis_resolve_lang(ont, lang, sizeof(lang));
	jarvis_resolve_wake_word(ont, wake, sizeof(wake));
	jarvis_resolve_tts_voice(ont, voice, sizeof(voice));
	jarvis_resolve_wm(wm, sizeof(wm));

	bool has_pw_rec = cmd_in_path("pw-record");
	bool has_pw_cat = cmd_in_path("pw-cat");
	bool has_wpctl  = cmd_in_path("wpctl");

	char aec_conf[768];
	snprintf(aec_conf, sizeof(aec_conf), "%s/.config/pipewire/pipewire.conf.d/99-jarvis-aec.conf", home);
	bool has_aec_conf = (access(aec_conf, R_OK) == 0);

	bool has_aec_node = false;
	if (has_wpctl) {
		char wp_out[8192];
		char *const wp_argv[] = { "wpctl", "status", NULL };
		if (run_argv_capture(wp_argv, wp_out, sizeof(wp_out)) == 0) {
			has_aec_node = (strstr(wp_out, "jarvis_aec_source") != NULL);
		}
	}

	bool allow_en = (strncasecmp(lang, "en", 2) == 0);
	char model_path[1024] = {0};
	bool has_model = find_any_whisper_model(home, allow_en, model_path, sizeof(model_path));
	bool en_model_mismatch = false;
	if (has_model && !allow_en &&
	    (strstr(model_path, ".en.") != NULL || strstr(model_path, ".en-") != NULL)) {
		en_model_mismatch = true;
	}
	bool has_vulkan = (access("/dev/dri/renderD128", R_OK) == 0);

	bool has_edge_pcm = cmd_in_path("edge-tts-pcm");
	bool has_ffmpeg   = cmd_in_path("ffmpeg");
	bool has_espeak   = cmd_in_path("espeak-ng");
	bool has_edge_py  = false;
	{
		char venv1[768], venv2[768];
		snprintf(venv1, sizeof(venv1), "%s/.local/share/dwm-voice/venv/bin/python", home);
		snprintf(venv2, sizeof(venv2), "%s/.venv/bin/python", home);
		const char *py_cands[3] = {
			cmd_in_path("python3") ? "python3" : NULL,
			access(venv1, X_OK) == 0 ? venv1 : NULL,
			access(venv2, X_OK) == 0 ? venv2 : NULL
		};
		for (int i = 0; i < 3 && !has_edge_py; i++) {
			if (!py_cands[i]) continue;
			char *const py_argv[] = { (char *)py_cands[i], "-c", "import edge_tts", NULL };
			has_edge_py = (run_argv_sync(py_argv) == 0);
		}
	}
	bool has_local_onnx = has_piper_or_kokoro_model(home);

	bool has_ren = cmd_in_path("ren");
	char ren_wm_so[768], ren_jarvis_so[768];
	snprintf(ren_wm_so, sizeof(ren_wm_so), "%s/.local/lib/ren/ren-%s.so", home, wm);
	snprintf(ren_jarvis_so, sizeof(ren_jarvis_so), "%s/.local/lib/ren/ren-jarvis.so", home);
	bool has_ren_wm = (access(ren_wm_so, R_OK) == 0);
	bool has_ren_jarvis = (access(ren_jarvis_so, R_OK) == 0);

	bool auren_ok = probe_http_health(4242, "/v1/models");
	char ping_resp[64] = {0}, st_resp[64] = {0};
	bool daemon_up = daemon_cmd("PING", ping_resp, sizeof(ping_resp));
	bool daemon_listening = daemon_up && daemon_cmd("JARVIS_STATUS", st_resp, sizeof(st_resp)) &&
	                        (strcmp(st_resp, "ON") == 0);

	bool healthy = (ont != NULL) && has_pw_rec && has_pw_cat && has_model &&
	               !en_model_mismatch && (has_edge_pcm || has_local_onnx || has_espeak) && has_ren;

	if (as_json) {
		char e_rc[640], e_lang[64], e_wake[128], e_voice[128], e_wm[64], e_mod[1024];
		desktop_core_json_escape(has_rc ? rc_path : "none (zero-config defaults)", e_rc, sizeof(e_rc));
		desktop_core_json_escape(lang, e_lang, sizeof(e_lang));
		desktop_core_json_escape(wake, e_wake, sizeof(e_wake));
		desktop_core_json_escape(voice, e_voice, sizeof(e_voice));
		desktop_core_json_escape(wm, e_wm, sizeof(e_wm));
		desktop_core_json_escape(has_model ? model_path : "", e_mod, sizeof(e_mod));
		printf("{\"healthy\":%s,"
		       "\"config\":{\"rc_file\":\"%s\",\"has_rc\":%s,\"lang\":\"%s\",\"wake_word\":\"%s\",\"tts_voice\":\"%s\",\"wm\":\"%s\",\"ontology_loaded\":%s},"
		       "\"audio\":{\"pw_record\":%s,\"pw_cat\":%s,\"wpctl\":%s,\"aec_conf\":%s,\"aec_source_active\":%s},"
		       "\"whisper\":{\"model\":\"%s\",\"model_found\":%s,\"en_model_mismatch\":%s,\"vulkan_render_node\":%s},"
		       "\"tts\":{\"edge_tts_pcm\":%s,\"edge_tts_python\":%s,\"ffmpeg\":%s,\"local_onnx\":%s,\"espeak_ng\":%s},"
		       "\"ren\":{\"ren_bin\":%s,\"ren_jarvis_so\":%s,\"ren_wm_so\":%s,\"axil_auren_health\":%s},"
		       "\"daemon\":{\"running\":%s,\"listening\":%s}}\n",
		       healthy ? "true" : "false",
		       e_rc, has_rc ? "true" : "false", e_lang, e_wake, e_voice, e_wm, ont ? "true" : "false",
		       has_pw_rec ? "true" : "false", has_pw_cat ? "true" : "false",
		       has_wpctl ? "true" : "false", has_aec_conf ? "true" : "false",
		       has_aec_node ? "true" : "false",
		       e_mod, has_model ? "true" : "false", en_model_mismatch ? "true" : "false",
		       has_vulkan ? "true" : "false",
		       has_edge_pcm ? "true" : "false", has_edge_py ? "true" : "false",
		       has_ffmpeg ? "true" : "false", has_local_onnx ? "true" : "false",
		       has_espeak ? "true" : "false",
		       has_ren ? "true" : "false", has_ren_jarvis ? "true" : "false",
		       has_ren_wm ? "true" : "false", auren_ok ? "true" : "false",
		       daemon_up ? "true" : "false", daemon_listening ? "true" : "false");
	} else {
		printf("Jarvis Doctor (%s)\n", healthy ? "READY" : "NEEDS ATTENTION");
		printf("  [Config]\n");
		printf("    %-18s %s\n", "rc file:", has_rc ? rc_path : "none (zero-config defaults)");
		printf("    %-18s %s (wake=%s, tts=%s, wm=%s)\n", "resolved:", lang, wake, voice, wm);
		printf("    %-18s %s\n", "ontology:", ont ? "loaded" : "MISSING");
		printf("  [Audio & PipeWire]\n");
		printf("    %-18s pw-record=%s pw-cat=%s wpctl=%s\n", "tools:",
		       has_pw_rec ? "OK" : "MISSING", has_pw_cat ? "OK" : "MISSING", has_wpctl ? "OK" : "MISSING");
		printf("    %-18s conf=%s node(jarvis_aec_source)=%s\n", "AEC:",
		       has_aec_conf ? "OK" : "missing (run 'jarvis setup')",
		       has_aec_node ? "ACTIVE" : "inactive");
		printf("  [Whisper & GPU]\n");
		printf("    %-18s %s%s\n", "model:", has_model ? model_path : "MISSING (run 'jarvis setup')",
		       en_model_mismatch ? " [WARN: .en model with non-en lang]" : "");
		printf("    %-18s %s\n", "vulkan GPU:", has_vulkan ? "/dev/dri/renderD128 OK" : "not detected (CPU fallback)");
		printf("  [TTS Backends]\n");
		printf("    %-18s edge-tts-pcm=%s python-edge_tts=%s ffmpeg=%s\n", "neural:",
		       has_edge_pcm ? "OK" : "missing", has_edge_py ? "OK" : "missing", has_ffmpeg ? "OK" : "missing");
		printf("    %-18s local-onnx=%s espeak-ng=%s\n", "offline:",
		       has_local_onnx ? "OK" : "none", has_espeak ? "OK" : "missing");
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
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";

	char wdir[512], pwdir[512], cfgdir[512];
	snprintf(wdir, sizeof(wdir), "%s/.local/share/whisper", home);
	snprintf(pwdir, sizeof(pwdir), "%s/.config/pipewire/pipewire.conf.d", home);
	snprintf(cfgdir, sizeof(cfgdir), "%s/.config/jarvis", home);

	if (!dry_run) {
		mkdir_p_path(wdir);
		mkdir_p_path(pwdir);
		mkdir_p_path(cfgdir);
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
			if (dl_rc == 0 && rename(tmp_m, model_path) == 0) {
				has_model = true;
				model_action = "downloaded";
			} else {
				unlink(tmp_m);
				model_action = "download-failed";
			}
		}
	}

	char aec_dst[1024];
	snprintf(aec_dst, sizeof(aec_dst), "%s/99-jarvis-aec.conf", pwdir);
	bool has_aec = (access(aec_dst, R_OK) == 0);
	const char *aec_action = "present";
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
			if (access(cands[i], R_OK) == 0) {
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
		printf("  PipeWire AEC : %s (%s)\n", aec_dst, aec_action);
		if (strcmp(aec_action, "installed") == 0) {
			printf("    Note: run 'systemctl --user restart pipewire pipewire-pulse' to activate jarvis_aec_source.\n");
		}
		printf("  Defaults     : lang=%s wake=%s voice=%s wm=%s (no ~/.jarvisrc required)\n",
		       lang, wake, voice, wm);
		printf("  Switch lang  : jarvis config lang pt   (or 'jarvis config lang en')\n");
	}
	return 0;
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
