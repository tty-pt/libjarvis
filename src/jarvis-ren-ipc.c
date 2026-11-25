#define _GNU_SOURCE
#include "jarvis-ren-ipc.h"
#include "jarvis-platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

const char *jarvis_ren_endpoint(void)
{
	static char s_ep[512];
	const char *env = getenv("JARVIS_REN_SOCK");
	if (env && *env) {
		if (env[0] == '/' && strncmp(env, "unix:", 5) != 0) {
			snprintf(s_ep, sizeof(s_ep), "unix:%s", env);
			return s_ep;
		}
		snprintf(s_ep, sizeof(s_ep), "%s", env);
		return s_ep;
	}
	return "ren-jarvis";
}

void jarvis_ren_cleanup_stale(void)
{
	char tmpdir[256], user[64], p[512];
	stl_temp_dir(tmpdir, sizeof(tmpdir));
	stl_username(user, sizeof(user));

	snprintf(p, sizeof(p), "%s/ren-jarvis-%s.sock",
	         tmpdir[0] ? tmpdir : "/tmp", user[0] ? user : "user");
	unlink(p);

	snprintf(p, sizeof(p), "%s/ren-jarvis-%s.port",
	         tmpdir[0] ? tmpdir : "/tmp", user[0] ? user : "user");
	unlink(p);

#ifndef _WIN32
	if (jarvis_plat_find_in_path("tmux", p, sizeof(p)) == 0) {
		const char *argv[] = { "tmux", "kill-session", "-t", "jarvis", NULL };
		stl_proc_t proc;
		if (stl_spawn(argv, 0, &proc) == 0) {
			int ec = 0;
			stl_proc_wait(&proc, &ec, 1000);
			stl_proc_close(&proc);
		}
	}
#endif
}

static int mod_exists(const char *mod_name, const char *ext)
{
	const char *home = getenv("HOME");
	char hbuf[512];
	if (!home || !*home) {
		stl_home_dir(hbuf, sizeof(hbuf));
		home = hbuf[0] ? hbuf : NULL;
	}
	char cand[1024];
	if (home && *home) {
		snprintf(cand, sizeof(cand), "%s/.local/lib/ren/%s.%s", home, mod_name, ext);
		if (jarvis_plat_is_readable(cand))
			return 1;
	}
	snprintf(cand, sizeof(cand), "/usr/local/lib/ren/%s.%s", mod_name, ext);
	if (jarvis_plat_is_readable(cand))
		return 1;
	snprintf(cand, sizeof(cand), "/usr/lib/ren/%s.%s", mod_name, ext);
	if (jarvis_plat_is_readable(cand))
		return 1;
	snprintf(cand, sizeof(cand), "mods/%s.%s", mod_name, ext);
	if (jarvis_plat_is_readable(cand))
		return 1;
	return 0;
}

#if !defined(__APPLE__) && !defined(_WIN32)
static void detect_unix_wm_mod(char *wm_mod, size_t cap)
{
	const char *hypr = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	if (hypr && *hypr) {
		snprintf(wm_mod, cap, "ren-hyprland");
		return;
	}
	const char *sway = getenv("SWAYSOCK");
	if (sway && *sway) {
		snprintf(wm_mod, cap, "ren-sway");
		return;
	}
	static const char *const ENV_KEYS[] = {
		"XDG_CURRENT_DESKTOP", "DESKTOP_SESSION", "WINDOWMANAGER", NULL
	};
	for (size_t i = 0; ENV_KEYS[i]; i++) {
		const char *v = getenv(ENV_KEYS[i]);
		if (!v || !*v) continue;
		if (strcasestr(v, "hyprland")) {
			snprintf(wm_mod, cap, "ren-hyprland");
			return;
		}
		if (strcasestr(v, "sway") || strcasestr(v, "i3")) {
			snprintf(wm_mod, cap, "ren-sway");
			return;
		}
		if (strcasestr(v, "dwm")) {
			snprintf(wm_mod, cap, "ren-dwm");
			return;
		}
	}
	snprintf(wm_mod, cap, "ren-desktop");
}
#endif

