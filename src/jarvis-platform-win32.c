#define WIN32_LEAN_AND_MEAN
#include "jarvis-platform.h"

#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct jarvis_plat_lock {
	HANDLE mutex;
	char   pid_path[256];
};

static volatile LONG g_tts_seq = 0;

jarvis_plat_lock_t *jarvis_plat_singleton_acquire(const char *name)
{
	if (!name || !*name) name = "jarvisd";
	char user[64], mtx_name[128], pid_path[256];
	stl_username(user, sizeof(user));
	snprintf(mtx_name, sizeof(mtx_name), "Local\\%s-%s-lock",
	         name, user[0] ? user : "user");

	HANDLE m = CreateMutexA(NULL, FALSE, mtx_name);
	if (!m) return NULL;
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(m);
		return NULL;
	}

	jarvis_plat_state_path(name, ".pid", pid_path, sizeof(pid_path));
	FILE *pfp = fopen(pid_path, "w");
	if (pfp) {
		fprintf(pfp, "%d\n", _getpid());
		fclose(pfp);
	}

	jarvis_plat_lock_t *lk = (jarvis_plat_lock_t *)calloc(1, sizeof(*lk));
	if (!lk) {
		CloseHandle(m);
		return NULL;
	}
	lk->mutex = m;
	snprintf(lk->pid_path, sizeof(lk->pid_path), "%s", pid_path);
	return lk;
}

void jarvis_plat_singleton_release(jarvis_plat_lock_t *lk)
{
	if (!lk) return;
	if (lk->pid_path[0])
		_unlink(lk->pid_path);
	if (lk->mutex)
		CloseHandle(lk->mutex);
	free(lk);
}

