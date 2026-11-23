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
