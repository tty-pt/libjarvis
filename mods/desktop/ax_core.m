#include "a11y_core.h"
#include "../../include/ttypt/desktop-core.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreFoundation/CoreFoundation.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AX_MAX_NODES 500
#define AX_MAX_DEPTH 14

static void ax_cfstr_to_c(CFTypeRef ref, char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';
	if (!ref) return;
	if (CFGetTypeID(ref) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)ref, out, (CFIndex)cap, kCFStringEncodingUTF8);
	} else if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
		double d = 0.0;
		if (CFNumberGetValue((CFNumberRef)ref, kCFNumberFloat64Type, &d))
			snprintf(out, cap, "%g", d);
	}
}

static void ax_copy_str_attr(AXUIElementRef el, CFStringRef attr, char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';
	if (!el) return;
	CFTypeRef val = NULL;
	if (AXUIElementCopyAttributeValue(el, attr, &val) == kAXErrorSuccess && val) {
		ax_cfstr_to_c(val, out, cap);
		CFRelease(val);
	}
}

static bool ax_copy_bool_attr(AXUIElementRef el, CFStringRef attr, bool *out_b)
{
	if (out_b) *out_b = false;
	if (!el) return false;
	CFTypeRef val = NULL;
	if (AXUIElementCopyAttributeValue(el, attr, &val) != kAXErrorSuccess || !val)
		return false;
	bool ok = false;
	if (CFGetTypeID(val) == CFBooleanGetTypeID()) {
		if (out_b) *out_b = CFBooleanGetValue((CFBooleanRef)val);
		ok = true;
	}
	CFRelease(val);
	return ok;
}

static bool ax_get_bounds(AXUIElementRef el, int *x, int *y, int *w, int *h)
{
	if (!el) return false;
	CFTypeRef pos_ref = NULL, sz_ref = NULL;
	CGPoint pt = CGPointZero;
	CGSize sz = CGSizeZero;
	bool ok = false;

	if (AXUIElementCopyAttributeValue(el, kAXPositionAttribute, &pos_ref) == kAXErrorSuccess &&
	    AXUIElementCopyAttributeValue(el, kAXSizeAttribute, &sz_ref) == kAXErrorSuccess &&
	    pos_ref && sz_ref &&
	    CFGetTypeID(pos_ref) == AXValueGetTypeID() &&
	    CFGetTypeID(sz_ref) == AXValueGetTypeID()) {
		if (AXValueGetValue((AXValueRef)pos_ref, kAXValueTypeCGPoint, &pt) &&
		    AXValueGetValue((AXValueRef)sz_ref, kAXValueTypeCGSize, &sz)) {
			if (x) *x = (int)pt.x;
			if (y) *y = (int)pt.y;
			if (w) *w = (int)sz.width;
			if (h) *h = (int)sz.height;
			ok = (sz.width > 0 && sz.height > 0);
		}
	}
	if (pos_ref) CFRelease(pos_ref);
	if (sz_ref) CFRelease(sz_ref);
	return ok;
}

static bool ax_is_editable_role(const char *role, AXUIElementRef el)
{
	if (role) {
		if (strcmp(role, "AXTextField") == 0 ||
		    strcmp(role, "AXTextArea") == 0 ||
		    strcmp(role, "AXComboBox") == 0 ||
		    strcmp(role, "AXSearchField") == 0 ||
		    strcmp(role, "AXSecureTextField") == 0)
			return true;
	}
	Boolean settable = false;
	if (el && AXUIElementIsAttributeSettable(el, kAXValueAttribute, &settable) == kAXErrorSuccess && settable) {
		if (role && (strcmp(role, "AXCheckBox") == 0 || strcmp(role, "AXRadioButton") == 0))
			return false;
		return true;
	}
	return false;
}

static bool ax_is_interactive_role(const char *role)
{
	if (!role || !*role) return false;
	static const char *const roles[] = {
		"AXButton", "AXTextField", "AXTextArea", "AXSearchField",
		"AXSecureTextField", "AXPopUpButton", "AXComboBox", "AXCheckBox",
		"AXRadioButton", "AXLink", "AXMenuItem", "AXMenuBarItem",
		"AXTab", "AXSlider", "AXDisclosureTriangle", "AXIncrementor",
		NULL
	};
	for (size_t i = 0; roles[i]; i++) {
		if (strcmp(role, roles[i]) == 0) return true;
	}
	return false;
}

static int ax_score_label(const char *label, const char *query)
{
	if (!label || !*label || !query || !*query) return 0;
	if (strcasecmp(label, query) == 0) return 100;
	size_t qlen = strlen(query);
	if (strncasecmp(label, query, qlen) == 0) return 80;
	if (desktop_core_str_contains_ci(label, query)) return 50;
	return 0;
}

static void ax_enable_app_a11y(AXUIElementRef app)
{
	if (!app) return;
	AXUIElementSetAttributeValue(app, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
	AXUIElementSetAttributeValue(app, CFSTR("AXEnhancedUserInterface"), kCFBooleanTrue);
}

static AXUIElementRef ax_resolve_root(const a11y_win_ctx_t *win, char *out_err, size_t err_sz)
{
	if (!AXIsProcessTrusted()) {
		if (out_err && err_sz > 0)
			snprintf(out_err, err_sz, "Error: macOS Accessibility permission required for ren");
		return NULL;
	}

	pid_t pid = (win && win->pid > 0) ? (pid_t)win->pid : 0;
	if (pid <= 0) {
		@autoreleasepool {
			NSRunningApplication *front = [NSWorkspace sharedWorkspace].frontmostApplication;
			if (front) pid = front.processIdentifier;
		}
	}
	if (pid <= 0) {
		if (out_err && err_sz > 0)
			snprintf(out_err, err_sz, "Error: no target macOS application window found");
		return NULL;
	}

	AXUIElementRef app = AXUIElementCreateApplication(pid);
	if (!app) return NULL;
	ax_enable_app_a11y(app);

	if (win && win->title && win->title[0]) {
		CFTypeRef wins_ref = NULL;
		if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &wins_ref) == kAXErrorSuccess &&
		    wins_ref && CFGetTypeID(wins_ref) == CFArrayGetTypeID()) {
			CFIndex count = CFArrayGetCount((CFArrayRef)wins_ref);
			for (CFIndex i = 0; i < count; i++) {
				AXUIElementRef w = (AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)wins_ref, i);
				char title[256] = {0};
				ax_copy_str_attr(w, kAXTitleAttribute, title, sizeof(title));
				if (title[0] && (strcasecmp(title, win->title) == 0 ||
				                 desktop_core_str_contains_ci(title, win->title))) {
					CFRetain(w);
					CFRelease(wins_ref);
					CFRelease(app);
					return w;
				}
			}
		}
		if (wins_ref) CFRelease(wins_ref);
	}

	CFTypeRef foc_win = NULL;
	if (AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &foc_win) == kAXErrorSuccess &&
	    foc_win && CFGetTypeID(foc_win) == AXUIElementGetTypeID()) {
		CFRelease(app);
		return (AXUIElementRef)foc_win;
	}
	if (foc_win) CFRelease(foc_win);
	return app;
}

