/**
 * @file ren-macos.m
 * @brief Standalone ren extension for macOS Quartz/Cocoa input, windows, clipboard, and context.
 */

#include <ctype.h>
#include <libproc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#include <ttypt/stele.h>
#include "ttypt/libren.h"
#include <ttypt/xy.h>
#include "../../include/ttypt/desktop-core.h"

typedef struct {
	uint32_t win_id;
	pid_t    pid;
	char     owner[128];
	char     title[256];
	int      x, y, w, h;
	bool     focused;
} mac_win_t;

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
}

static size_t query_windows(mac_win_t *out, size_t cap, mac_win_t *out_foc)
{
	if (out_foc) memset(out_foc, 0, sizeof(*out_foc));
	size_t count = 0;

	@autoreleasepool {
		NSRunningApplication *front = [NSWorkspace sharedWorkspace].frontmostApplication;
		pid_t front_pid = front ? front.processIdentifier : 0;

		CFArrayRef list = CGWindowListCopyWindowInfo(
			kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
			kCGNullWindowID);
		if (list) {
			CFIndex nitems = CFArrayGetCount(list);
			for (CFIndex i = 0; i < nitems; i++) {
				NSDictionary *d = (NSDictionary *)CFArrayGetValueAtIndex(list, i);
				NSNumber *layer = [d objectForKey:(id)kCGWindowLayer];
				if (layer && [layer intValue] != 0) continue;

				CFDictionaryRef bounds = (CFDictionaryRef)[d objectForKey:(id)kCGWindowBounds];
				CGRect rc = CGRectZero;
				if (!bounds || !CGRectMakeWithDictionaryRepresentation(bounds, &rc))
					continue;
				if (rc.size.width < 40 || rc.size.height < 40)
					continue;

				mac_win_t mw;
				memset(&mw, 0, sizeof(mw));
				mw.win_id = [d[(id)kCGWindowNumber] unsignedIntValue];
				mw.pid = (pid_t)[d[(id)kCGWindowOwnerPID] intValue];
				NSString *own = d[(id)kCGWindowOwnerName];
				NSString *nam = d[(id)kCGWindowName];
				if (own) snprintf(mw.owner, sizeof(mw.owner), "%s", own.UTF8String);
				if (nam) snprintf(mw.title, sizeof(mw.title), "%s", nam.UTF8String);
				mw.x = (int)rc.origin.x;
				mw.y = (int)rc.origin.y;
				mw.w = (int)rc.size.width;
				mw.h = (int)rc.size.height;
				mw.focused = (front_pid > 0 && mw.pid == front_pid);

				if (mw.focused && out_foc && out_foc->pid == 0)
					*out_foc = mw;

				if (out && count < cap)
					out[count++] = mw;
			}
			CFRelease(list);
		}
		if (out_foc && out_foc->pid == 0 && front) {
			out_foc->pid = front.processIdentifier;
			if (front.localizedName)
				snprintf(out_foc->owner, sizeof(out_foc->owner), "%s", front.localizedName.UTF8String);
			out_foc->focused = true;
		}
	}
	return count;
}