int jarvis_ren_resolve_mods(const char *wm_hint, const char *rc_mods,
                            char *out_csv, size_t cap)
{
	if (!out_csv || cap == 0) return -1;
	out_csv[0] = '\0';

	if (rc_mods && *rc_mods) {
		snprintf(out_csv, cap, "%s", rc_mods);
		return 0;
	}

	const char *ext = stl_so_ext();
	char wm_mod[80] = {0};
	if (wm_hint && *wm_hint) {
		snprintf(wm_mod, sizeof(wm_mod), "ren-%s", wm_hint);
	} else {
#ifdef __APPLE__
		snprintf(wm_mod, sizeof(wm_mod), "ren-macos");
#elif defined(_WIN32)
		snprintf(wm_mod, sizeof(wm_mod), "ren-win32");
#else
		detect_unix_wm_mod(wm_mod, sizeof(wm_mod));
#endif
	}

#ifdef __APPLE__
	const char *cands[] = {
		"ren-macos", wm_mod, "ren-reflex", "ren-firefox", "ren-desktop", "ren-jarvis", "ren-mm", NULL
	};
#elif defined(_WIN32)
	const char *cands[] = {
		"ren-win32", wm_mod, "ren-reflex", "ren-firefox", "ren-desktop", "ren-jarvis", "ren-mm", NULL
	};
#else
	const char *cands[] = {
		"ren-reflex", "ren-x11", wm_mod, "ren-firefox", "ren-desktop", "ren-jarvis", "ren-mm", NULL
	};
#endif

	size_t off = 0;
	for (size_t i = 0; cands[i]; i++) {
		if (!cands[i][0]) continue;
		/* Deduplicate against already appended modules */
		int dup = 0;
		for (size_t j = 0; j < i; j++) {
			if (strcmp(cands[j], cands[i]) == 0) {
				dup = 1;
				break;
			}
		}
		if (dup) continue;

		if (strcmp(cands[i], "ren-jarvis") == 0 ||
		    strcmp(cands[i], "ren-mm") == 0 ||
		    strcmp(cands[i], "ren-reflex") == 0 ||
		    mod_exists(cands[i], ext)) {
			off += (size_t)snprintf(out_csv + off, cap - off,
			                        "%s%s", off > 0 ? ":" : "", cands[i]);
			if (off >= cap) break;
		}
	}
	if (!out_csv[0])
		snprintf(out_csv, cap, "ren-jarvis:ren-reflex:ren-mm");
	return 0;
}

stl_socket_t jarvis_ren_connect(int timeout_ms)
{
	stl_sock_init();
	stl_socket_t fd = stl_ipc_connect(jarvis_ren_endpoint());
	if (fd != STL_INVALID_SOCKET && timeout_ms > 0)
		stl_sock_set_timeout(fd, timeout_ms, timeout_ms);
	return fd;
}