static void ax_best_label(AXUIElementRef el, char *out, size_t cap)
{
	ax_copy_str_attr(el, kAXTitleAttribute, out, cap);
	if (!out[0]) ax_copy_str_attr(el, kAXDescriptionAttribute, out, cap);
	if (!out[0]) ax_copy_str_attr(el, kAXPlaceholderValueAttribute, out, cap);
	if (!out[0]) ax_copy_str_attr(el, kAXHelpAttribute, out, cap);
	if (!out[0]) ax_copy_str_attr(el, kAXIdentifierAttribute, out, cap);
}

typedef struct {
	bool        inspect_mode;
	const char *filter;
	char       *out;
	size_t      out_sz;
	size_t      off;
	int         visited;
} ax_read_ctx_t;

static void ax_walk_read(AXUIElementRef el, int depth, ax_read_ctx_t *ctx)
{
	if (!el || depth > AX_MAX_DEPTH || ctx->visited >= AX_MAX_NODES ||
	    ctx->off + 256 >= ctx->out_sz)
		return;
	ctx->visited++;

	char role[64] = {0}, label[256] = {0}, val[256] = {0};
	ax_copy_str_attr(el, kAXRoleAttribute, role, sizeof(role));
	ax_best_label(el, label, sizeof(label));
	ax_copy_str_attr(el, kAXValueAttribute, val, sizeof(val));

	int x = 0, y = 0, w = 0, h = 0;
	bool has_rect = ax_get_bounds(el, &x, &y, &w, &h);
	bool interesting = ctx->inspect_mode ? (role[0] != '\0' && (label[0] || val[0] || ax_is_interactive_role(role)))
	                                     : (ax_is_interactive_role(role) || (label[0] || val[0]));
	if (interesting) {
		bool match = true;
		if (ctx->filter && *ctx->filter) {
			match = desktop_core_str_contains_ci(role, ctx->filter) ||
			        desktop_core_str_contains_ci(label, ctx->filter) ||
			        desktop_core_str_contains_ci(val, ctx->filter);
		}
		if (match) {
			int n = 0;
			if (has_rect && ctx->inspect_mode) {
				n = snprintf(ctx->out + ctx->off, ctx->out_sz - ctx->off,
				             "%*s[%s] \"%s\"%s%s%s (%d,%d %dx%d)\n",
				             depth > 4 ? 8 : depth * 2, "",
				             role[0] ? role : "AXElement",
				             label,
				             val[0] ? " val=\"" : "", val[0] ? val : "", val[0] ? "\"" : "",
				             x, y, w, h);
			} else if (label[0] || val[0]) {
				n = snprintf(ctx->out + ctx->off, ctx->out_sz - ctx->off,
				             "[%s] %s%s%s\n",
				             role[0] ? role : "AXElement",
				             label[0] ? label : val,
				             (label[0] && val[0] && strcmp(label, val) != 0) ? " = " : "",
				             (label[0] && val[0] && strcmp(label, val) != 0) ? val : "");
			}
			if (n > 0) {
				if ((size_t)n >= ctx->out_sz - ctx->off)
					ctx->off = ctx->out_sz - 1;
				else
					ctx->off += (size_t)n;
			}
		}
	}

	CFTypeRef children = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
	    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount((CFArrayRef)children);
		if (n > 64) n = 64;
		for (CFIndex i = 0; i < n; i++) {
			if (ctx->visited >= AX_MAX_NODES || ctx->off + 256 >= ctx->out_sz) break;
			AXUIElementRef ch = (AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)children, i);
			ax_walk_read(ch, depth + 1, ctx);
		}
	}
	if (children) CFRelease(children);
}

typedef struct {
	const char    *query;
	bool           editable_only;
	AXUIElementRef best;
	int            best_score;
	char           best_role[64];
	char           best_label[256];
	int            visited;
} ax_find_ctx_t;

static void ax_walk_find(AXUIElementRef el, int depth, ax_find_ctx_t *ctx)
{
	if (!el || depth > AX_MAX_DEPTH || ctx->visited >= AX_MAX_NODES)
		return;
	ctx->visited++;

	char role[64] = {0}, label[256] = {0}, val[128] = {0};
	ax_copy_str_attr(el, kAXRoleAttribute, role, sizeof(role));
	ax_best_label(el, label, sizeof(label));
	ax_copy_str_attr(el, kAXValueAttribute, val, sizeof(val));

	bool ok_kind = ctx->editable_only ? ax_is_editable_role(role, el) : true;
	if (ok_kind) {
		int sc = ax_score_label(label, ctx->query);
		int vsc = ax_score_label(val, ctx->query);
		if (vsc > sc) sc = vsc;
		if (sc > 0 && ax_is_interactive_role(role)) sc += 5;
		if (sc > ctx->best_score) {
			if (ctx->best) CFRelease(ctx->best);
			ctx->best = (AXUIElementRef)CFRetain(el);
			ctx->best_score = sc;
			snprintf(ctx->best_role, sizeof(ctx->best_role), "%s", role);
			snprintf(ctx->best_label, sizeof(ctx->best_label), "%s", label[0] ? label : val);
		}
	}

	CFTypeRef children = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
	    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount((CFArrayRef)children);
		if (n > 64) n = 64;
		for (CFIndex i = 0; i < n; i++) {
			if (ctx->visited >= AX_MAX_NODES) break;
			AXUIElementRef ch = (AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)children, i);
			ax_walk_find(ch, depth + 1, ctx);
		}
	}
	if (children) CFRelease(children);
}

