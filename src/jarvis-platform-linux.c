#define _GNU_SOURCE
#include "jarvis-platform.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct jarvis_plat_lock {
	int  fd;
	char pid_path[256];
};

jarvis_plat_lock_t *jarvis_plat_singleton_acquire(const char *name)
{
	if (!name || !*name) name = "jarvisd";
	char lock_path[256], pid_path[256];
	jarvis_plat_state_path(name, ".lock", lock_path, sizeof(lock_path));
	jarvis_plat_state_path(name, ".pid", pid_path, sizeof(pid_path));

	int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) return NULL;
	if (stl_flock(fd, STL_LOCK_EX | STL_LOCK_NB) < 0) {
		close(fd);
		return NULL;
	}

	FILE *pfp = fopen(pid_path, "w");
	if (pfp) {
		fprintf(pfp, "%d\n", (int)getpid());
		fclose(pfp);
	}

	jarvis_plat_lock_t *lk = calloc(1, sizeof(*lk));
	if (!lk) {
		close(fd);
		return NULL;
	}
	lk->fd = fd;
	snprintf(lk->pid_path, sizeof(lk->pid_path), "%s", pid_path);
	return lk;
}

void jarvis_plat_singleton_release(jarvis_plat_lock_t *lk)
{
	if (!lk) return;
	if (lk->pid_path[0])
		unlink(lk->pid_path);
	if (lk->fd >= 0) {
		stl_flock(lk->fd, STL_LOCK_UN);
		close(lk->fd);
	}
	free(lk);
}

