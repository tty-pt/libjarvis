#ifndef JARVIS_X11_CORE_H
#define JARVIS_X11_CORE_H

/**
 * @file x11_core.h
 * @brief Self-contained pure X11 / EWMH / XTest core (zero libjarvis/libren/dwm deps).
 */

#include <ctype.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

typedef struct {
	char          id[32];
	char          wm_class[128];
	char          title[256];
	unsigned long pid;
	int           tag;
	bool          visible;
	bool          focused;
} x11_win_info_t;

static double x11_core_now_sec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static bool x11_core_str_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle) return false;
	return strcasestr(haystack, needle) != NULL;
}

static bool x11_core_is_terminal_class(const char *wm_class)
{
	static const char *const TERMS[] = {
		"st-256color", "st", "xterm", "alacritty", "kitty", "foot", "wezterm", "urxvt", "ghostty", NULL
	};
	if (!wm_class || !*wm_class) return false;
	for (size_t i = 0; TERMS[i]; i++) {
		if (x11_core_str_ci(wm_class, TERMS[i])) return true;
	}
	return false;
}

static bool x11_core_is_browser_class(const char *wm_class)
{
	if (!wm_class || !*wm_class) return false;
	return x11_core_str_ci(wm_class, "firefox") ||
	       x11_core_str_ci(wm_class, "chromium") ||
	       x11_core_str_ci(wm_class, "chrome") ||
	       x11_core_str_ci(wm_class, "brave") ||
	       x11_core_str_ci(wm_class, "qutebrowser");
}

static __attribute__((unused)) void x11_core_json_escape(const char *src, char *dst, size_t dst_sz)
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

