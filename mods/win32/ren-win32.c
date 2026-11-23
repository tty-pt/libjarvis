/**
 * @file ren-win32.c
 * @brief Standalone ren extension for Win32 SendInput, windows, clipboard, and desktop context.
 */

#define WIN32_LEAN_AND_MEAN
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <psapi.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#include <ttypt/stele.h>
#include "ttypt/libren.h"
#include <ttypt/xy.h>
#include "../../include/ttypt/desktop-core.h"

typedef struct {
	HWND  hwnd;
	DWORD pid;
	char  cls[128];
	char  title[256];
	int   x, y, w, h;
	bool  focused;
	bool  visible;
} w32_win_t;

static void wchar_to_utf8(const WCHAR *ws, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!ws || !*ws) return;
	char tmp[1024];
	int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, tmp, (int)sizeof(tmp), NULL, NULL);
	if (n <= 0) {
		n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, (int)out_sz, NULL, NULL);
		if (n <= 0) out[out_sz - 1] = '\0';
	} else {
		size_t copy_len = strlen(tmp);
		if (copy_len >= out_sz) copy_len = out_sz - 1;
		memcpy(out, tmp, copy_len);
		out[copy_len] = '\0';
	}
	desktop_core_utf8_trim_tail(out);
}

static void json_esc(const char *in, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	if (!in) {
		out[0] = '\0';
		return;
	}
	size_t w = 0;
	for (size_t i = 0; in[i] && w + 2 < out_sz; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '"' || c == '\\') {
			out[w++] = '\\';
			out[w++] = (char)c;
		} else if (c == '\n') {
			out[w++] = '\\';
			out[w++] = 'n';
		} else if (c == '\r') {
			out[w++] = '\\';
			out[w++] = 'r';
		} else if (c == '\t') {
			out[w++] = '\\';
			out[w++] = 't';
		} else if (c >= 0x20) {
			out[w++] = (char)c;
		}
	}
	out[w] = '\0';
	desktop_core_utf8_trim_tail(out);
}

static void resolve_proc_exe(DWORD pid, char *out, size_t cap)
{
	out[0] = '\0';
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h) return;
	WCHAR wpath[MAX_PATH] = {0};
	DWORD sz = MAX_PATH;
	if (QueryFullProcessImageNameW(h, 0, wpath, &sz) && sz > 0) {
		char path[1024];
		wchar_to_utf8(wpath, path, sizeof(path));
		const char *base = strrchr(path, '\\');
		snprintf(out, cap, "%s", base ? base + 1 : path);
		char *dot = strrchr(out, '.');
		if (dot && _stricmp(dot, ".exe") == 0) *dot = '\0';
	}
	CloseHandle(h);
}

static void fill_win_info(HWND hwnd, HWND fg, w32_win_t *w)
{
	memset(w, 0, sizeof(*w));
	w->hwnd = hwnd;
	w->focused = (hwnd == fg);
	w->visible = !IsIconic(hwnd);
	GetWindowThreadProcessId(hwnd, &w->pid);
	resolve_proc_exe(w->pid, w->cls, sizeof(w->cls));
	if (!w->cls[0]) {
		WCHAR wcls[128] = {0};
		if (GetClassNameW(hwnd, wcls, (int)(sizeof(wcls) / sizeof(wcls[0]))) > 0)
			wchar_to_utf8(wcls, w->cls, sizeof(w->cls));
	}
	WCHAR wtitle[256] = {0};
	if (GetWindowTextW(hwnd, wtitle, (int)(sizeof(wtitle) / sizeof(wtitle[0]))) > 0)
		wchar_to_utf8(wtitle, w->title, sizeof(w->title));
	RECT rc = {0, 0, 0, 0};
	if (GetWindowRect(hwnd, &rc)) {
		w->x = (int)rc.left;
		w->y = (int)rc.top;
		w->w = (int)(rc.right - rc.left);
		w->h = (int)(rc.bottom - rc.top);
	}
}

typedef struct {
	w32_win_t *arr;
	size_t     cap;
	size_t     count;
	HWND       fg;
} w32_enum_t;