bool ax_core_read_window(const a11y_win_ctx_t *win, bool inspect_mode,
                         const char *filter, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';
	char err[256] = {0};
	AXUIElementRef root = ax_resolve_root(win, err, sizeof(err));
	if (!root) {
		if (err[0]) snprintf(out, out_sz, "%s", err);
		return false;
	}

	ax_read_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.inspect_mode = inspect_mode;
	ctx.filter = filter;
	ctx.out = out;
	ctx.out_sz = out_sz;
	ax_walk_read(root, 0, &ctx);
	CFRelease(root);
	return out[0] != '\0';
}

bool ax_core_click(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                   const char *query, int btn, char *out_msg, size_t out_sz)
{
	if (!query || !*query) {
		snprintf(out_msg, out_sz, "Error: empty ui click target");
		return false;
	}
	AXUIElementRef root = ax_resolve_root(win, out_msg, out_sz);
	if (!root) return false;

	ax_find_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ax_walk_find(root, 0, &ctx);
	CFRelease(root);

	if (!ctx.best) {
		snprintf(out_msg, out_sz, "Error: ui target not found: %.160s", query);
		return false;
	}

	if (ops && ops->is_win_focused && ops->focus_win && !ops->is_win_focused(ops->ud)) {
		ops->focus_win(ops->ud);
		usleep(80000);
	}

	bool ok = false;
	if (btn == 3) {
		ok = (AXUIElementPerformAction(ctx.best, kAXShowMenuAction) == kAXErrorSuccess);
	} else {
		ok = (AXUIElementPerformAction(ctx.best, kAXPressAction) == kAXErrorSuccess);
	}
	if (!ok && ops && ops->click_xy) {
		int x = 0, y = 0, w = 0, h = 0;
		if (ax_get_bounds(ctx.best, &x, &y, &w, &h))
			ok = ops->click_xy(ops->ud, x + w / 2, y + h / 2, btn);
	}
	snprintf(out_msg, out_sz, "%s [%s '%.80s']",
	         ok ? "clicked" : "Error: failed to click",
	         ctx.best_role[0] ? ctx.best_role : "AXElement",
	         ctx.best_label[0] ? ctx.best_label : query);
	CFRelease(ctx.best);
	return ok;
}

bool ax_core_focus(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                   const char *query, char *out_msg, size_t out_sz)
{
	if (!query || !*query) {
		snprintf(out_msg, out_sz, "Error: empty ui focus target");
		return false;
	}
	AXUIElementRef root = ax_resolve_root(win, out_msg, out_sz);
	if (!root) return false;

	ax_find_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ax_walk_find(root, 0, &ctx);
	CFRelease(root);

	if (!ctx.best) {
		snprintf(out_msg, out_sz, "Error: ui target not found: %.160s", query);
		return false;
	}

	if (ops && ops->is_win_focused && ops->focus_win && !ops->is_win_focused(ops->ud)) {
		ops->focus_win(ops->ud);
		usleep(80000);
	}

	bool ok = (AXUIElementSetAttributeValue(ctx.best, kAXFocusedAttribute, kCFBooleanTrue) == kAXErrorSuccess);
	if (!ok && ops && ops->click_xy) {
		int x = 0, y = 0, w = 0, h = 0;
		if (ax_get_bounds(ctx.best, &x, &y, &w, &h))
			ok = ops->click_xy(ops->ud, x + w / 2, y + h / 2, 1);
	}
	snprintf(out_msg, out_sz, "%s [%s '%.80s']",
	         ok ? "focused" : "Error: failed to focus",
	         ctx.best_role[0] ? ctx.best_role : "AXElement",
	         ctx.best_label[0] ? ctx.best_label : query);
	CFRelease(ctx.best);
	return ok;
}

bool ax_core_prepare_input(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                           const char *target, bool is_terminal,
                           char *out_role, size_t role_sz,
                           char *out_label, size_t label_sz,
                           char *out_err, size_t err_sz)
{
	if (out_role && role_sz > 0) out_role[0] = '\0';
	if (out_label && label_sz > 0) out_label[0] = '\0';
	if (out_err && err_sz > 0) out_err[0] = '\0';

	if (is_terminal) {
		if (out_role && role_sz > 0) snprintf(out_role, role_sz, "terminal");
		return true;
	}

	AXUIElementRef root = ax_resolve_root(win, out_err, err_sz);
	if (!root) return false;

	if (target && *target) {
		ax_find_ctx_t ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.query = target;
		ctx.editable_only = true;
		ax_walk_find(root, 0, &ctx);
		if (!ctx.best) {
			memset(&ctx, 0, sizeof(ctx));
			ctx.query = target;
			ax_walk_find(root, 0, &ctx);
		}
		CFRelease(root);
		if (!ctx.best) {
			if (out_err && err_sz > 0)
				snprintf(out_err, err_sz, "Error: input field not found matching '%.160s'", target);
			return false;
		}
		if (ops && ops->is_win_focused && ops->focus_win && !ops->is_win_focused(ops->ud)) {
			ops->focus_win(ops->ud);
			usleep(80000);
		}
		bool focused = (AXUIElementSetAttributeValue(ctx.best, kAXFocusedAttribute, kCFBooleanTrue) == kAXErrorSuccess);
		if (!focused && ops && ops->click_xy) {
			int x = 0, y = 0, w = 0, h = 0;
			if (ax_get_bounds(ctx.best, &x, &y, &w, &h))
				focused = ops->click_xy(ops->ud, x + w / 2, y + h / 2, 1);
		}
		if (out_role && role_sz > 0)
			snprintf(out_role, role_sz, "%s", ctx.best_role[0] ? ctx.best_role : "input");
		if (out_label && label_sz > 0)
			snprintf(out_label, label_sz, "%s", ctx.best_label[0] ? ctx.best_label : target);
		CFRelease(ctx.best);
		if (!focused && out_err && err_sz > 0)
			snprintf(out_err, err_sz, "Error: failed to focus input field '%.160s'", target);
		return focused;
	}

	AXUIElementRef sys = AXUIElementCreateSystemWide();
	CFTypeRef foc_el = NULL;
	AXUIElementRef check_el = NULL;
	if (sys && AXUIElementCopyAttributeValue(sys, kAXFocusedUIElementAttribute, &foc_el) == kAXErrorSuccess &&
	    foc_el && CFGetTypeID(foc_el) == AXUIElementGetTypeID()) {
		check_el = (AXUIElementRef)foc_el;
	} else {
		if (foc_el) CFRelease(foc_el);
		if (AXUIElementCopyAttributeValue(root, kAXFocusedUIElementAttribute, &foc_el) == kAXErrorSuccess &&
		    foc_el && CFGetTypeID(foc_el) == AXUIElementGetTypeID()) {
			check_el = (AXUIElementRef)foc_el;
		}
	}
	if (sys) CFRelease(sys);
	CFRelease(root);

	if (!check_el) {
		if (out_err && err_sz > 0)
			snprintf(out_err, err_sz, "Error: no editable input focused");
		return false;
	}
	char role[64] = {0}, label[128] = {0};
	ax_copy_str_attr(check_el, kAXRoleAttribute, role, sizeof(role));
	ax_best_label(check_el, label, sizeof(label));
	bool editable = ax_is_editable_role(role, check_el);
	CFRelease(check_el);

	if (!editable) {
		if (out_err && err_sz > 0)
			snprintf(out_err, err_sz, "Error: focused element [%s] is not editable", role[0] ? role : "unknown");
		return false;
	}
	if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", role);
	if (out_label && label_sz > 0) snprintf(out_label, label_sz, "%s", label);
	return true;
}