static unsigned long x11_core_get_cardinal_prop(Display *dpy, Window w, const char *atom_name, unsigned long def_val)
{
	Atom prop_atom = XInternAtom(dpy, atom_name, False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	unsigned long val = def_val;

	if (XGetWindowProperty(dpy, w, prop_atom, 0, 1, False, XA_CARDINAL,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) {
			val = *(unsigned long *)prop;
		}
		XFree(prop);
	}
	return val;
}

static int x11_core_mask_to_tag(unsigned int mask)
{
	for (int t = 1; t <= 9; t++) {
		if (mask & (1U << (t - 1))) return t;
	}
	return 1;
}

static void x11_core_get_win_title(Display *dpy, Window w, char *buf, size_t len)
{
	Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop = NULL;

	if (!buf || len == 0) return;
	buf[0] = '\0';
	if (XGetWindowProperty(dpy, w, net_wm_name, 0, 256, False, utf8,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		snprintf(buf, len, "%s", (char *)prop);
		XFree(prop);
		return;
	}
	if (XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 256, False, AnyPropertyType,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		snprintf(buf, len, "%s", (char *)prop);
		XFree(prop);
	}
}

static void x11_core_get_win_class(Display *dpy, Window w, char *buf, size_t len)
{
	XClassHint ch = { NULL, NULL };
	if (!buf || len == 0) return;
	buf[0] = '\0';
	if (XGetClassHint(dpy, w, &ch)) {
		snprintf(buf, len, "%s", ch.res_class ? ch.res_class : (ch.res_name ? ch.res_name : "-"));
		if (ch.res_class) XFree(ch.res_class);
		if (ch.res_name) XFree(ch.res_name);
	} else {
		snprintf(buf, len, "-");
	}
}

static size_t x11_core_query_windows(
	x11_win_info_t *clients, size_t max_clients,
	x11_win_info_t *out_focused, x11_win_info_t *out_other)
{
	if (out_focused) memset(out_focused, 0, sizeof(*out_focused));
	if (out_other) memset(out_other, 0, sizeof(*out_other));
	if (!clients || max_clients == 0) return 0;

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return 0;
	Window root = DefaultRootWindow(dpy);

	Atom net_clients = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	Atom net_active  = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	Window active_win = 0;
	unsigned int cur_mask = (unsigned int)x11_core_get_cardinal_prop(dpy, root, "_DWM_CUR_TAG", 0UL);
	bool has_dwm_tags = (cur_mask != 0);
	if (!has_dwm_tags) cur_mask = 1U;

	if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) active_win = *(Window *)prop;
		XFree(prop);
		prop = NULL;
	}

	if (active_win && out_focused) {
		snprintf(out_focused->id, sizeof(out_focused->id), "0x%08lx", (unsigned long)active_win);
		x11_core_get_win_class(dpy, active_win, out_focused->wm_class, sizeof(out_focused->wm_class));
		x11_core_get_win_title(dpy, active_win, out_focused->title, sizeof(out_focused->title));
		out_focused->pid = x11_core_get_cardinal_prop(dpy, active_win, "_NET_WM_PID", 0UL);
		if (has_dwm_tags) {
			unsigned int c_mask = (unsigned int)x11_core_get_cardinal_prop(dpy, active_win, "_DWM_CLIENT_TAGS", cur_mask);
			out_focused->tag = x11_core_mask_to_tag(c_mask);
			out_focused->visible = (c_mask & cur_mask) != 0;
		} else {
			out_focused->tag = (int)x11_core_get_cardinal_prop(dpy, active_win, "_NET_WM_DESKTOP", 0UL) + 1;
			out_focused->visible = true;
		}
		out_focused->focused = true;
	}

	size_t count = 0;
	nitems = 0;
	if (XGetWindowProperty(dpy, root, net_clients, 0, 1024, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		Window *wins = (Window *)prop;
		x11_win_info_t fallback_other;
		memset(&fallback_other, 0, sizeof(fallback_other));
		bool has_other_on_cur = false;

		for (unsigned long i = 0; i < nitems && count < max_clients; i++) {
			x11_win_info_t *cw = &clients[count++];
			memset(cw, 0, sizeof(*cw));
			snprintf(cw->id, sizeof(cw->id), "0x%08lx", (unsigned long)wins[i]);
			x11_core_get_win_class(dpy, wins[i], cw->wm_class, sizeof(cw->wm_class));
			x11_core_get_win_title(dpy, wins[i], cw->title, sizeof(cw->title));
			cw->pid = x11_core_get_cardinal_prop(dpy, wins[i], "_NET_WM_PID", 0UL);
			if (has_dwm_tags) {
				unsigned int c_mask = (unsigned int)x11_core_get_cardinal_prop(dpy, wins[i], "_DWM_CLIENT_TAGS", cur_mask);
				cw->tag = x11_core_mask_to_tag(c_mask);
				cw->visible = (c_mask & cur_mask) != 0;
			} else {
				XWindowAttributes wa;
				cw->tag = (int)x11_core_get_cardinal_prop(dpy, wins[i], "_NET_WM_DESKTOP", 0UL) + 1;
				cw->visible = (XGetWindowAttributes(dpy, wins[i], &wa) && wa.map_state == IsViewable);
			}
			cw->focused = (wins[i] == active_win);

			if (out_other && wins[i] != active_win) {
				bool cur_is_term = out_focused && x11_core_is_terminal_class(out_focused->wm_class);
				bool cand_is_term = x11_core_is_terminal_class(cw->wm_class);
				if (cw->visible) {
					if (!has_other_on_cur || (cur_is_term && !cand_is_term && x11_core_is_terminal_class(out_other->wm_class))) {
						*out_other = *cw;
						has_other_on_cur = true;
					}
				} else if (!fallback_other.id[0]) {
					fallback_other = *cw;
				}
			}
		}
		if (out_other && !has_other_on_cur && fallback_other.id[0]) {
			*out_other = fallback_other;
		}
		XFree(prop);
	}

	XCloseDisplay(dpy);
	return count;
}

static bool x11_core_get_focused(x11_win_info_t *out_foc)
{
	if (!out_foc) return false;
	memset(out_foc, 0, sizeof(*out_foc));
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	Window root = DefaultRootWindow(dpy);
	Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	Window active_win = 0;

	if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) active_win = *(Window *)prop;
		XFree(prop);
	}
	if (active_win) {
		snprintf(out_foc->id, sizeof(out_foc->id), "0x%08lx", (unsigned long)active_win);
		x11_core_get_win_class(dpy, active_win, out_foc->wm_class, sizeof(out_foc->wm_class));
		x11_core_get_win_title(dpy, active_win, out_foc->title, sizeof(out_foc->title));
		out_foc->pid = x11_core_get_cardinal_prop(dpy, active_win, "_NET_WM_PID", 0UL);
		out_foc->visible = true;
		out_foc->focused = true;
	}
	XCloseDisplay(dpy);
	return active_win != 0;
}