static bool keycode_for_name(const char *tok, CGKeyCode *out_kc)
{
	if (!tok || !*tok || !out_kc) return false;
	if (strcasecmp(tok, "Return") == 0 || strcasecmp(tok, "Enter") == 0) { *out_kc = kVK_Return; return true; }
	if (strcasecmp(tok, "Tab") == 0) { *out_kc = kVK_Tab; return true; }
	if (strcasecmp(tok, "space") == 0) { *out_kc = kVK_Space; return true; }
	if (strcasecmp(tok, "BackSpace") == 0 || strcasecmp(tok, "Backspace") == 0) { *out_kc = kVK_Delete; return true; }
	if (strcasecmp(tok, "Delete") == 0) { *out_kc = kVK_ForwardDelete; return true; }
	if (strcasecmp(tok, "Escape") == 0 || strcasecmp(tok, "Esc") == 0) { *out_kc = kVK_Escape; return true; }
	if (strcasecmp(tok, "Left") == 0) { *out_kc = kVK_LeftArrow; return true; }
	if (strcasecmp(tok, "Right") == 0) { *out_kc = kVK_RightArrow; return true; }
	if (strcasecmp(tok, "Down") == 0) { *out_kc = kVK_DownArrow; return true; }
	if (strcasecmp(tok, "Up") == 0) { *out_kc = kVK_UpArrow; return true; }
	if (strcasecmp(tok, "Home") == 0) { *out_kc = kVK_Home; return true; }
	if (strcasecmp(tok, "End") == 0) { *out_kc = kVK_End; return true; }
	if (strcasecmp(tok, "Page_Up") == 0 || strcasecmp(tok, "Prior") == 0) { *out_kc = kVK_PageUp; return true; }
	if (strcasecmp(tok, "Page_Down") == 0 || strcasecmp(tok, "Next") == 0) { *out_kc = kVK_PageDown; return true; }

	if ((tok[0] == 'F' || tok[0] == 'f') && isdigit((unsigned char)tok[1])) {
		int fn = atoi(tok + 1);
		static const CGKeyCode fkeys[12] = {
			kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6,
			kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12
		};
		if (fn >= 1 && fn <= 12) {
			*out_kc = fkeys[fn - 1];
			return true;
		}
	}

	if (tok[1] == '\0') {
		char c = (char)tolower((unsigned char)tok[0]);
		switch (c) {
		case 'a': *out_kc = kVK_ANSI_A; return true;
		case 'b': *out_kc = kVK_ANSI_B; return true;
		case 'c': *out_kc = kVK_ANSI_C; return true;
		case 'd': *out_kc = kVK_ANSI_D; return true;
		case 'e': *out_kc = kVK_ANSI_E; return true;
		case 'f': *out_kc = kVK_ANSI_F; return true;
		case 'g': *out_kc = kVK_ANSI_G; return true;
		case 'h': *out_kc = kVK_ANSI_H; return true;
		case 'i': *out_kc = kVK_ANSI_I; return true;
		case 'j': *out_kc = kVK_ANSI_J; return true;
		case 'k': *out_kc = kVK_ANSI_K; return true;
		case 'l': *out_kc = kVK_ANSI_L; return true;
		case 'm': *out_kc = kVK_ANSI_M; return true;
		case 'n': *out_kc = kVK_ANSI_N; return true;
		case 'o': *out_kc = kVK_ANSI_O; return true;
		case 'p': *out_kc = kVK_ANSI_P; return true;
		case 'q': *out_kc = kVK_ANSI_Q; return true;
		case 'r': *out_kc = kVK_ANSI_R; return true;
		case 's': *out_kc = kVK_ANSI_S; return true;
		case 't': *out_kc = kVK_ANSI_T; return true;
		case 'u': *out_kc = kVK_ANSI_U; return true;
		case 'v': *out_kc = kVK_ANSI_V; return true;
		case 'w': *out_kc = kVK_ANSI_W; return true;
		case 'x': *out_kc = kVK_ANSI_X; return true;
		case 'y': *out_kc = kVK_ANSI_Y; return true;
		case 'z': *out_kc = kVK_ANSI_Z; return true;
		case '0': *out_kc = kVK_ANSI_0; return true;
		case '1': *out_kc = kVK_ANSI_1; return true;
		case '2': *out_kc = kVK_ANSI_2; return true;
		case '3': *out_kc = kVK_ANSI_3; return true;
		case '4': *out_kc = kVK_ANSI_4; return true;
		case '5': *out_kc = kVK_ANSI_5; return true;
		case '6': *out_kc = kVK_ANSI_6; return true;
		case '7': *out_kc = kVK_ANSI_7; return true;
		case '8': *out_kc = kVK_ANSI_8; return true;
		case '9': *out_kc = kVK_ANSI_9; return true;
		case '-': *out_kc = kVK_ANSI_Minus; return true;
		case '=': *out_kc = kVK_ANSI_Equal; return true;
		case '[': *out_kc = kVK_ANSI_LeftBracket; return true;
		case ']': *out_kc = kVK_ANSI_RightBracket; return true;
		case '\\': *out_kc = kVK_ANSI_Backslash; return true;
		case ';': *out_kc = kVK_ANSI_Semicolon; return true;
		case '\'': *out_kc = kVK_ANSI_Quote; return true;
		case '`': *out_kc = kVK_ANSI_Grave; return true;
		case '/': *out_kc = kVK_ANSI_Slash; return true;
		case '.': *out_kc = kVK_ANSI_Period; return true;
		case ',': *out_kc = kVK_ANSI_Comma; return true;
		default: break;
		}
	}
	return false;
}