static BOOL CALLBACK enum_win_cb(HWND hwnd, LPARAM lp)
{
	w32_enum_t *st = (w32_enum_t *)lp;
	if (!IsWindowVisible(hwnd)) return TRUE;
	LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
	if (ex & WS_EX_TOOLWINDOW) return TRUE;
	int tlen = GetWindowTextLengthW(hwnd);
	if (tlen <= 0) return TRUE;
	if (st->count >= st->cap) return FALSE;
	fill_win_info(hwnd, st->fg, &st->arr[st->count]);
	st->count++;
	return TRUE;
}

static size_t query_windows(w32_win_t *arr, size_t cap, w32_win_t *out_foc)
{
	HWND fg = GetForegroundWindow();
	if (out_foc) {
		if (fg) fill_win_info(fg, fg, out_foc);
		else memset(out_foc, 0, sizeof(*out_foc));
	}
	if (!arr || cap == 0) return 0;
	w32_enum_t st = { arr, cap, 0, fg };
	EnumWindows(enum_win_cb, (LPARAM)&st);
	return st.count;
}

static WORD vk_for_name(const char *tok, BYTE *out_shift)
{
	if (out_shift) *out_shift = 0;
	if (!tok || !*tok) return 0;
	if (_stricmp(tok, "Return") == 0 || _stricmp(tok, "Enter") == 0) return VK_RETURN;
	if (_stricmp(tok, "Tab") == 0) return VK_TAB;
	if (_stricmp(tok, "space") == 0) return VK_SPACE;
	if (_stricmp(tok, "BackSpace") == 0 || _stricmp(tok, "Backspace") == 0) return VK_BACK;
	if (_stricmp(tok, "Delete") == 0) return VK_DELETE;
	if (_stricmp(tok, "Escape") == 0 || _stricmp(tok, "Esc") == 0) return VK_ESCAPE;
	if (_stricmp(tok, "Left") == 0) return VK_LEFT;
	if (_stricmp(tok, "Right") == 0) return VK_RIGHT;
	if (_stricmp(tok, "Up") == 0) return VK_UP;
	if (_stricmp(tok, "Down") == 0) return VK_DOWN;
	if (_stricmp(tok, "Home") == 0) return VK_HOME;
	if (_stricmp(tok, "End") == 0) return VK_END;
	if (_stricmp(tok, "Page_Up") == 0 || _stricmp(tok, "Prior") == 0) return VK_PRIOR;
	if (_stricmp(tok, "Page_Down") == 0 || _stricmp(tok, "Next") == 0) return VK_NEXT;
	if (_stricmp(tok, "MediaPlayPause") == 0 || _stricmp(tok, "XF86AudioPlay") == 0) return VK_MEDIA_PLAY_PAUSE;
	if (_stricmp(tok, "MediaNext") == 0 || _stricmp(tok, "MediaNextTrack") == 0 || _stricmp(tok, "XF86AudioNext") == 0) return VK_MEDIA_NEXT_TRACK;
	if (_stricmp(tok, "MediaPrev") == 0 || _stricmp(tok, "MediaPrevTrack") == 0 || _stricmp(tok, "XF86AudioPrev") == 0) return VK_MEDIA_PREV_TRACK;
	if (_stricmp(tok, "VolumeUp") == 0 || _stricmp(tok, "XF86AudioRaiseVolume") == 0) return VK_VOLUME_UP;
	if (_stricmp(tok, "VolumeDown") == 0 || _stricmp(tok, "XF86AudioLowerVolume") == 0) return VK_VOLUME_DOWN;
	if (_stricmp(tok, "VolumeMute") == 0 || _stricmp(tok, "XF86AudioMute") == 0) return VK_VOLUME_MUTE;
	if ((tok[0] == 'F' || tok[0] == 'f') && isdigit((unsigned char)tok[1])) {
		int fn = atoi(tok + 1);
		if (fn >= 1 && fn <= 24) return (WORD)(VK_F1 + fn - 1);
	}
	WCHAR wtok[4] = {0};
	int wn = MultiByteToWideChar(CP_UTF8, 0, tok, -1, wtok, 4);
	if (wn == 2 && wtok[0] != 0) {
		if (wtok[0] < 128 && isalpha((unsigned char)wtok[0])) return (WORD)toupper((unsigned char)wtok[0]);
		if (wtok[0] < 128 && isdigit((unsigned char)wtok[0])) return (WORD)wtok[0];
		SHORT vk = VkKeyScanW(wtok[0]);
		if (vk != -1) {
			if (out_shift) *out_shift = (BYTE)((vk >> 8) & 0x07);
			return (WORD)(vk & 0xFF);
		}
	}
	return 0;
}