int jarvis_plat_state_path(const char *prefix, const char *suffix, char *out, size_t cap)
{
	if (!out || cap == 0) return -1;
	char tdir[256], user[64];
	stl_temp_dir(tdir, sizeof(tdir));
	stl_username(user, sizeof(user));
	int n = snprintf(out, cap, "%s\\%s-%s%s",
	                 tdir[0] ? tdir : ".",
	                 (prefix && *prefix) ? prefix : "jarvis",
	                 user[0] ? user : "user",
	                 suffix ? suffix : "");
	return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int jarvis_plat_data_file(const char *subdir, const char *filename, char *out, size_t cap)
{
	if (!out || cap == 0 || !subdir || !filename) return -1;
	char ddir[512];
	if (stl_data_dir(subdir, ddir, sizeof(ddir)) && ddir[0]) {
		snprintf(out, cap, "%s\\%s", ddir, filename);
		if (jarvis_plat_is_readable(out))
			return 0;
	}
	const char *home = getenv("HOME");
	char hbuf[512];
	if (!home || !*home) {
		stl_home_dir(hbuf, sizeof(hbuf));
		home = hbuf[0] ? hbuf : NULL;
	}
	if (home && *home) {
		snprintf(out, cap, "%s\\.local\\share\\%s\\%s", home, subdir, filename);
		if (jarvis_plat_is_readable(out))
			return 0;
	}
	if (ddir[0]) {
		snprintf(out, cap, "%s\\%s", ddir, filename);
		return 0;
	}
	out[0] = '\0';
	return -1;
}

int jarvis_plat_is_readable(const char *path)
{
	if (!path || !*path) return 0;
	DWORD attr = GetFileAttributesA(path);
	if (attr == INVALID_FILE_ATTRIBUTES) return 0;
	return _access(path, 4) == 0 ? 1 : 0;
}

int jarvis_plat_is_exec(const char *path)
{
	if (!path || !*path) return 0;
	DWORD attr = GetFileAttributesA(path);
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
		return 0;
	return 1;
}

int jarvis_plat_find_in_path(const char *cmd, char *out, size_t cap)
{
	if (!cmd || !*cmd) return -1;
	if (strchr(cmd, '\\') != NULL || strchr(cmd, '/') != NULL) {
		if (jarvis_plat_is_exec(cmd)) {
			if (out && cap > 0) snprintf(out, cap, "%s", cmd);
			return 0;
		}
		return -1;
	}
	char buf[512];
	DWORD n = SearchPathA(NULL, cmd, ".exe", (DWORD)sizeof(buf), buf, NULL);
	if (n == 0 || n >= sizeof(buf))
		n = SearchPathA(NULL, cmd, NULL, (DWORD)sizeof(buf), buf, NULL);
	if (n > 0 && n < sizeof(buf)) {
		if (out && cap > 0) snprintf(out, cap, "%s", buf);
		return 0;
	}
	return -1;
}

int jarvis_plat_notify_osd(const char *title, const char *body, int timeout_ms)
{
	char script[1024];
	int t = timeout_ms > 0 ? timeout_ms : 4500;
	snprintf(script, sizeof(script),
	         "Add-Type -AssemblyName System.Windows.Forms; "
	         "$n = New-Object System.Windows.Forms.NotifyIcon; "
	         "$n.Icon = [System.Drawing.SystemIcons]::Information; "
	         "$n.Visible = $true; "
	         "$n.ShowBalloonTip(%d, $env:JARVIS_N_TITLE, $env:JARVIS_N_BODY, [System.Windows.Forms.ToolTipIcon]::Info); "
	         "Start-Sleep -Milliseconds 1500; $n.Dispose()",
	         t);
	stl_setenv("JARVIS_N_TITLE", (title && *title) ? title : "Jarvis", 1);
	stl_setenv("JARVIS_N_BODY", body ? body : "", 1);
	const char *argv[] = {
		"powershell.exe", "-NoProfile", "-WindowStyle", "Hidden", "-Command", script, NULL
	};
	return stl_spawn_detached(argv);
}

void jarvis_plat_notify_bar(void)
{
	/* No dwmblocks status bar on Windows */
}

int jarvis_plat_lock_screen(const char *override_cmd)
{
	if (override_cmd && *override_cmd) {
		const char *argv[] = { "cmd.exe", "/c", override_cmd, NULL };
		return stl_spawn_detached(argv);
	}
	return LockWorkStation() ? 0 : -1;
}

int jarvis_plat_open_url(const char *url)
{
	if (!url || !*url) return -1;
	HINSTANCE rc = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
	return ((INT_PTR)rc > 32) ? 0 : -1;
}

void jarvis_plat_kbd_lang(char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';

	HWND fg = GetForegroundWindow();
	DWORD tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
	HKL hkl = GetKeyboardLayout(tid);
	LANGID lid = LOWORD((ULONG_PTR)hkl);
	WORD prim = PRIMARYLANGID(lid);
	if (prim == LANG_PORTUGUESE) {
		snprintf(out, cap, "pt");
	} else if (prim == LANG_ENGLISH) {
		snprintf(out, cap, "en");
	} else if (prim == LANG_SPANISH) {
		snprintf(out, cap, "es");
	} else if (prim == LANG_FRENCH) {
		snprintf(out, cap, "fr");
	} else if (prim == LANG_GERMAN) {
		snprintf(out, cap, "de");
	}
}

int16_t *jarvis_plat_tts_fallback_pcm(const char *text, const char *lang_hint,
                                      size_t *out_samples, int *out_rate)
{
	(void)lang_hint;
	if (out_samples) *out_samples = 0;
	if (out_rate) *out_rate = 22050;
	if (!text || !*text) return NULL;

	LONG seq = InterlockedIncrement(&g_tts_seq);
	char suf[64], tmpwav[512];
	snprintf(suf, sizeof(suf), "-sapi-%d-%ld.wav", _getpid(), (long)seq);
	if (jarvis_plat_state_path("jarvis-tts", suf, tmpwav, sizeof(tmpwav)) != 0)
		return NULL;

	stl_setenv("JARVIS_SAPI_TEXT", text, 1);
	stl_setenv("JARVIS_SAPI_WAV", tmpwav, 1);
	const char *script =
		"Add-Type -AssemblyName System.Speech; "
		"$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
		"$s.SetOutputToWaveFile($env:JARVIS_SAPI_WAV); "
		"$s.Speak($env:JARVIS_SAPI_TEXT); "
		"$s.Dispose()";
	const char *argv[] = {
		"powershell.exe", "-NoProfile", "-WindowStyle", "Hidden", "-Command", script, NULL
	};
	stl_proc_t p;
	if (stl_spawn(argv, 0, &p) != 0)
		return NULL;
	int ec = 0;
	stl_proc_wait(&p, &ec, 15000);
	stl_proc_close(&p);

	FILE *fp = fopen(tmpwav, "rb");
	int16_t *pcm = NULL;
	if (fp) {
		fseek(fp, 0, SEEK_END);
		long fsz = ftell(fp);
		rewind(fp);
		if (fsz > 44 && fsz < 32 * 1024 * 1024) {
			uint8_t *wav = (uint8_t *)malloc((size_t)fsz);
			if (wav) {
				size_t rd = fread(wav, 1, (size_t)fsz, fp);
				if (rd == (size_t)fsz)
					(void)jarvis_plat_read_wav_pcm16(wav, rd, &pcm, out_samples, out_rate);
				free(wav);
			}
		}
		fclose(fp);
	}
	_unlink(tmpwav);
	return pcm;
}

const char *jarvis_plat_espeak_lib_name(void)
{
	return "libespeak-ng.dll";
}

const char *jarvis_plat_ort_lib_name(int alt_index)
{
	return alt_index == 0 ? "onnxruntime.dll" : "libonnxruntime.dll";
}

void jarvis_plat_suppress_stderr_begin(int *saved_fd)
{
	if (!saved_fd) return;
	*saved_fd = _dup(2);
	int devnull = _open("NUL", _O_WRONLY);
	if (devnull >= 0) {
		_dup2(devnull, 2);
		_close(devnull);
	}
}

void jarvis_plat_suppress_stderr_end(int saved_fd)
{
	if (saved_fd >= 0) {
		_dup2(saved_fd, 2);
		_close(saved_fd);
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
		int r = recv(fd, &peek, 1, MSG_PEEK);
		if (r == 0) return 1;
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
				int16_t *pcm = (int16_t *)malloc(total_samp * sizeof(int16_t));
				if (!pcm) return -1;
				memcpy(pcm, wav + body, total_samp * sizeof(int16_t));
				if (out_pcm) *out_pcm = pcm;
				else free(pcm);
				if (out_samples) *out_samples = total_samp;
			} else {
				size_t frames = total_samp / channels;
				if (frames == 0) return -1;
				int16_t *pcm = (int16_t *)malloc(frames * sizeof(int16_t));
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