static void mac_press_key_combo(const char *combo)
{
	if (!combo || !*combo) return;
	char copy[128];
	snprintf(copy, sizeof(copy), "%s", combo);

	CGEventFlags flags = 0;
	CGKeyCode kc = 0;
	bool have_kc = false;

	char *saveptr = NULL;
	for (char *tok = strtok_r(copy, "+", &saveptr); tok; tok = strtok_r(NULL, "+", &saveptr)) {
		while (*tok == ' ') tok++;
		if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0) {
			flags |= kCGEventFlagMaskControl;
		} else if (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "cmd") == 0 ||
		           strcasecmp(tok, "meta") == 0 || strcasecmp(tok, "win") == 0) {
			flags |= kCGEventFlagMaskCommand;
		} else if (strcasecmp(tok, "alt") == 0 || strcasecmp(tok, "option") == 0 ||
		           strcasecmp(tok, "mod1") == 0) {
			flags |= kCGEventFlagMaskAlternate;
		} else if (strcasecmp(tok, "shift") == 0) {
			flags |= kCGEventFlagMaskShift;
		} else {
			have_kc = keycode_for_name(tok, &kc);
		}
	}
	if (!have_kc) return;

	CGEventRef down = CGEventCreateKeyboardEvent(NULL, kc, true);
	CGEventRef up   = CGEventCreateKeyboardEvent(NULL, kc, false);
	if (down) {
		CGEventSetFlags(down, flags);
		CGEventPost(kCGHIDEventTap, down);
		CFRelease(down);
	}
	if (up) {
		CGEventSetFlags(up, flags);
		CGEventPost(kCGHIDEventTap, up);
		CFRelease(up);
	}
}