int jarvis_plat_state_path(const char *prefix, const char *suffix, char *out, size_t cap)
{
	if (!out || cap == 0) return -1;
	char tdir[256], user[64];
	stl_temp_dir(tdir, sizeof(tdir));
	stl_username(user, sizeof(user));
	int n = snprintf(out, cap, "%s/%s-%s%s",
	                 tdir[0] ? tdir : "/tmp",
	                 (prefix && *prefix) ? prefix : "jarvis",
	                 user[0] ? user : "user",
	                 suffix ? suffix : "");
	return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int jarvis_plat_data_file(const char *subdir, const char *filename, char *out, size_t cap)
{
	if (!out || cap == 0 || !subdir || !filename) return -1;
	const char *home = getenv("HOME");
	char hbuf[512];
	if (!home || !*home) {
		stl_home_dir(hbuf, sizeof(hbuf));
		home = hbuf[0] ? hbuf : NULL;
	}
	if (home && *home) {
		snprintf(out, cap, "%s/.local/share/%s/%s", home, subdir, filename);
		if (jarvis_plat_is_readable(out))
			return 0;
	}
	char ddir[512];
	if (stl_data_dir(subdir, ddir, sizeof(ddir)) && ddir[0]) {
		snprintf(out, cap, "%s/%s", ddir, filename);
		if (jarvis_plat_is_readable(out))
			return 0;
	}
	if (home && *home) {
		snprintf(out, cap, "%s/.local/share/%s/%s", home, subdir, filename);
		return 0;
	}
	out[0] = '\0';
	return -1;
}

int jarvis_plat_is_readable(const char *path)
{
	if (!path || !*path) return 0;
	return access(path, R_OK) == 0 ? 1 : 0;
}

int jarvis_plat_is_exec(const char *path)
{
	if (!path || !*path) return 0;
	struct stat st;
	if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
		return 0;
	return access(path, X_OK) == 0 ? 1 : 0;
}

int jarvis_plat_find_in_path(const char *cmd, char *out, size_t cap)
{
	if (!cmd || !*cmd) return -1;
	if (strchr(cmd, '/') != NULL) {
		if (jarvis_plat_is_exec(cmd)) {
			if (out && cap > 0) snprintf(out, cap, "%s", cmd);
			return 0;
		}
		return -1;
	}
	const char *path_env = getenv("PATH");
	if (!path_env || !*path_env) return -1;
	char *copy = strdup(path_env);
	if (!copy) return -1;

	int found = -1;
	char *saveptr = NULL;
	for (char *dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
		if (!*dir) continue;
		char cand[512];
		snprintf(cand, sizeof(cand), "%s/%s", dir, cmd);
		if (jarvis_plat_is_exec(cand)) {
			if (out && cap > 0) snprintf(out, cap, "%s", cand);
			found = 0;
			break;
		}
	}
	free(copy);
	return found;
}

int jarvis_plat_notify_osd(const char *title, const char *body, int timeout_ms)
{
	char tstr[32];
	snprintf(tstr, sizeof(tstr), "%d", timeout_ms > 0 ? timeout_ms : 4500);
	const char *argv[] = {
		"notify-send", "-t", tstr,
		"-h", "string:x-canonical-private-synchronous:jarvis",
		(title && *title) ? title : "Jarvis",
		body ? body : "",
		NULL
	};
	return stl_spawn_detached(argv);
}

void jarvis_plat_notify_bar(void)
{
#ifdef SIGRTMIN
	char pid_s[32] = {0};
	FILE *fp = popen("pidof dwmblocks 2>/dev/null", "r");
	if (fp) {
		if (fgets(pid_s, sizeof(pid_s), fp)) {
			char *tok = strtok(pid_s, " \t\n");
			while (tok) {
				pid_t p = (pid_t)atoi(tok);
				if (p > 1)
					kill(p, SIGRTMIN + 11);
				tok = strtok(NULL, " \t\n");
			}
		}
		pclose(fp);
	}
#endif
}

int jarvis_plat_lock_screen(const char *override_cmd)
{
	if (override_cmd && *override_cmd) {
		const char *argv[] = { "sh", "-c", override_cmd, NULL };
		return stl_spawn_detached(argv);
	}
	const char *argv[] = { "slock", NULL };
	return stl_spawn_detached(argv);
}

int jarvis_plat_open_url(const char *url)
{
	if (!url || !*url) return -1;
	const char *argv[] = { "xdg-open", url, NULL };
	return stl_spawn_detached(argv);
}

void jarvis_plat_kbd_lang(char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';

	FILE *fp = popen("xkb-switch 2>/dev/null || (setxkbmap -query 2>/dev/null | awk '/layout/{print $2}')", "r");
	if (!fp) return;
	char buf[128] = {0};
	if (fgets(buf, sizeof(buf), fp)) {
		size_t n = strlen(buf);
		while (n > 0 && isspace((unsigned char)buf[n - 1]))
			buf[--n] = '\0';
		if (strncmp(buf, "us", 2) == 0 || strncmp(buf, "en", 2) == 0 || strncmp(buf, "gb", 2) == 0) {
			snprintf(out, cap, "en");
		} else if (strncmp(buf, "pt", 2) == 0 || strncmp(buf, "br", 2) == 0) {
			snprintf(out, cap, "pt");
		} else if (buf[0]) {
			snprintf(out, cap, "%.15s", buf);
		}
	}
	pclose(fp);
}

int16_t *jarvis_plat_tts_fallback_pcm(const char *text, const char *lang_hint,
                                      size_t *out_samples, int *out_rate)
{
	(void)text;
	(void)lang_hint;
	if (out_samples) *out_samples = 0;
	if (out_rate) *out_rate = 22050;
	return NULL;
}

const char *jarvis_plat_espeak_lib_name(void)
{
	return "libespeak-ng.so.1";
}

const char *jarvis_plat_ort_lib_name(int alt_index)
{
	return alt_index == 0 ? "libonnxruntime.so.1.23" : "libonnxruntime.so";
}

void jarvis_plat_suppress_stderr_begin(int *saved_fd)
{
	if (!saved_fd) return;
	*saved_fd = dup(STDERR_FILENO);
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, STDERR_FILENO);
		close(devnull);
	}
}

void jarvis_plat_suppress_stderr_end(int saved_fd)
{
	if (saved_fd >= 0) {
		dup2(saved_fd, STDERR_FILENO);
		close(saved_fd);
	}
}