stl_socket_t jarvis_ren_ensure_session(const char *workdir,
                                       const char *extra_mods_csv,
                                       const char *wm_hint)
{
	const char *ep = jarvis_ren_endpoint();
	jarvis_ren_cleanup_stale();

	char mods[512];
	jarvis_ren_resolve_mods(wm_hint, extra_mods_csv, mods, sizeof(mods));

	char ren_mods[768];
	if (extra_mods_csv && *extra_mods_csv && strcmp(extra_mods_csv, mods) != 0) {
		snprintf(ren_mods, sizeof(ren_mods), "%s", extra_mods_csv);
	} else {
		snprintf(ren_mods, sizeof(ren_mods), "%s", mods);
	}

	char wd[1024];
	if (workdir && *workdir) {
		snprintf(wd, sizeof(wd), "%s", workdir);
	} else {
		const char *env_wd = getenv("JARVIS_WORKDIR");
		if (env_wd && *env_wd) {
			snprintf(wd, sizeof(wd), "%s", env_wd);
		} else {
			char hbuf[512];
			stl_home_dir(hbuf, sizeof(hbuf));
			snprintf(wd, sizeof(wd), "%s/site", hbuf[0] ? hbuf : ".");
			struct stat st;
			if (stat(wd, &st) != 0 || !S_ISDIR(st.st_mode))
				snprintf(wd, sizeof(wd), "%s", hbuf[0] ? hbuf : ".");
		}
	}

#if !defined(_WIN32)
	char tmux_bin[512];
	if (!getenv("JARVIS_NO_TMUX") && jarvis_plat_find_in_path("tmux", tmux_bin, sizeof(tmux_bin)) == 0) {
		const char *argv[] = {
			"sh", "-c",
			"cd \"$1\" 2>/dev/null || true; "
			"tmux kill-session -t jarvis >/dev/null 2>&1 || true; "
			"tmux new-session -d -s jarvis -x 120 -y 36 -c \"$1\" "
			"ren --session jarvis --listen \"$2\" --max-turns 16 -m \"$3\" >/dev/null 2>&1 "
			"|| exec ren --session jarvis --listen \"$2\" --max-turns 16 -m \"$3\"",
			"sh", wd, ep, ren_mods, NULL
		};
		if (stl_spawn_detached(argv) != 0)
			return STL_INVALID_SOCKET;
	} else
#endif
	{
		char prev_cwd[512] = {0};
		if (wd[0]) {
#ifdef _WIN32
			if (_getcwd(prev_cwd, (int)sizeof(prev_cwd)))
				if (_chdir(wd) != 0) prev_cwd[0] = '\0';
#else
			if (getcwd(prev_cwd, sizeof(prev_cwd)))
				if (chdir(wd) != 0) prev_cwd[0] = '\0';
#endif
		}
		const char *argv[] = {
			"ren", "--headless", "--session", "jarvis",
			"--listen", ep, "--max-turns", "16", "-m", ren_mods, NULL
		};
		int rc = stl_spawn_detached(argv);
		if (prev_cwd[0]) {
#ifdef _WIN32
			if (_chdir(prev_cwd) != 0) {}
#else
			if (chdir(prev_cwd) != 0) {}
#endif
		}
		if (rc != 0)
			return STL_INVALID_SOCKET;
	}

	for (int i = 0; i < 40; i++) {
		stl_sleep_ms(50);
		stl_socket_t fd = jarvis_ren_connect(0);
		if (fd != STL_INVALID_SOCKET)
			return fd;
	}
	return STL_INVALID_SOCKET;
}

void jarvis_ren_reader_init(jarvis_ren_reader_t *r)
{
	if (!r) return;
	r->len = 0;
	r->pos = 0;
}

int jarvis_ren_read_line(stl_socket_t fd, jarvis_ren_reader_t *r,
                         char *out_line, size_t cap)
{
	if (fd == STL_INVALID_SOCKET || !r || !out_line || cap == 0)
		return -1;

	size_t w = 0;
	for (;;) {
		while (r->pos < r->len) {
			char c = r->buf[r->pos++];
			if (c == '\n') {
				while (w > 0 && out_line[w - 1] == '\r')
					w--;
				out_line[w] = '\0';
				return 1;
			}
			if (w + 1 < cap)
				out_line[w++] = c;
		}
		r->pos = 0;
		r->len = 0;
		ssize_t n = stl_sock_recv(fd, r->buf, sizeof(r->buf), 0);
		if (n <= 0) {
			if (w > 0) {
				while (w > 0 && out_line[w - 1] == '\r')
					w--;
				out_line[w] = '\0';
				return 1;
			}
			out_line[0] = '\0';
			return (n == 0) ? 0 : -1;
		}
		r->len = (size_t)n;
	}
}