bool ax_core_set(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                 const char *query, const char *value, char *out_msg, size_t out_sz)
{
	if (!query || !*query || !value) {
		snprintf(out_msg, out_sz, "Error: usage: ui set <target> | <value>");
		return false;
	}
	AXUIElementRef root = ax_resolve_root(win, out_msg, out_sz);
	if (!root) return false;

	ax_find_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ax_walk_find(root, 0, &ctx);
	CFRelease(root);

	if (!ctx.best) {
		snprintf(out_msg, out_sz, "Error: ui target not found: %.160s", query);
		return false;
	}

	bool ok = false;
	Boolean settable = false;
	if (AXUIElementIsAttributeSettable(ctx.best, kAXValueAttribute, &settable) == kAXErrorSuccess && settable) {
		if (strcmp(ctx.best_role, "AXSlider") == 0) {
			double dv = strtod(value, NULL);
			CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &dv);
			if (num) {
				ok = (AXUIElementSetAttributeValue(ctx.best, kAXValueAttribute, num) == kAXErrorSuccess);
				CFRelease(num);
			}
		}
		if (!ok) {
			CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, value, kCFStringEncodingUTF8);
			if (s) {
				ok = (AXUIElementSetAttributeValue(ctx.best, kAXValueAttribute, s) == kAXErrorSuccess);
				CFRelease(s);
			}
		}
	}

	if (!ok && ops && ops->send_key && ops->type_text) {
		if (AXUIElementSetAttributeValue(ctx.best, kAXFocusedAttribute, kCFBooleanTrue) == kAXErrorSuccess) {
			ops->send_key(ops->ud, "super+a");
			ok = ops->type_text(ops->ud, value, false);
		}
	}

	snprintf(out_msg, out_sz, "%s [%s '%.80s'] = '%.80s'",
	         ok ? "set" : "Error: failed to set",
	         ctx.best_role[0] ? ctx.best_role : "AXElement",
	         ctx.best_label[0] ? ctx.best_label : query,
	         value);
	CFRelease(ctx.best);
	return ok;
}

#define AX_WEB_MAX_NODES 3500
#define AX_WEB_MAX_DEPTH 28

static void ax_copy_url_attr(AXUIElementRef el, char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';
	if (!el) return;
	CFTypeRef val = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXURLAttribute, &val) == kAXErrorSuccess && val) {
		if (CFGetTypeID(val) == CFURLGetTypeID()) {
			CFStringRef s = CFURLGetString((CFURLRef)val);
			if (s) CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingUTF8);
		} else {
			ax_cfstr_to_c(val, out, cap);
		}
		CFRelease(val);
	}
}

static AXUIElementRef ax_resolve_browser_root(int *out_wx, int *out_wy, int *out_ww, int *out_wh,
                                              int *out_sw, int *out_sh)
{
	if (out_wx) *out_wx = 0;
	if (out_wy) *out_wy = 0;
	if (out_ww) *out_ww = 0;
	if (out_wh) *out_wh = 0;
	if (out_sw) *out_sw = 0;
	if (out_sh) *out_sh = 0;
	if (!AXIsProcessTrusted()) return NULL;

	CGRect dbounds = CGDisplayBounds(CGMainDisplayID());
	if (out_sw) *out_sw = (int)dbounds.size.width;
	if (out_sh) *out_sh = (int)dbounds.size.height;

	pid_t pid = 0;
	@autoreleasepool {
		NSWorkspace *ws = [NSWorkspace sharedWorkspace];
		NSRunningApplication *front = ws.frontmostApplication;
		if (front && front.localizedName &&
		    desktop_core_is_browser_name(front.localizedName.UTF8String)) {
			pid = front.processIdentifier;
		} else {
			for (NSRunningApplication *app in ws.runningApplications) {
				if (!app.localizedName) continue;
				if (desktop_core_is_browser_name(app.localizedName.UTF8String)) {
					pid = app.processIdentifier;
					if (!app.hidden) break;
				}
			}
		}
	}
	if (pid <= 0) return NULL;

	AXUIElementRef app = AXUIElementCreateApplication(pid);
	if (!app) return NULL;
	ax_enable_app_a11y(app);
	CFTypeRef foc_win = NULL;
	if (AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &foc_win) == kAXErrorSuccess &&
	    foc_win && CFGetTypeID(foc_win) == AXUIElementGetTypeID()) {
		ax_get_bounds((AXUIElementRef)foc_win, out_wx, out_wy, out_ww, out_wh);
		CFRelease(app);
		return (AXUIElementRef)foc_win;
	}
	if (foc_win) CFRelease(foc_win);
	return app;
}

static bool ax_is_web_interactive(const char *role)
{
	if (!role) return false;
	return ax_is_interactive_role(role) ||
	       strcmp(role, "AXHeading") == 0 ||
	       strcmp(role, "AXRow") == 0;
}

static bool ax_is_web_text(const char *role)
{
	if (!role) return false;
	return strcmp(role, "AXStaticText") == 0 ||
	       strcmp(role, "AXHeading") == 0 ||
	       strcmp(role, "AXLink") == 0 ||
	       strcmp(role, "AXTextField") == 0 ||
	       strcmp(role, "AXTextArea") == 0 ||
	       strcmp(role, "AXButton") == 0;
}

static bool ax_in_viewport(int x, int y, int wx, int wy, int ww, int wh, int sw, int sh)
{
	if (x < 0 || y < 0) return false;
	if (ww > 0 && wh > 0 && (x < wx || x >= wx + ww || y < wy || y >= wy + wh))
		return false;
	if (sw > 0 && sh > 0 && (x >= sw || y >= sh))
		return false;
	return true;
}