static void mac_type_text(const char *text, bool submit)
{
	if (text && *text) {
		@autoreleasepool {
			NSString *ns = [NSString stringWithUTF8String:text];
			if (ns) {
				NSUInteger len = ns.length;
				NSUInteger off = 0;
				while (off < len) {
					NSUInteger chunk = (len - off > 16) ? 16 : (len - off);
					UniChar ubuf[16];
					[ns getCharacters:ubuf range:NSMakeRange(off, chunk)];
					CGEventRef down = CGEventCreateKeyboardEvent(NULL, 0, true);
					CGEventRef up   = CGEventCreateKeyboardEvent(NULL, 0, false);
					if (down) {
						CGEventKeyboardSetUnicodeString(down, (UniCharCount)chunk, ubuf);
						CGEventPost(kCGHIDEventTap, down);
						CFRelease(down);
					}
					if (up) {
						CGEventKeyboardSetUnicodeString(up, (UniCharCount)chunk, ubuf);
						CGEventPost(kCGHIDEventTap, up);
						CFRelease(up);
					}
					off += chunk;
					stl_sleep_ms(4);
				}
			}
		}
	}
	if (submit) {
		stl_sleep_ms(12);
		mac_press_key_combo("Return");
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
		mac_type_text(txt, true);
		char buf[256];
		snprintf(buf, sizeof(buf), "typed+enter: %.120s", txt);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "type ", 5) == 0 || strncasecmp(args, "fill ", 5) == 0) {
		const char *txt = args + 5;
		while (*txt == ' ') txt++;
		mac_type_text(txt, false);
		char buf[256];
		snprintf(buf, sizeof(buf), "typed: %.120s", txt);
		*out_result = strdup(buf);
		return 0;
	}

	if (strncasecmp(args, "key ", 4) == 0) {
		const char *k = args + 4;
		while (*k == ' ') k++;
		mac_press_key_combo(*k ? k : "Return");
		stl_sleep_ms(20);
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
			mac_win_t foc;
			query_windows(NULL, 0, &foc);
			if (foc.w > 0 && foc.h > 0) {
				x = foc.x + foc.w / 2;
				y = foc.y + foc.h / 2;
			} else {
				CGRect scr = CGDisplayBounds(CGMainDisplayID());
				x = (int)(scr.size.width / 2);
				y = (int)(scr.size.height / 2);
			}
		} else {
			*out_result = strdup("Error: input click accepts '<x> <y> [btn]' or 'center'");
			return -1;
		}
		CGPoint pt = CGPointMake((CGFloat)x, (CGFloat)y);
		CGMouseButton mb = (btn == 3) ? kCGMouseButtonRight : ((btn == 2) ? kCGMouseButtonCenter : kCGMouseButtonLeft);
		CGEventType down_t = (btn == 3) ? kCGEventRightMouseDown : ((btn == 2) ? kCGEventOtherMouseDown : kCGEventLeftMouseDown);
		CGEventType up_t   = (btn == 3) ? kCGEventRightMouseUp   : ((btn == 2) ? kCGEventOtherMouseUp   : kCGEventLeftMouseUp);
		CGEventRef d = CGEventCreateMouseEvent(NULL, down_t, pt, mb);
		CGEventRef u = CGEventCreateMouseEvent(NULL, up_t, pt, mb);
		if (d) { CGEventPost(kCGHIDEventTap, d); CFRelease(d); }
		stl_sleep_ms(10);
		if (u) { CGEventPost(kCGHIDEventTap, u); CFRelease(u); }
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
		int32_t dy = 0, dx = 0;
		if (strcasecmp(dir, "up") == 0) dy = amt;
		else if (strcasecmp(dir, "down") == 0) dy = -amt;
		else if (strcasecmp(dir, "left") == 0) dx = amt;
		else if (strcasecmp(dir, "right") == 0) dx = -amt;
		else dy = -amt;

		CGEventRef s = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, dy, dx);
		if (s) {
			CGEventPost(kCGHIDEventTap, s);
			CFRelease(s);
		}
		char buf[128];
		snprintf(buf, sizeof(buf), "scrolled %s x%d", dir, amt);
		*out_result = strdup(buf);
		return 0;
	}

	*out_result = strdup("Error: usage: input <submit <text> | type <text> | key <combo> | click <x y [btn] | center> | scroll <up|down|left|right> [amount]>");
	return -1;
}

static bool activate_window(const mac_win_t *mw)
{
	if (!mw || mw->pid <= 0) return false;
	@autoreleasepool {
		NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:mw->pid];
		if (!app) return false;
		BOOL act = [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
		if (mw->title[0]) {
			AXUIElementRef ax_app = AXUIElementCreateApplication(mw->pid);
			if (ax_app) {
				CFTypeRef wins = NULL;
				if (AXUIElementCopyAttributeValue(ax_app, kAXWindowsAttribute, &wins) == kAXErrorSuccess &&
				    wins && CFGetTypeID(wins) == CFArrayGetTypeID()) {
					CFIndex n = CFArrayGetCount((CFArrayRef)wins);
					for (CFIndex i = 0; i < n; i++) {
						AXUIElementRef w = (AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)wins, i);
						CFTypeRef t = NULL;
						char tbuf[256] = {0};
						if (AXUIElementCopyAttributeValue(w, kAXTitleAttribute, &t) == kAXErrorSuccess && t) {
							if (CFGetTypeID(t) == CFStringGetTypeID())
								CFStringGetCString((CFStringRef)t, tbuf, sizeof(tbuf), kCFStringEncodingUTF8);
							CFRelease(t);
						}
						if (tbuf[0] && desktop_core_str_contains_ci(tbuf, mw->title)) {
							AXUIElementSetAttributeValue(w, kAXMainAttribute, kCFBooleanTrue);
							AXUIElementPerformAction(w, kAXRaiseAction);
							break;
						}
					}
				}
				if (wins) CFRelease(wins);
				CFRelease(ax_app);
			}
		}
		return act == YES;
	}
}