static void add_mod_unique(WORD *mods, int *nmods, int max_mods, WORD m)
{
	for (int i = 0; i < *nmods; i++) {
		if (mods[i] == m) return;
	}
	if (*nmods < max_mods) mods[(*nmods)++] = m;
}

static void win32_press_key_combo(const char *combo)
{
	if (!combo || !*combo) return;
	char copy[128];
	snprintf(copy, sizeof(copy), "%s", combo);

	WORD mods[8] = {0};
	int nmods = 0;
	WORD vk = 0;
	BYTE implied_shift = 0;

	char *tok = copy;
	while (tok && *tok) {
		char *plus = strchr(tok, '+');
		if (plus) *plus++ = '\0';
		while (*tok == ' ') tok++;
		if (_stricmp(tok, "ctrl") == 0 || _stricmp(tok, "control") == 0) {
			add_mod_unique(mods, &nmods, 8, VK_CONTROL);
		} else if (_stricmp(tok, "super") == 0 || _stricmp(tok, "win") == 0 ||
		           _stricmp(tok, "meta") == 0) {
			add_mod_unique(mods, &nmods, 8, VK_LWIN);
		} else if (_stricmp(tok, "alt") == 0 || _stricmp(tok, "mod1") == 0) {
			add_mod_unique(mods, &nmods, 8, VK_MENU);
		} else if (_stricmp(tok, "shift") == 0) {
			add_mod_unique(mods, &nmods, 8, VK_SHIFT);
		} else {
			vk = vk_for_name(tok, &implied_shift);
		}
		tok = plus;
	}
	if (vk == 0) return;
	if (implied_shift & 1) add_mod_unique(mods, &nmods, 8, VK_SHIFT);
	if (implied_shift & 2) add_mod_unique(mods, &nmods, 8, VK_CONTROL);
	if (implied_shift & 4) add_mod_unique(mods, &nmods, 8, VK_MENU);

	INPUT in[18];
	memset(in, 0, sizeof(in));
	UINT idx = 0;
	for (int i = 0; i < nmods; i++) {
		in[idx].type = INPUT_KEYBOARD;
		in[idx].ki.wVk = mods[i];
		idx++;
	}
	in[idx].type = INPUT_KEYBOARD;
	in[idx].ki.wVk = vk;
	idx++;
	in[idx].type = INPUT_KEYBOARD;
	in[idx].ki.wVk = vk;
	in[idx].ki.dwFlags = KEYEVENTF_KEYUP;
	idx++;
	for (int i = nmods - 1; i >= 0; i--) {
		in[idx].type = INPUT_KEYBOARD;
		in[idx].ki.wVk = mods[i];
		in[idx].ki.dwFlags = KEYEVENTF_KEYUP;
		idx++;
	}
	SendInput(idx, in, sizeof(INPUT));
}

static void win32_type_text(const char *text, bool submit)
{
	if (text && *text) {
		int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
		if (wlen > 1) {
			WCHAR *wbuf = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
			if (wbuf) {
				MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, wlen);
				for (int i = 0; wbuf[i]; i++) {
					INPUT ev[2];
					memset(ev, 0, sizeof(ev));
					ev[0].type = INPUT_KEYBOARD;
					ev[0].ki.wScan = (WORD)wbuf[i];
					ev[0].ki.dwFlags = KEYEVENTF_UNICODE;
					ev[1].type = INPUT_KEYBOARD;
					ev[1].ki.wScan = (WORD)wbuf[i];
					ev[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
					SendInput(2, ev, sizeof(INPUT));
				}
				free(wbuf);
			}
		}
	}
	if (submit) {
		Sleep(12);
		win32_press_key_combo("Return");
	}
}