static void ax_walk_browser_probe(AXUIElementRef el, int depth, int *visited, a11y_browser_doc_t *st)
{
	if (!el || !st || st->found || depth > AX_WEB_MAX_DEPTH || *visited >= 1200)
		return;
	(*visited)++;

	char role[64] = {0};
	ax_copy_str_attr(el, kAXRoleAttribute, role, sizeof(role));
	if (strcmp(role, "AXWebArea") == 0) {
		st->found = true;
		st->showing = true;
		st->defunct = false;
		bool loaded = true;
		if (ax_copy_bool_attr(el, CFSTR("AXLoaded"), &loaded) && !loaded)
			st->busy = true;
		ax_copy_str_attr(el, kAXTitleAttribute, st->title, sizeof(st->title));
		ax_copy_url_attr(el, st->url, sizeof(st->url));
		CFTypeRef children = NULL;
		if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
		    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
			st->child_count = (int)CFArrayGetCount((CFArrayRef)children);
		}
		if (children) CFRelease(children);
		return;
	}

	CFTypeRef children = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
	    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount((CFArrayRef)children);
		if (n > 64) n = 64;
		for (CFIndex i = 0; i < n && !st->found; i++) {
			ax_walk_browser_probe((AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)children, i),
			                      depth + 1, visited, st);
		}
	}
	if (children) CFRelease(children);
}

bool ax_core_browser_probe_doc(a11y_browser_doc_t *out_doc)
{
	if (!out_doc) return false;
	memset(out_doc, 0, sizeof(*out_doc));
	AXUIElementRef root = ax_resolve_browser_root(NULL, NULL, NULL, NULL, NULL, NULL);
	if (!root) return false;
	int visited = 0;
	ax_walk_browser_probe(root, 0, &visited, out_doc);
	if (out_doc->found && !out_doc->title[0])
		ax_copy_str_attr(root, kAXTitleAttribute, out_doc->title, sizeof(out_doc->title));
	CFRelease(root);
	return out_doc->found;
}

typedef struct {
	const char *filter;
	bool        read_mode;
	size_t      skip_chars;
	size_t      seen_chars;
	bool        truncated;
	char       *out;
	size_t      out_sz;
	size_t      off;
	int         visited;
	int         wx, wy, ww, wh, sw, sh;
} ax_bwalk_ctx_t;

static void ax_bemit_line(ax_bwalk_ctx_t *ctx, const char *line)
{
	if (!ctx || !line || !*line || ctx->truncated || ctx->out_sz < 96) return;
	size_t len = strlen(line);
	if (ctx->seen_chars + len <= ctx->skip_chars) {
		ctx->seen_chars += len;
		return;
	}
	size_t start = (ctx->skip_chars > ctx->seen_chars) ? (ctx->skip_chars - ctx->seen_chars) : 0;
	ctx->seen_chars += len;
	size_t rem = len - start;
	size_t limit = ctx->out_sz - 80;
	if (ctx->off >= limit) {
		ctx->truncated = true;
		return;
	}
	size_t avail = limit - ctx->off;
	if (rem > avail) {
		memcpy(ctx->out + ctx->off, line + start, avail);
		ctx->off += avail;
		ctx->out[ctx->off] = '\0';
		ctx->truncated = true;
		return;
	}
	memcpy(ctx->out + ctx->off, line + start, rem);
	ctx->off += rem;
	ctx->out[ctx->off] = '\0';
}

static void ax_walk_browser_collect(AXUIElementRef el, int depth, bool in_doc, bool in_tabs,
                                    const char *filter, ax_bwalk_ctx_t *ctx)
{
	if (!el || depth > AX_WEB_MAX_DEPTH || ctx->visited >= AX_WEB_MAX_NODES || ctx->truncated)
		return;
	ctx->visited++;

	char role[64] = {0}, label[256] = {0}, val[256] = {0};
	ax_copy_str_attr(el, kAXRoleAttribute, role, sizeof(role));
	ax_best_label(el, label, sizeof(label));
	ax_copy_str_attr(el, kAXValueAttribute, val, sizeof(val));

	bool now_in_doc = in_doc;
	bool now_in_tabs = in_tabs || (strcmp(role, "AXTabGroup") == 0);
	const char *child_filter = filter;
	bool skip_children = false;

	if (strcmp(role, "AXWebArea") == 0) {
		now_in_doc = true;
		char url[512] = {0};
		ax_copy_url_attr(el, url, sizeof(url));
		if (child_filter && *child_filter &&
		    (desktop_core_str_contains_ci(label, child_filter) ||
		     desktop_core_str_contains_ci(url, child_filter))) {
			child_filter = NULL;
		}
		char line[900];
		snprintf(line, sizeof(line), "[document web] %.200s%s%.500s\n",
		         label, url[0] ? " url=" : "", url);
		ax_bemit_line(ctx, line);
	} else if (!in_doc && !ctx->read_mode &&
	           (strcmp(role, "AXTab") == 0 || (in_tabs && strcmp(role, "AXRadioButton") == 0)) &&
	           label[0]) {
		int x = 0, y = 0, w = 0, h = 0;
		char coord[32] = {0};
		if (ax_get_bounds(el, &x, &y, &w, &h)) {
			int cx = x + w / 2, cy = y + h / 2;
			if (ax_in_viewport(cx, cy, ctx->wx, ctx->wy, ctx->ww, ctx->wh, ctx->sw, ctx->sh))
				snprintf(coord, sizeof(coord), " @(%d,%d)", cx, cy);
			else
				snprintf(coord, sizeof(coord), " *OFF-SCREEN*");
		}
		bool active = (strcmp(val, "1") == 0);
		if (!active) ax_copy_bool_attr(el, kAXSelectedAttribute, &active);
		char line[360];
		snprintf(line, sizeof(line), "[page tab%s%s] %.200s\n",
		         coord, active ? " *ACTIVE*" : "", label);
		ax_bemit_line(ctx, line);
		skip_children = true;
	} else if (now_in_doc && !ctx->read_mode &&
	           (ax_is_web_interactive(role) || ax_is_editable_role(role, el))) {
		const char *disp = label[0] ? label : val;
		bool is_input = ax_is_editable_role(role, el);
		bool match = true;
		if (child_filter && *child_filter && !is_input) {
			match = desktop_core_str_contains_ci(disp, child_filter) ||
			        desktop_core_str_contains_ci(role, child_filter);
		}
		if (match && (disp[0] || is_input)) {
			int x = 0, y = 0, w = 0, h = 0;
			if (ax_get_bounds(el, &x, &y, &w, &h)) {
				int cx = x + w / 2, cy = y + h / 2;
				bool in_vp = ax_in_viewport(cx, cy, ctx->wx, ctx->wy, ctx->ww, ctx->wh, ctx->sw, ctx->sh);
				bool foc = false, en = true;
				ax_copy_bool_attr(el, kAXFocusedAttribute, &foc);
				ax_copy_bool_attr(el, kAXEnabledAttribute, &en);
				char line[320];
				if (in_vp) {
					snprintf(line, sizeof(line), "[%s @(%d,%d)%s%s] %.120s\n",
					         role, cx, cy, foc ? " *FOCUSED*" : "", en ? "" : " *DISABLED*",
					         disp[0] ? disp : "(unlabeled)");
				} else {
					snprintf(line, sizeof(line), "[%s *OFF-SCREEN*%s%s] %.120s\n",
					         role, foc ? " *FOCUSED*" : "", en ? "" : " *DISABLED*",
					         disp[0] ? disp : "(unlabeled)");
				}
				ax_bemit_line(ctx, line);
				if (strcmp(role, "AXButton") == 0 || strcmp(role, "AXLink") == 0 ||
				    strcmp(role, "AXHeading") == 0 || is_input) {
					skip_children = true;
				}
			}
		}
	} else if (now_in_doc && ctx->read_mode && ax_is_web_text(role)) {
		const char *txt = (strcmp(role, "AXStaticText") == 0 && val[0]) ? val : (label[0] ? label : val);
		if (txt[0]) {
			bool match = !child_filter || !*child_filter || desktop_core_str_contains_ci(txt, child_filter);
			if (match) {
				char line[384];
				if (strcmp(role, "AXStaticText") == 0)
					snprintf(line, sizeof(line), "%.360s\n", txt);
				else
					snprintf(line, sizeof(line), "[%s] %.300s\n", role, txt);
				ax_bemit_line(ctx, line);
				if (strcmp(role, "AXLink") == 0 || strcmp(role, "AXButton") == 0 || strcmp(role, "AXHeading") == 0)
					skip_children = true;
			}
		}
	}

	if (skip_children || ctx->truncated) return;

	CFTypeRef children = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
	    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount((CFArrayRef)children);
		if (n > 150) n = 150;
		for (CFIndex i = 0; i < n && !ctx->truncated; i++) {
			ax_walk_browser_collect((AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)children, i),
			                        depth + 1, now_in_doc, now_in_tabs, child_filter, ctx);
		}
	}
	if (children) CFRelease(children);
}