static bool x11_core_wait_focused(const char *target_id_or_class, double timeout_sec)
{
	if (!target_id_or_class || !*target_id_or_class) return false;
	double deadline = x11_core_now_sec() + timeout_sec;
	while (x11_core_now_sec() < deadline) {
		x11_win_info_t foc;
		if (x11_core_get_focused(&foc)) {
			if (strcasecmp(foc.id, target_id_or_class) == 0 ||
			    x11_core_str_ci(foc.wm_class, target_id_or_class) ||
			    x11_core_str_ci(foc.title, target_id_or_class)) {
				return true;
			}
		}
		usleep(35000);
	}
	return false;
}

static void x11_core_get_geometry(const char *wid_str, int *out_x, int *out_y, int *out_w, int *out_h)
{
	*out_x = 0; *out_y = 0; *out_w = 1920; *out_h = 1080;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	Window root = DefaultRootWindow(dpy);
	Window w = 0;
	if (wid_str && *wid_str) {
		w = (Window)strtoul(wid_str, NULL, 0);
	} else {
		x11_win_info_t foc;
		if (x11_core_get_focused(&foc) && foc.id[0]) {
			w = (Window)strtoul(foc.id, NULL, 0);
		}
	}
	if (w) {
		Window child = 0;
		int rx = 0, ry = 0;
		unsigned int rw = 0, rh = 0, bw = 0, depth = 0;
		Window rret = 0;
		if (XGetGeometry(dpy, w, &rret, &rx, &ry, &rw, &rh, &bw, &depth)) {
			int abs_x = rx, abs_y = ry;
			XTranslateCoordinates(dpy, w, root, 0, 0, &abs_x, &abs_y, &child);
			*out_x = abs_x;
			*out_y = abs_y;
			*out_w = (int)rw;
			*out_h = (int)rh;
		}
	}
	XCloseDisplay(dpy);
}

static __attribute__((unused)) void x11_core_get_mouse_location(int *out_x, int *out_y)
{
	*out_x = 0; *out_y = 0;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	Window root = DefaultRootWindow(dpy);
	Window rret, cret;
	int rx = 0, ry = 0, wx = 0, wy = 0;
	unsigned int mask = 0;
	if (XQueryPointer(dpy, root, &rret, &cret, &rx, &ry, &wx, &wy, &mask)) {
		*out_x = rx;
		*out_y = ry;
	}
	XCloseDisplay(dpy);
}

static bool x11_core_focus_ewmh(const char *wid_str, double timeout_sec)
{
	if (!wid_str || !*wid_str) return false;
	Window w = (Window)strtoul(wid_str, NULL, 0);
	if (!w) return false;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	Window root = DefaultRootWindow(dpy);
	Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = net_active;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = 2; /* pager / automation */
	ev.xclient.data.l[1] = CurrentTime;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XRaiseWindow(dpy, w);
	XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
	XFlush(dpy);
	XCloseDisplay(dpy);
	return x11_core_wait_focused(wid_str, timeout_sec);
}

static __attribute__((unused)) bool x11_core_close_ewmh(const char *wid_str)
{
	if (!wid_str || !*wid_str) return false;
	Window w = (Window)strtoul(wid_str, NULL, 0);
	if (!w) return false;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	Window root = DefaultRootWindow(dpy);
	Atom net_close = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = net_close;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = 2;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XFlush(dpy);
	XCloseDisplay(dpy);
	return true;
}

typedef struct {
	int  (*fake_key)(Display *, unsigned int, Bool, unsigned long);
	int  (*fake_btn)(Display *, unsigned int, Bool, unsigned long);
	int  (*fake_mot)(Display *, int, int, int, unsigned long);
	bool ready;
	bool tried;
} x11_xtst_syms_t;

static x11_xtst_syms_t g_x11_xtst = {0};