int jarvis_plat_sock_peer_closed(stl_socket_t fd)
{
	if (fd == STL_INVALID_SOCKET) return 1;
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN | POLLHUP | POLLERR;
	pfd.revents = 0;
	if (stl_poll(&pfd, 1, 0) <= 0)
		return 0;
	if (pfd.revents & (POLLHUP | POLLERR))
		return 1;
	if (pfd.revents & POLLIN) {
		char peek = 0;
		ssize_t r = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
		if (r == 0)
			return 1;
	}
	return 0;
}

void jarvis_plat_write_sock_all(stl_socket_t fd, const char *buf, size_t len)
{
	if (fd == STL_INVALID_SOCKET || !buf) return;
	while (len > 0) {
		ssize_t w = stl_sock_send(fd, buf, len, 0);
		if (w <= 0) break;
		buf += w;
		len -= (size_t)w;
	}
}

int jarvis_plat_read_wav_pcm16(const uint8_t *wav, size_t len,
                               int16_t **out_pcm, size_t *out_samples, int *out_rate)
{
	if (out_pcm) *out_pcm = NULL;
	if (out_samples) *out_samples = 0;
	if (out_rate) *out_rate = 22050;
	if (!wav || len < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
		return -1;

	uint32_t sample_rate = 22050;
	uint16_t channels = 1;
	uint16_t bits_per_sample = 16;
	size_t off = 12;

	while (off + 8 <= len) {
		uint32_t csz = (uint32_t)wav[off + 4] | ((uint32_t)wav[off + 5] << 8) |
		               ((uint32_t)wav[off + 6] << 16) | ((uint32_t)wav[off + 7] << 24);
		size_t body = off + 8;
		if (memcmp(wav + off, "fmt ", 4) == 0 && body + 16 <= len && csz >= 16) {
			channels = (uint16_t)wav[body + 2] | ((uint16_t)wav[body + 3] << 8);
			sample_rate = (uint32_t)wav[body + 4] | ((uint32_t)wav[body + 5] << 8) |
			              ((uint32_t)wav[body + 6] << 16) | ((uint32_t)wav[body + 7] << 24);
			bits_per_sample = (uint16_t)wav[body + 14] | ((uint16_t)wav[body + 15] << 8);
		} else if (memcmp(wav + off, "data", 4) == 0 && bits_per_sample == 16 && body < len) {
			size_t avail = len - body;
			size_t bytes = (csz > 0 && csz <= avail) ? (size_t)csz : avail;
			size_t total_samp = bytes / sizeof(int16_t);
			if (total_samp == 0) return -1;
			if (channels <= 1) {
				int16_t *pcm = malloc(total_samp * sizeof(int16_t));
				if (!pcm) return -1;
				memcpy(pcm, wav + body, total_samp * sizeof(int16_t));
				if (out_pcm) *out_pcm = pcm;
				else free(pcm);
				if (out_samples) *out_samples = total_samp;
			} else {
				size_t frames = total_samp / channels;
				if (frames == 0) return -1;
				int16_t *pcm = malloc(frames * sizeof(int16_t));
				if (!pcm) return -1;
				const int16_t *src = (const int16_t *)(wav + body);
				for (size_t i = 0; i < frames; i++) {
					int32_t sum = 0;
					for (uint16_t c = 0; c < channels; c++)
						sum += src[i * channels + c];
					pcm[i] = (int16_t)(sum / channels);
				}
				if (out_pcm) *out_pcm = pcm;
				else free(pcm);
				if (out_samples) *out_samples = frames;
			}
			if (out_rate)
				*out_rate = (sample_rate >= 8000 && sample_rate <= 96000) ? (int)sample_rate : 22050;
			return 0;
		}
		size_t step = 8 + (size_t)csz + ((csz & 1U) ? 1U : 0U);
		if (step < 8 || off + step > len) break;
		off += step;
	}
	return -1;
}