static int tool_window(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (args && strcasecmp(args, "focused") == 0) {
		mac_win_t foc;
		query_windows(NULL, 0, &foc);
		if (foc.pid == 0 && !foc.owner[0]) {
			*out_result = strdup("{\"id\":\"\",\"pid\":0,\"class\":\"\",\"title\":\"\",\"x\":0,\"y\":0,\"w\":0,\"h\":0}");
			return 0;
		}
		CGRect scr = CGDisplayBounds(CGMainDisplayID());
		char eo[160], et[300], buf[700];
		json_esc(foc.owner, eo, sizeof(eo));
		json_esc(foc.title, et, sizeof(et));
		snprintf(buf, sizeof(buf),
		         "{\"id\":\"%u\",\"pid\":%d,\"class\":\"%s\",\"title\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"sw\":%d,\"sh\":%d}",
		         foc.win_id, (int)foc.pid, eo, et, foc.x, foc.y, foc.w, foc.h,
		         (int)scr.size.width, (int)scr.size.height);
		*out_result = strdup(buf);
		return 0;
	}

	if (args && strncasecmp(args, "focus ", 6) == 0) {
		const char *ident = args + 6;
		while (*ident == ' ') ident++;
		mac_win_t wins[48];
		size_t n = query_windows(wins, 48, NULL);
		char *endp = NULL;
		unsigned long qid = strtoul(ident, &endp, 10);
		bool numeric = (endp && endp != ident && *endp == '\0');
		for (size_t i = 0; i < n; i++) {
			if ((numeric && wins[i].win_id == (uint32_t)qid) ||
			    desktop_core_str_contains_ci(wins[i].owner, ident) ||
			    desktop_core_str_contains_ci(wins[i].title, ident)) {
				bool ok = activate_window(&wins[i]);
				char buf[256];
				snprintf(buf, sizeof(buf), "%s %s: %.64s",
				         ok ? "focused" : "failed to focus",
				         wins[i].owner, wins[i].title);
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
		uint32_t qid = (uint32_t)strtoul(ident, NULL, 10);
		mac_win_t wins[48];
		size_t n = query_windows(wins, 48, NULL);
		for (size_t i = 0; i < n; i++) {
			if ((qid > 0 && wins[i].win_id == qid) ||
			    desktop_core_str_contains_ci(wins[i].owner, ident) ||
			    desktop_core_str_contains_ci(wins[i].title, ident)) {
				bool ok = false;
				AXUIElementRef ax_app = AXUIElementCreateApplication(wins[i].pid);
				if (ax_app) {
					CFTypeRef ax_wins = NULL;
					if (AXUIElementCopyAttributeValue(ax_app, kAXWindowsAttribute, &ax_wins) == kAXErrorSuccess &&
					    ax_wins && CFGetTypeID(ax_wins) == CFArrayGetTypeID() &&
					    CFArrayGetCount((CFArrayRef)ax_wins) > 0) {
						CFIndex wcnt = CFArrayGetCount((CFArrayRef)ax_wins);
						AXUIElementRef target_w = (AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)ax_wins, 0);
						if (wins[i].title[0]) {
							for (CFIndex wi = 0; wi < wcnt; wi++) {
								AXUIElementRef cand = (AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)ax_wins, wi);
								CFTypeRef t = NULL;
								char tbuf[256] = {0};
								if (AXUIElementCopyAttributeValue(cand, kAXTitleAttribute, &t) == kAXErrorSuccess && t) {
									if (CFGetTypeID(t) == CFStringGetTypeID())
										CFStringGetCString((CFStringRef)t, tbuf, sizeof(tbuf), kCFStringEncodingUTF8);
									CFRelease(t);
								}
								if (tbuf[0] && desktop_core_str_contains_ci(tbuf, wins[i].title)) {
									target_w = cand;
									break;
								}
							}
						}
						CFTypeRef btn = NULL;
						if (AXUIElementCopyAttributeValue(target_w, kAXCloseButtonAttribute, &btn) == kAXErrorSuccess && btn) {
							ok = (AXUIElementPerformAction((AXUIElementRef)btn, kAXPressAction) == kAXErrorSuccess);
							CFRelease(btn);
						}
					}
					if (ax_wins) CFRelease(ax_wins);
					CFRelease(ax_app);
				}
				*out_result = strdup(ok ? "closed window" : "Error: failed to close window");
				return ok ? 0 : -1;
			}
		}
		*out_result = strdup("Error: window not found");
		return -1;
	}

	mac_win_t wins[48];
	size_t n = query_windows(wins, 48, NULL);
	char out[4096];
	size_t off = 0;
	off += (size_t)snprintf(out + off, sizeof(out) - off, "{\"windows\":[");
	for (size_t i = 0; i < n && off + 200 < sizeof(out); i++) {
		char eo[160], et[256];
		json_esc(wins[i].owner, eo, sizeof(eo));
		json_esc(wins[i].title, et, sizeof(et));
		off += (size_t)snprintf(out + off, sizeof(out) - off,
		                        "%s{\"id\":\"%u\",\"class\":\"%s\",\"title\":\"%s\",\"tag\":1,\"visible\":true,\"focused\":%s}",
		                        i > 0 ? "," : "",
		                        wins[i].win_id, eo, et,
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
	@autoreleasepool {
		NSPasteboard *pb = [NSPasteboard generalPasteboard];
		NSString *s = pb ? [pb stringForType:NSPasteboardTypeString] : nil;
		if (s) desktop_core_utf8_copy(clip, sizeof(clip), s.UTF8String);
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
	const char *argv[] = { "open", "-a", "Terminal", NULL };
	int rc = stl_spawn_detached(argv);
	*out_result = strdup(rc == 0 ? "opened Terminal.app" : "Error: failed to open Terminal");
	return rc == 0 ? 0 : -1;
}

static char *macos_context_provider(void)
{
	mac_win_t wins[24], foc;
	size_t n = query_windows(wins, 24, &foc);
	if (n == 0 && !foc.owner[0] && !foc.title[0]) return NULL;

	char cwd[256] = {0};
	if (foc.pid > 0 && desktop_core_is_terminal_class(foc.owner)) {
		struct proc_vnodepathinfo vpi;
		if (proc_pidinfo(foc.pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) > 0)
			snprintf(cwd, sizeof(cwd), "%s", vpi.pvi_cdir.vip_path);
	}

	char wlist[512] = {0};
	size_t woff = 0;
	for (size_t i = 0; i < n && woff + 64 < sizeof(wlist); i++) {
		char short_t[41];
		desktop_core_utf8_copy(short_t, sizeof(short_t), wins[i].title);
		woff += (size_t)snprintf(wlist + woff, sizeof(wlist) - woff,
		                         "%s%s:'%s'",
		                         i > 0 ? ", " : "",
		                         wins[i].owner, short_t);
	}

	char foc_t[81];
	desktop_core_utf8_copy(foc_t, sizeof(foc_t), foc.title);
	char buf[1280];
	snprintf(buf, sizeof(buf),
	         "macos_focused='%s' title='%s' cwd='%s' windows=[%s]",
	         foc.owner, foc_t, cwd, wlist);
	return strdup(buf);
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("input", "input <submit <text> | type <text> | key <combo> | click <x y [btn] | center> | scroll <up|down|left|right> [amount]>", tool_input);
	ren_register_tool("window", "window [focused | focus <id_or_title> | close <id>]", tool_window);
	ren_register_tool("clipboard", "clipboard", tool_clipboard);
	ren_register_tool("open_terminal", "open_terminal", tool_open_terminal);
	ren_register_context_provider(macos_context_provider);
}