static bool x11_core_ensure_xtst(void)
{
	if (g_x11_xtst.tried) return g_x11_xtst.ready;
	g_x11_xtst.tried = true;
	void *h = dlopen("libXtst.so.6", RTLD_LAZY | RTLD_LOCAL);
	if (!h) h = dlopen("libXtst.so", RTLD_LAZY | RTLD_LOCAL);
	if (!h) return false;
	*(void **)&g_x11_xtst.fake_key = dlsym(h, "XTestFakeKeyEvent");
	*(void **)&g_x11_xtst.fake_btn = dlsym(h, "XTestFakeButtonEvent");
	*(void **)&g_x11_xtst.fake_mot = dlsym(h, "XTestFakeMotionEvent");
	if (g_x11_xtst.fake_key && g_x11_xtst.fake_btn && g_x11_xtst.fake_mot) {
		g_x11_xtst.ready = true;
	}
	return g_x11_xtst.ready;
}

static KeySym x11_core_map_keysym_token(const char *tok)
{
	if (!tok || !*tok) return NoSymbol;
	if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0) return XStringToKeysym("Control_L");
	if (strcasecmp(tok, "shift") == 0) return XStringToKeysym("Shift_L");
	if (strcasecmp(tok, "alt") == 0) return XStringToKeysym("Alt_L");
	if (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "win") == 0 || strcasecmp(tok, "meta") == 0) return XStringToKeysym("Super_L");
	if (strcasecmp(tok, "enter") == 0 || strcasecmp(tok, "return") == 0) return XStringToKeysym("Return");
	if (strcasecmp(tok, "esc") == 0 || strcasecmp(tok, "escape") == 0) return XStringToKeysym("Escape");
	if (strcasecmp(tok, "space") == 0) return XStringToKeysym("space");
	if (strcasecmp(tok, "tab") == 0) return XStringToKeysym("Tab");
	if (strcasecmp(tok, "backspace") == 0 || strcasecmp(tok, "bs") == 0) return XStringToKeysym("BackSpace");
	if (strcasecmp(tok, "plus") == 0) return XStringToKeysym("equal");
	if (strcasecmp(tok, "minus") == 0) return XStringToKeysym("minus");
	KeySym ks = XStringToKeysym(tok);
	if (ks == NoSymbol && strlen(tok) == 1) {
		ks = (KeySym)(unsigned char)tok[0];
	}
	return ks;
}

static void x11_core_clear_modifiers(Display *dpy)
{
	if (!dpy || !x11_core_ensure_xtst()) return;
	static const char *const MODS[] = {
		"Super_L", "Super_R", "Shift_L", "Shift_R",
		"Control_L", "Control_R", "Alt_L", "Alt_R", NULL
	};
	for (size_t i = 0; MODS[i]; i++) {
		KeySym ks = XStringToKeysym(MODS[i]);
		if (ks != NoSymbol) {
			KeyCode kc = XKeysymToKeycode(dpy, ks);
			if (kc != 0) g_x11_xtst.fake_key(dpy, kc, False, 0);
		}
	}
	XFlush(dpy);
}

static void x11_core_press_key_combo(const char *combo)
{
	if (!combo || !*combo) combo = "Return";
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (!x11_core_ensure_xtst()) {
		XCloseDisplay(dpy);
		return;
	}
	x11_core_clear_modifiers(dpy);

	char seq_buf[256];
	snprintf(seq_buf, sizeof(seq_buf), "%s", combo);
	char *seq_save = NULL;
	for (char *chord = strtok_r(seq_buf, " ", &seq_save); chord; chord = strtok_r(NULL, " ", &seq_save)) {
		char cbuf[128];
		snprintf(cbuf, sizeof(cbuf), "%s", chord);
		KeyCode codes[8];
		size_t n_codes = 0;
		bool need_shift_plus = (strcasestr(chord, "plus") != NULL && !strcasestr(chord, "shift"));
		if (need_shift_plus) {
			KeyCode skc = XKeysymToKeycode(dpy, XStringToKeysym("Shift_L"));
			if (skc != 0) codes[n_codes++] = skc;
		}
		char *csave = NULL;
		for (char *part = strtok_r(cbuf, "+", &csave); part && n_codes < 8; part = strtok_r(NULL, "+", &csave)) {
			KeySym ks = x11_core_map_keysym_token(part);
			if (ks != NoSymbol) {
				KeyCode kc = XKeysymToKeycode(dpy, ks);
				if (kc != 0) codes[n_codes++] = kc;
			}
		}
		for (size_t i = 0; i + 1 < n_codes; i++) {
			g_x11_xtst.fake_key(dpy, codes[i], True, 0);
		}
		if (n_codes > 1) {
			XFlush(dpy);
			usleep(12000);
		}
		if (n_codes > 0) {
			g_x11_xtst.fake_key(dpy, codes[n_codes - 1], True, 0);
			XFlush(dpy);
			usleep(12000);
			g_x11_xtst.fake_key(dpy, codes[n_codes - 1], False, 0);
			XFlush(dpy);
			usleep(12000);
		}
		for (size_t i = n_codes > 1 ? n_codes - 1 : 0; i > 0; i--) {
			g_x11_xtst.fake_key(dpy, codes[i - 1], False, 0);
		}
		XFlush(dpy);
		usleep(12000);
	}
	XCloseDisplay(dpy);
}