bool ax_core_browser_collect(const a11y_ops_t *ops, const char *filter_query,
                             bool read_mode, size_t skip_chars,
                             char *out, size_t out_sz)
{
	(void)ops;
	if (!out || out_sz == 0) return false;
	out[0] = '\0';

	ax_bwalk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.filter = filter_query;
	ctx.read_mode = read_mode;
	ctx.skip_chars = skip_chars;
	ctx.out = out;
	ctx.out_sz = out_sz;

	AXUIElementRef root = ax_resolve_browser_root(&ctx.wx, &ctx.wy, &ctx.ww, &ctx.wh, &ctx.sw, &ctx.sh);
	if (!root) return false;

	ax_walk_browser_collect(root, 0, false, false, filter_query, &ctx);
	CFRelease(root);

	if (ctx.truncated && ctx.off + 64 < out_sz) {
		snprintf(out + ctx.off, out_sz - ctx.off,
		         "\n[truncated; use 'browser read %zu' for more]", skip_chars + ctx.off);
	}
	return ctx.off > 0;
}

typedef struct {
	const char    *query;
	bool           active_match;
	AXUIElementRef found_tab;
	char           matched_title[128];
	int            visited;
} ax_btab_ctx_t;

static void ax_walk_browser_tab(AXUIElementRef el, int depth, bool in_tabs, ax_btab_ctx_t *ctx)
{
	if (!el || depth > 14 || ctx->visited >= 800 || ctx->active_match)
		return;
	ctx->visited++;

	char role[64] = {0}, label[256] = {0}, val[32] = {0};
	ax_copy_str_attr(el, kAXRoleAttribute, role, sizeof(role));
	if (strcmp(role, "AXWebArea") == 0) return;

	bool now_in_tabs = in_tabs || (strcmp(role, "AXTabGroup") == 0);
	if (strcmp(role, "AXTab") == 0 || (in_tabs && strcmp(role, "AXRadioButton") == 0)) {
		ax_best_label(el, label, sizeof(label));
		if (label[0] && desktop_core_str_contains_ci(label, ctx->query)) {
			ax_copy_str_attr(el, kAXValueAttribute, val, sizeof(val));
			bool active = (strcmp(val, "1") == 0);
			if (!active) ax_copy_bool_attr(el, kAXSelectedAttribute, &active);
			if (active) {
				ctx->active_match = true;
				snprintf(ctx->matched_title, sizeof(ctx->matched_title), "%.120s", label);
			} else if (!ctx->found_tab) {
				ctx->found_tab = (AXUIElementRef)CFRetain(el);
				snprintf(ctx->matched_title, sizeof(ctx->matched_title), "%.120s", label);
			}
		}
		return;
	}

	CFTypeRef children = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
	    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount((CFArrayRef)children);
		if (n > 64) n = 64;
		for (CFIndex i = 0; i < n && !ctx->active_match; i++) {
			ax_walk_browser_tab((AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)children, i),
			                    depth + 1, now_in_tabs, ctx);
		}
	}
	if (children) CFRelease(children);
}