static int tool_input(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: input <submit <text> | type <text> | key <combo> | click <x y [btn] | center> | scroll <up|down|left|right> [amount]>");
		return -1;
	}

	if (strncasecmp(args, "submit ", 7) == 0 || strncasecmp(args, "enter ", 6) == 0) {
		const char *txt = (args[0] == 's' || args[0] == 'S') ? (args + 7) : (args + 6);
		while (*txt == ' ') txt++;
		win32_type_text(txt, true);
		char buf[256];
		snprintf(buf, sizeof(buf), "typed+enter: %.120s", txt);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "type ", 5) == 0 || strncasecmp(args, "fill ", 5) == 0) {
		const char *txt = args + 5;
		while (*txt == ' ') txt++;
		win32_type_text(txt, false);
		char buf[256];
		snprintf(buf, sizeof(buf), "typed: %.120s", txt);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "key ", 4) == 0) {
		const char *k = args + 4;
		while (*k == ' ') k++;
		win32_press_key_combo(*k ? k : "Return");
		Sleep(20);
		char buf[160];
		snprintf(buf, sizeof(buf), "key: %s", *k ? k : "Return");
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "click", 5) == 0) {
		const char *rest = args + 5;
		while (*rest == ' ') rest++;
		int x = 0, y = 0, btn = 1;
		char extra = '\0';
		if (*rest && sscanf(rest, "%d %d %d %c", &x, &y, &btn, &extra) == 3) {
		} else if (*rest && sscanf(rest, "%d %d %c", &x, &y, &extra) == 2) {
		} else if (*rest && sscanf(rest, "%d,%d %c", &x, &y, &extra) == 2) {
		} else if (!*rest || strcasecmp(rest, "center") == 0) {
			w32_win_t foc;
			query_windows(NULL, 0, &foc);
			if (foc.w > 0 && foc.h > 0) {
				x = foc.x + foc.w / 2;
				y = foc.y + foc.h / 2;
			} else {
				x = GetSystemMetrics(SM_CXSCREEN) / 2;
				y = GetSystemMetrics(SM_CYSCREEN) / 2;
			}
		} else {
			*out_result = strdup("Error: input click accepts '<x> <y> [btn]' or 'center'");
			return -1;
		}
		int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
		int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
		int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		if (vw <= 0) vw = 1;
		if (vh <= 0) vh = 1;
		LONG nx = (LONG)(((double)(x - vx) * 65535.0) / vw);
		LONG ny = (LONG)(((double)(y - vy) * 65535.0) / vh);

		DWORD down_f = (btn == 3) ? MOUSEEVENTF_RIGHTDOWN : ((btn == 2) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_LEFTDOWN);
		DWORD up_f   = (btn == 3) ? MOUSEEVENTF_RIGHTUP   : ((btn == 2) ? MOUSEEVENTF_MIDDLEUP   : MOUSEEVENTF_LEFTUP);
		INPUT ev[2];
		memset(ev, 0, sizeof(ev));
		ev[0].type = INPUT_MOUSE;
		ev[0].mi.dx = nx;
		ev[0].mi.dy = ny;
		ev[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | down_f;
		ev[1].type = INPUT_MOUSE;
		ev[1].mi.dx = nx;
		ev[1].mi.dy = ny;
		ev[1].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | up_f;
		SendInput(2, ev, sizeof(INPUT));

		char buf[128];
		snprintf(buf, sizeof(buf), "click at (%d,%d) button %d", x, y, btn);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "scroll", 6) == 0) {
		const char *rest = args + 6;
		while (*rest == ' ') rest++;
		char dir[32] = "down";
		int amt = 5;
		if (*rest) sscanf(rest, "%31s %d", dir, &amt);
		if (amt <= 0) amt = 5;
		if (amt > 20) amt = 20;
		INPUT ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = INPUT_MOUSE;
		if (strcasecmp(dir, "left") == 0) {
			ev.mi.dwFlags = MOUSEEVENTF_HWHEEL;
			ev.mi.mouseData = (DWORD)(-amt * WHEEL_DELTA);
		} else if (strcasecmp(dir, "right") == 0) {
			ev.mi.dwFlags = MOUSEEVENTF_HWHEEL;
			ev.mi.mouseData = (DWORD)(amt * WHEEL_DELTA);
		} else if (strcasecmp(dir, "up") == 0) {
			ev.mi.dwFlags = MOUSEEVENTF_WHEEL;
			ev.mi.mouseData = (DWORD)(amt * WHEEL_DELTA);
		} else {
			ev.mi.dwFlags = MOUSEEVENTF_WHEEL;
			ev.mi.mouseData = (DWORD)(-amt * WHEEL_DELTA);
		}
		SendInput(1, &ev, sizeof(INPUT));
		char buf[128];
		snprintf(buf, sizeof(buf), "scrolled %s x%d", dir, amt);
		*out_result = strdup(buf);
		return 0;
	}

	*out_result = strdup("Error: usage: input <submit <text> | type <text> | key <combo> | click <x y [btn] | center> | scroll <up|down|left|right> [amount]>");
	return -1;
}