static bool x11_core_char_to_keysym(char c, const char **out_sym, bool *out_shift)
{
	*out_shift = false;
	if (c >= 'a' && c <= 'z') {
		static char s[2];
		s[0] = c; s[1] = '\0';
		*out_sym = s;
		return true;
	}
	if (c >= 'A' && c <= 'Z') {
		static char s[2];
		s[0] = (char)tolower((unsigned char)c); s[1] = '\0';
		*out_sym = s;
		*out_shift = true;
		return true;
	}
	if (c >= '0' && c <= '9') {
		static char s[2];
		s[0] = c; s[1] = '\0';
		*out_sym = s;
		return true;
	}
	switch (c) {
		case ' ':  *out_sym = "space"; return true;
		case '\n': *out_sym = "Return"; return true;
		case '\t': *out_sym = "Tab"; return true;
		case '-':  *out_sym = "minus"; return true;
		case '_':  *out_sym = "minus"; *out_shift = true; return true;
		case '=':  *out_sym = "equal"; return true;
		case '+':  *out_sym = "equal"; *out_shift = true; return true;
		case '/':  *out_sym = "slash"; return true;
		case '?':  *out_sym = "slash"; *out_shift = true; return true;
		case '.':  *out_sym = "period"; return true;
		case '>':  *out_sym = "period"; *out_shift = true; return true;
		case ',':  *out_sym = "comma"; return true;
		case '<':  *out_sym = "comma"; *out_shift = true; return true;
		case ';':  *out_sym = "semicolon"; return true;
		case ':':  *out_sym = "semicolon"; *out_shift = true; return true;
		case '\'': *out_sym = "apostrophe"; return true;
		case '"':  *out_sym = "apostrophe"; *out_shift = true; return true;
		case '[':  *out_sym = "bracketleft"; return true;
		case '{':  *out_sym = "bracketleft"; *out_shift = true; return true;
		case ']':  *out_sym = "bracketright"; return true;
		case '}':  *out_sym = "bracketright"; *out_shift = true; return true;
		case '\\': *out_sym = "backslash"; return true;
		case '|':  *out_sym = "backslash"; *out_shift = true; return true;
		case '`':  *out_sym = "grave"; return true;
		case '~':  *out_sym = "grave"; *out_shift = true; return true;
		case '!':  *out_sym = "1"; *out_shift = true; return true;
		case '@':  *out_sym = "2"; *out_shift = true; return true;
		case '#':  *out_sym = "3"; *out_shift = true; return true;
		case '$':  *out_sym = "4"; *out_shift = true; return true;
		case '%':  *out_sym = "5"; *out_shift = true; return true;
		case '^':  *out_sym = "6"; *out_shift = true; return true;
		case '&':  *out_sym = "7"; *out_shift = true; return true;
		case '*':  *out_sym = "8"; *out_shift = true; return true;
		case '(':  *out_sym = "9"; *out_shift = true; return true;
		case ')':  *out_sym = "0"; *out_shift = true; return true;
		default:   return false;
	}
}