bool ax_core_browser_switch_tab(const a11y_ops_t *ops, const char *query,
                                char *out_tab_title, size_t title_sz)
{
	if (out_tab_title && title_sz > 0) out_tab_title[0] = '\0';
	if (!query || !*query) return false;
	AXUIElementRef root = ax_resolve_browser_root(NULL, NULL, NULL, NULL, NULL, NULL);
	if (!root) return false;

	ax_btab_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ax_walk_browser_tab(root, 0, false, &ctx);
	CFRelease(root);

	if (ctx.active_match) {
		if (ctx.found_tab) CFRelease(ctx.found_tab);
		if (out_tab_title && title_sz > 0) snprintf(out_tab_title, title_sz, "%s", ctx.matched_title);
		return true;
	}
	if (!ctx.found_tab) return false;

	bool switched = (AXUIElementPerformAction(ctx.found_tab, kAXPressAction) == kAXErrorSuccess);
	if (!switched && ops && ops->click_xy) {
		int x = 0, y = 0, w = 0, h = 0;
		if (ax_get_bounds(ctx.found_tab, &x, &y, &w, &h))
			switched = ops->click_xy(ops->ud, x + w / 2, y + h / 2, 1);
	}
	CFRelease(ctx.found_tab);
	if (switched) {
		usleep(160000);
		if (out_tab_title && title_sz > 0) snprintf(out_tab_title, title_sz, "%s", ctx.matched_title);
	}
	return switched;
}

typedef struct {
	const char    *query;
	AXUIElementRef best;
	int            best_score;
	int            best_cx;
	int            best_cy;
	bool           best_in_vp;
	char           best_role[64];
	char           best_label[128];
	int            visited;
	int            wx, wy, ww, wh, sw, sh;
} ax_bfind_ctx_t;

static void ax_walk_browser_find(AXUIElementRef el, int depth, bool in_doc, ax_bfind_ctx_t *ctx)
{
	if (!el || depth > AX_WEB_MAX_DEPTH || ctx->visited >= AX_WEB_MAX_NODES)
		return;
	ctx->visited++;

	char role[64] = {0}, label[256] = {0}, val[128] = {0};
	ax_copy_str_attr(el, kAXRoleAttribute, role, sizeof(role));
	bool now_in_doc = in_doc || (strcmp(role, "AXWebArea") == 0);

	if (strcmp(role, "AXWindow") != 0 && strcmp(role, "AXApplication") != 0 &&
	    strcmp(role, "AXWebArea") != 0) {
		ax_best_label(el, label, sizeof(label));
		ax_copy_str_attr(el, kAXValueAttribute, val, sizeof(val));
		const char *disp = label[0] ? label : val;
		int sc = ax_score_label(disp, ctx->query);
		if (sc == 0 && ax_is_web_interactive(role) && desktop_core_str_contains_ci(role, ctx->query))
			sc = 35;
		if (sc > 0) {
			if (now_in_doc) sc += 20;
			if (ax_is_web_interactive(role)) sc += 30;
			int x = 0, y = 0, w = 0, h = 0;
			if (ax_get_bounds(el, &x, &y, &w, &h)) {
				int cx = x + w / 2, cy = y + h / 2;
				bool in_vp = ax_in_viewport(cx, cy, ctx->wx, ctx->wy, ctx->ww, ctx->wh, ctx->sw, ctx->sh);
				if (in_vp) sc += 25;
				if (sc > ctx->best_score) {
					if (ctx->best) CFRelease(ctx->best);
					ctx->best = (AXUIElementRef)CFRetain(el);
					ctx->best_score = sc;
					ctx->best_cx = cx;
					ctx->best_cy = cy;
					ctx->best_in_vp = in_vp;
					snprintf(ctx->best_role, sizeof(ctx->best_role), "%.63s", role);
					snprintf(ctx->best_label, sizeof(ctx->best_label), "%.120s", disp[0] ? disp : role);
				}
			}
		}
	}

	CFTypeRef children = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
	    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount((CFArrayRef)children);
		if (n > 100) n = 100;
		for (CFIndex i = 0; i < n; i++) {
			ax_walk_browser_find((AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)children, i),
			                     depth + 1, now_in_doc, ctx);
		}
	}
	if (children) CFRelease(children);
}

bool ax_core_browser_click(const a11y_ops_t *ops, const char *query, int btn,
                           char *out_role, size_t role_sz,
                           char *out_msg, size_t out_sz)
{
	if (out_role && role_sz > 0) out_role[0] = '\0';
	if (out_msg && out_sz > 0) out_msg[0] = '\0';
	if (!query || !*query) return false;

	if (ops && ops->focus_win) ops->focus_win(ops->ud);

	ax_bfind_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	AXUIElementRef root = ax_resolve_browser_root(&ctx.wx, &ctx.wy, &ctx.ww, &ctx.wh, &ctx.sw, &ctx.sh);
	if (!root) return false;

	ax_walk_browser_find(root, 0, false, &ctx);
	CFRelease(root);

	if (!ctx.best) return false;
	if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", ctx.best_role);

	if (!ctx.best_in_vp) {
		AXUIElementPerformAction(ctx.best, CFSTR("AXScrollToVisible"));
		usleep(180000);
		int x = 0, y = 0, w = 0, h = 0;
		if (ax_get_bounds(ctx.best, &x, &y, &w, &h)) {
			int cx = x + w / 2, cy = y + h / 2;
			if (ax_in_viewport(cx, cy, ctx.wx, ctx.wy, ctx.ww, ctx.wh, ctx.sw, ctx.sh)) {
				ctx.best_cx = cx;
				ctx.best_cy = cy;
				ctx.best_in_vp = true;
			}
		}
		if (!ctx.best_in_vp) {
			CFStringRef act = (btn == 3) ? kAXShowMenuAction : kAXPressAction;
			if (AXUIElementPerformAction(ctx.best, act) == kAXErrorSuccess) {
				usleep(90000);
				if (out_msg && out_sz > 0) {
					snprintf(out_msg, out_sz, "activated [%s] '%.64s' via native action",
					         ctx.best_role, ctx.best_label);
				}
				CFRelease(ctx.best);
				return true;
			}
			if (out_msg && out_sz > 0) {
				snprintf(out_msg, out_sz,
				         "Error: element [%s] '%.64s' is off-screen (outside viewport); scroll or filter first",
				         ctx.best_role, ctx.best_label);
			}
			CFRelease(ctx.best);
			return false;
		}
	}

	if (ax_is_editable_role(ctx.best_role, ctx.best))
		AXUIElementSetAttributeValue(ctx.best, kAXFocusedAttribute, kCFBooleanTrue);

	bool ok = false;
	if (ops && ops->click_xy) {
		ok = ops->click_xy(ops->ud, ctx.best_cx, ctx.best_cy, btn);
	}
	if (!ok) {
		CFStringRef act = (btn == 3) ? kAXShowMenuAction : kAXPressAction;
		ok = (AXUIElementPerformAction(ctx.best, act) == kAXErrorSuccess);
	}
	usleep(90000);
	if (out_msg && out_sz > 0) {
		snprintf(out_msg, out_sz, "%s [%s] '%.64s' at (%d,%d)",
		         ok ? "clicked" : "Error: failed to click",
		         ctx.best_role, ctx.best_label, ctx.best_cx, ctx.best_cy);
	}
	CFRelease(ctx.best);
	return ok;
}