static bool focus_hwnd(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd)) return false;
	if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
	HWND fg = GetForegroundWindow();
	DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
	DWORD cur_tid = GetCurrentThreadId();
	if (fg_tid && fg_tid != cur_tid)
		AttachThreadInput(cur_tid, fg_tid, TRUE);
	BOOL ok = SetForegroundWindow(hwnd);
	BringWindowToTop(hwnd);
	if (fg_tid && fg_tid != cur_tid)
		AttachThreadInput(cur_tid, fg_tid, FALSE);
	return ok ? true : false;
}

static int tool_window(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (args && strcasecmp(args, "focused") == 0) {
		w32_win_t foc;
		query_windows(NULL, 0, &foc);
		if (!foc.hwnd) {
			*out_result = strdup("{\"id\":\"\",\"pid\":0,\"class\":\"\",\"title\":\"\",\"x\":0,\"y\":0,\"w\":0,\"h\":0}");
			return 0;
		}
		int sw = GetSystemMetrics(SM_CXSCREEN);
		int sh = GetSystemMetrics(SM_CYSCREEN);
		char ec[160], et[300], buf[700];
		json_esc(foc.cls, ec, sizeof(ec));
		json_esc(foc.title, et, sizeof(et));
		snprintf(buf, sizeof(buf),
		         "{\"id\":\"0x%llx\",\"pid\":%lu,\"class\":\"%s\",\"title\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"sw\":%d,\"sh\":%d}",
		         (unsigned long long)(uintptr_t)foc.hwnd,
		         (unsigned long)foc.pid, ec, et, foc.x, foc.y, foc.w, foc.h, sw, sh);
		*out_result = strdup(buf);
		return 0;
	}

	if (args && strncasecmp(args, "focus ", 6) == 0) {
		const char *ident = args + 6;
		while (*ident == ' ') ident++;
		if (strncmp(ident, "0x", 2) == 0 || strncmp(ident, "0X", 2) == 0) {
			uintptr_t val = (uintptr_t)strtoull(ident, NULL, 16);
			bool ok = focus_hwnd((HWND)val);
			*out_result = strdup(ok ? "focused window" : "Error: failed to focus window");
			return ok ? 0 : -1;
		}
		w32_win_t wins[48];
		size_t n = query_windows(wins, 48, NULL);
		for (size_t i = 0; i < n; i++) {
			if (desktop_core_str_contains_ci(wins[i].cls, ident) ||
			    desktop_core_str_contains_ci(wins[i].title, ident)) {
				bool ok = focus_hwnd(wins[i].hwnd);
				char buf[256];
				snprintf(buf, sizeof(buf), "%s %s: %.64s",
				         ok ? "focused" : "failed to focus",
				         wins[i].cls, wins[i].title);
				*out_result = strdup(buf);
				return ok ? 0 : -1;
			}
		}
		*out_result = strdup("Error: window not found");
		return -1;
	}

	if (args && strncasecmp(args, "close ", 6) == 0) {
		const char *ident = args + 6;
		while (*ident == ' ') ident++;
		HWND target = NULL;
		if (strncmp(ident, "0x", 2) == 0 || strncmp(ident, "0X", 2) == 0) {
			target = (HWND)(uintptr_t)strtoull(ident, NULL, 16);
		} else {
			w32_win_t wins[48];
			size_t n = query_windows(wins, 48, NULL);
			for (size_t i = 0; i < n; i++) {
				if (desktop_core_str_contains_ci(wins[i].cls, ident) ||
				    desktop_core_str_contains_ci(wins[i].title, ident)) {
					target = wins[i].hwnd;
					break;
				}
			}
		}
		if (!target || !IsWindow(target)) {
			*out_result = strdup("Error: window not found");
			return -1;
		}
		BOOL ok = PostMessageA(target, WM_CLOSE, 0, 0);
		*out_result = strdup(ok ? "closed window" : "Error: failed to close window");
		return ok ? 0 : -1;
	}

	w32_win_t wins[48];
	size_t n = query_windows(wins, 48, NULL);
	char out[4096];
	size_t off = 0;
	off += (size_t)snprintf(out + off, sizeof(out) - off, "{\"windows\":[");
	for (size_t i = 0; i < n && off + 200 < sizeof(out); i++) {
		char ec[160], et[256];
		json_esc(wins[i].cls, ec, sizeof(ec));
		json_esc(wins[i].title, et, sizeof(et));
		off += (size_t)snprintf(out + off, sizeof(out) - off,
		                        "%s{\"id\":\"0x%llx\",\"class\":\"%s\",\"title\":\"%s\",\"tag\":1,\"visible\":%s,\"focused\":%s}",
		                        i > 0 ? "," : "",
		                        (unsigned long long)(uintptr_t)wins[i].hwnd,
		                        ec, et,
		                        wins[i].visible ? "true" : "false",
		                        wins[i].focused ? "true" : "false");
	}
	snprintf(out + off, sizeof(out) - off, "]}");
	*out_result = strdup(out);
	return 0;
}