static void x11_core_type_text(const char *text, bool press_enter)
{
	bool has_utf8 = false;
	if (text) {
		for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
			if (*p >= 0x80) {
				has_utf8 = true;
				break;
			}
		}
	}

	x11_win_info_t foc;
	bool has_foc = x11_core_get_focused(&foc);
	bool is_term = (has_foc && x11_core_is_terminal_class(foc.wm_class));
	bool is_browser = (has_foc && x11_core_is_browser_class(foc.wm_class));

	if ((has_utf8 || is_browser) && text && *text) {
		FILE *fp = popen("xsel -ib 2>/dev/null || xclip -selection clipboard -i 2>/dev/null", "w");
		if (fp) {
			fwrite(text, 1, strlen(text), fp);
			pclose(fp);
			usleep(25000);
			x11_core_press_key_combo(is_term ? "ctrl+shift+v" : "ctrl+v");
			usleep(120000);
			if (press_enter) {
				x11_core_press_key_combo("Return");
				usleep(60000);
			}
			return;
		}
	}

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (!x11_core_ensure_xtst()) {
		XCloseDisplay(dpy);
		return;
	}
	x11_core_clear_modifiers(dpy);
	KeyCode shift_kc = XKeysymToKeycode(dpy, XStringToKeysym("Shift_L"));

	if (text && *text) {
		for (const char *p = text; *p; p++) {
			const char *sym_name = NULL;
			bool need_shift = false;
			if (!x11_core_char_to_keysym(*p, &sym_name, &need_shift)) continue;
			KeySym ks = XStringToKeysym(sym_name);
			if (ks == NoSymbol) continue;
			KeyCode kc = XKeysymToKeycode(dpy, ks);
			if (kc == 0) continue;
			if (need_shift && shift_kc != 0) g_x11_xtst.fake_key(dpy, shift_kc, True, 0);
			g_x11_xtst.fake_key(dpy, kc, True, 0);
			g_x11_xtst.fake_key(dpy, kc, False, 0);
			if (need_shift && shift_kc != 0) g_x11_xtst.fake_key(dpy, shift_kc, False, 0);
			XFlush(dpy);
			usleep(1800);
		}
		usleep(15000);
	}
	if (press_enter) {
		KeyCode ret_kc = XKeysymToKeycode(dpy, XStringToKeysym("Return"));
		if (ret_kc != 0) {
			g_x11_xtst.fake_key(dpy, ret_kc, True, 0);
			g_x11_xtst.fake_key(dpy, ret_kc, False, 0);
			XFlush(dpy);
		}
		usleep(25000);
	}
	XCloseDisplay(dpy);
}

static void x11_core_mouse_click(int x, int y, int btn)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (x11_core_ensure_xtst()) {
		g_x11_xtst.fake_mot(dpy, DefaultScreen(dpy), x, y, 0);
		XFlush(dpy);
		usleep(12000);
		g_x11_xtst.fake_btn(dpy, (unsigned int)btn, True, 0);
		XFlush(dpy);
		usleep(18000);
		g_x11_xtst.fake_btn(dpy, (unsigned int)btn, False, 0);
		XFlush(dpy);
	}
	XCloseDisplay(dpy);
}

static __attribute__((unused)) void x11_core_mouse_scroll(int move_x, int move_y, bool need_move, int btn, int count)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (x11_core_ensure_xtst()) {
		if (need_move) {
			g_x11_xtst.fake_mot(dpy, DefaultScreen(dpy), move_x, move_y, 0);
			XFlush(dpy);
		}
		for (int i = 0; i < count; i++) {
			g_x11_xtst.fake_btn(dpy, (unsigned int)btn, True, 0);
			g_x11_xtst.fake_btn(dpy, (unsigned int)btn, False, 0);
			XFlush(dpy);
			if (i + 1 < count) usleep(15000);
		}
	}
	XCloseDisplay(dpy);
}

static void x11_core_cap(const char *cmd, char *out, size_t sz)
{
	if (!out || sz == 0) return;
	out[0] = '\0';
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	size_t rd = fread(out, 1, sz - 1, fp);
	out[rd] = '\0';
	while (rd > 0 && (out[rd - 1] == '\n' || out[rd - 1] == '\r' || out[rd - 1] == ' ')) {
		out[--rd] = '\0';
	}
	pclose(fp);
}

static __attribute__((unused)) void x11_core_read_clipboard(char *prim, size_t prim_sz, char *clip, size_t clip_sz)
{
	x11_core_cap("xclip -o -selection primary 2>/dev/null || xsel -op 2>/dev/null", prim, prim_sz);
	x11_core_cap("xclip -o -selection clipboard 2>/dev/null || xsel -ob 2>/dev/null", clip, clip_sz);
}

#endif /* JARVIS_X11_CORE_H */