typedef struct {
	char role[64];
	char label[128];
	int  cx;
	int  cy;
	bool multi_line;
} ax_bedit_cand_t;

typedef struct {
	a11y_browser_input_t focused;
	ax_bedit_cand_t      cands[16];
	int                  n_cands;
	int                  visited;
	int                  wx, wy, ww, wh, sw, sh;
} ax_binput_ctx_t;

static void ax_walk_browser_inputs(AXUIElementRef el, int depth, bool in_doc, ax_binput_ctx_t *ctx)
{
	if (!el || depth > AX_WEB_MAX_DEPTH || ctx->visited >= 2800 || ctx->focused.found)
		return;
	ctx->visited++;

	char role[64] = {0}, label[128] = {0};
	ax_copy_str_attr(el, kAXRoleAttribute, role, sizeof(role));
	bool now_in_doc = in_doc || (strcmp(role, "AXWebArea") == 0);
	bool skip_children = false;

	if (now_in_doc && strcmp(role, "AXWebArea") != 0 && ax_is_editable_role(role, el)) {
		bool en = true;
		ax_copy_bool_attr(el, kAXEnabledAttribute, &en);
		if (en) {
			bool foc = false;
			ax_copy_bool_attr(el, kAXFocusedAttribute, &foc);
			ax_best_label(el, label, sizeof(label));
			const char *disp = label[0] ? label : "(unlabeled)";
			int x = 0, y = 0, w = 0, h = 0;
			bool have_c = ax_get_bounds(el, &x, &y, &w, &h);
			int cx = have_c ? (x + w / 2) : 0;
			int cy = have_c ? (y + h / 2) : 0;
			bool in_vp = have_c && ax_in_viewport(cx, cy, ctx->wx, ctx->wy, ctx->ww, ctx->wh, ctx->sw, ctx->sh);
			if (foc) {
				ctx->focused.found = true;
				ctx->focused.editable = true;
				snprintf(ctx->focused.role, sizeof(ctx->focused.role), "%.63s", role);
				snprintf(ctx->focused.label, sizeof(ctx->focused.label), "%.127s", disp);
				ctx->focused.cx = cx;
				ctx->focused.cy = cy;
			} else if (in_vp && ctx->n_cands < 16) {
				ax_bedit_cand_t *c = &ctx->cands[ctx->n_cands++];
				snprintf(c->role, sizeof(c->role), "%.63s", role);
				snprintf(c->label, sizeof(c->label), "%.127s", disp);
				c->cx = cx;
				c->cy = cy;
				c->multi_line = (strcmp(role, "AXTextArea") == 0);
			}
			skip_children = true;
		}
	}

	if (skip_children || ctx->focused.found) return;

	CFTypeRef children = NULL;
	if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &children) == kAXErrorSuccess &&
	    children && CFGetTypeID(children) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount((CFArrayRef)children);
		if (n > 100) n = 100;
		for (CFIndex i = 0; i < n && !ctx->focused.found; i++) {
			ax_walk_browser_inputs((AXUIElementRef)CFArrayGetValueAtIndex((CFArrayRef)children, i),
			                       depth + 1, now_in_doc, ctx);
		}
	}
	if (children) CFRelease(children);
}

bool ax_core_browser_ensure_editable(const a11y_ops_t *ops, bool focus_if_needed,
                                     a11y_browser_input_t *out_info,
                                     char *out_err, size_t err_sz)
{
	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (out_err && err_sz > 0) out_err[0] = '\0';

	if (focus_if_needed && ops && ops->is_win_focused && !ops->is_win_focused(ops->ud)) {
		if (ops->focus_win) ops->focus_win(ops->ud);
		usleep(40000);
		if (!ops->is_win_focused(ops->ud)) {
			if (out_err && err_sz > 0)
				snprintf(out_err, err_sz, "Error: browser window is not focused");
			return false;
		}
	}

	ax_binput_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	AXUIElementRef root = ax_resolve_browser_root(&ctx.wx, &ctx.wy, &ctx.ww, &ctx.wh, &ctx.sw, &ctx.sh);
	if (!root) {
		if (out_err && err_sz > 0) snprintf(out_err, err_sz, "Error: browser accessibility unavailable");
		return false;
	}
	ax_walk_browser_inputs(root, 0, false, &ctx);
	CFRelease(root);

	if (ctx.focused.found) {
		if (out_info) *out_info = ctx.focused;
		return true;
	}
	if (!focus_if_needed) return false;

	if (ctx.n_cands == 0) {
		if (out_err && err_sz > 0)
			snprintf(out_err, err_sz, "Error: no visible editable input found on page");
		return false;
	}
	int chosen = (ctx.n_cands == 1) ? 0 : -1;
	if (chosen < 0) {
		int multi_idx = -1, n_multi = 0;
		for (int i = 0; i < ctx.n_cands; i++) {
			if (ctx.cands[i].multi_line) {
				multi_idx = i;
				n_multi++;
			}
		}
		if (n_multi == 1) chosen = multi_idx;
	}
	if (chosen >= 0) {
		const ax_bedit_cand_t *c = &ctx.cands[chosen];
		if (ops && ops->click_xy) {
			ops->click_xy(ops->ud, c->cx, c->cy, 1);
			usleep(80000);
		}
		if (out_info) {
			out_info->found = true;
			out_info->editable = true;
			snprintf(out_info->role, sizeof(out_info->role), "%s", c->role);
			snprintf(out_info->label, sizeof(out_info->label), "%s", c->label);
			out_info->cx = c->cx;
			out_info->cy = c->cy;
		}
		return true;
	}
	if (out_err && err_sz > 0) {
		size_t off = (size_t)snprintf(
			out_err, err_sz,
			"Error: no input focused and %d editable inputs visible (", ctx.n_cands);
		for (int i = 0; i < ctx.n_cands && i < 4 && off + 48 < err_sz; i++) {
			off += (size_t)snprintf(
				out_err + off, err_sz - off,
				"%s[%s '%.32s']", i > 0 ? ", " : "",
				ctx.cands[i].role, ctx.cands[i].label);
		}
		snprintf(out_err + off, err_sz - off, "); click or specify target first");
	}
	return false;
}