static int tool_clipboard(const char *args, char **out_result)
{
	(void)args;
	char clip[384] = {0};
	if (OpenClipboard(NULL)) {
		HANDLE h = GetClipboardData(CF_UNICODETEXT);
		if (h) {
			const WCHAR *ws = (const WCHAR *)GlobalLock(h);
			if (ws) {
				WCHAR wclip[256];
				size_t wlen = 0;
				while (wlen < 255 && ws[wlen]) {
					wclip[wlen] = ws[wlen];
					wlen++;
				}
				if (wlen == 255 && wclip[wlen - 1] >= 0xD800 && wclip[wlen - 1] <= 0xDBFF)
					wlen--;
				wclip[wlen] = 0;
				wchar_to_utf8(wclip, clip, sizeof(clip));
				GlobalUnlock(h);
			}
		}
		CloseClipboard();
	}
	char e_clip[800], buf[1200];
	json_esc(clip, e_clip, sizeof(e_clip));
	snprintf(buf, sizeof(buf), "{\"primary_selection\":\"\",\"clipboard\":\"%s\"}", e_clip);
	*out_result = strdup(buf);
	return 0;
}

static int tool_open_terminal(const char *args, char **out_result)
{
	(void)args;
	const char *wt[] = { "wt.exe", NULL };
	if (stl_spawn_detached(wt) == 0) {
		*out_result = strdup("opened Windows Terminal");
		return 0;
	}
	const char *ps[] = { "powershell.exe", NULL };
	int rc = stl_spawn_detached(ps);
	*out_result = strdup(rc == 0 ? "opened PowerShell" : "Error: failed to open terminal");
	return rc == 0 ? 0 : -1;
}

static char *win32_context_provider(void)
{
	w32_win_t wins[24], foc;
	size_t n = query_windows(wins, 24, &foc);
	if (n == 0 && !foc.cls[0] && !foc.title[0]) return NULL;

	char wlist[512] = {0};
	size_t woff = 0;
	for (size_t i = 0; i < n && woff + 64 < sizeof(wlist); i++) {
		char short_t[41];
		desktop_core_utf8_copy(short_t, sizeof(short_t), wins[i].title);
		woff += (size_t)snprintf(wlist + woff, sizeof(wlist) - woff,
		                         "%s%s:'%s'",
		                         i > 0 ? ", " : "",
		                         wins[i].cls, short_t);
	}
	char foc_t[81];
	desktop_core_utf8_copy(foc_t, sizeof(foc_t), foc.title);
	char buf[1280];
	snprintf(buf, sizeof(buf),
	         "win32_focused='%s' title='%s' windows=[%s]",
	         foc.cls, foc_t, wlist);
	return strdup(buf);
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("input", "input <submit <text> | type <text> | key <combo> | click <x y [btn] | center> | scroll <up|down|left|right> [amount]>", tool_input);
	ren_register_tool("window", "window [focused | focus <id_or_title> | close <id>]", tool_window);
	ren_register_tool("clipboard", "clipboard", tool_clipboard);
	ren_register_tool("open_terminal", "open_terminal", tool_open_terminal);
	ren_register_context_provider(win32_context_provider);
}
