#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "a11y_core.h"
#include "../../include/ttypt/desktop-core.h"

#include <windows.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UIA_MAX_NODES 500
#define UIA_MAX_DEPTH 14

static const GUID jv_CLSID_CUIAutomation = {
	0xff48dba4, 0x60ef, 0x4201, {0xaa,0x87,0x54,0x10,0x3e,0xef,0x59,0x4e}
};
static const GUID jv_IID_IUIAutomation = {
	0x30cbe57d, 0xd9d0, 0x452a, {0xab,0x13,0x7a,0xc5,0xac,0x48,0x25,0xee}
};
static const GUID jv_IID_IUIAutomationInvokePattern = {
	0xfb377fbe, 0x8ea6, 0x46d5, {0x9c,0x73,0x64,0x99,0x64,0x2d,0x30,0x59}
};
static const GUID jv_IID_IUIAutomationValuePattern = {
	0xa94cd8b1, 0x0844, 0x4cd6, {0x9d,0x2d,0x64,0x05,0x37,0xab,0x39,0xe9}
};
static const GUID jv_IID_IUIAutomationRangeValuePattern = {
	0x59213f4f, 0x7346, 0x49e5, {0xb1,0x20,0x80,0x55,0x59,0x87,0xa1,0x48}
};

static void bstr_to_utf8(BSTR b, char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';
	if (!b) return;
	int len = SysStringLen(b);
	if (len <= 0) return;
	int n = WideCharToMultiByte(CP_UTF8, 0, b, len, out, (int)cap - 1, NULL, NULL);
	if (n > 0)
		out[n] = '\0';
}

static const char *uia_role_name(CONTROLTYPEID ct)
{
	switch (ct) {
	case UIA_ButtonControlTypeId: return "Button";
	case UIA_CalendarControlTypeId: return "Calendar";
	case UIA_CheckBoxControlTypeId: return "CheckBox";
	case UIA_ComboBoxControlTypeId: return "ComboBox";
	case UIA_EditControlTypeId: return "Edit";
	case UIA_HyperlinkControlTypeId: return "Hyperlink";
	case UIA_ImageControlTypeId: return "Image";
	case UIA_ListItemControlTypeId: return "ListItem";
	case UIA_ListControlTypeId: return "List";
	case UIA_MenuControlTypeId: return "Menu";
	case UIA_MenuBarControlTypeId: return "MenuBar";
	case UIA_MenuItemControlTypeId: return "MenuItem";
	case UIA_ProgressBarControlTypeId: return "ProgressBar";
	case UIA_RadioButtonControlTypeId: return "RadioButton";
	case UIA_ScrollBarControlTypeId: return "ScrollBar";
	case UIA_SliderControlTypeId: return "Slider";
	case UIA_SpinnerControlTypeId: return "Spinner";
	case UIA_StatusBarControlTypeId: return "StatusBar";
	case UIA_TabControlTypeId: return "Tab";
	case UIA_TabItemControlTypeId: return "TabItem";
	case UIA_TextControlTypeId: return "Text";
	case UIA_ToolBarControlTypeId: return "ToolBar";
	case UIA_ToolTipControlTypeId: return "ToolTip";
	case UIA_TreeControlTypeId: return "Tree";
	case UIA_TreeItemControlTypeId: return "TreeItem";
	case UIA_GroupControlTypeId: return "Group";
	case UIA_DataGridControlTypeId: return "DataGrid";
	case UIA_DataItemControlTypeId: return "DataItem";
	default: return "Control";
	}
}

static bool uia_is_interactive_ct(CONTROLTYPEID ct)
{
	switch (ct) {
	case UIA_ButtonControlTypeId:
	case UIA_CheckBoxControlTypeId:
	case UIA_ComboBoxControlTypeId:
	case UIA_EditControlTypeId:
	case UIA_HyperlinkControlTypeId:
	case UIA_ListItemControlTypeId:
	case UIA_MenuItemControlTypeId:
	case UIA_RadioButtonControlTypeId:
	case UIA_SliderControlTypeId:
	case UIA_SpinnerControlTypeId:
	case UIA_TabItemControlTypeId:
	case UIA_TreeItemControlTypeId:
		return true;
	default:
		return false;
	}
}

static bool uia_is_editable_ct(CONTROLTYPEID ct, IUIAutomationElement *el)
{
	if (ct == UIA_EditControlTypeId || ct == UIA_ComboBoxControlTypeId)
		return true;
	IUIAutomationValuePattern *vp = NULL;
	if (el && SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_ValuePatternId,
	                                                           &jv_IID_IUIAutomationValuePattern,
	                                                           (void **)&vp)) && vp) {
		BOOL ro = TRUE;
		IUIAutomationValuePattern_get_CurrentIsReadOnly(vp, &ro);
		IUIAutomationValuePattern_Release(vp);
		return ro ? false : true;
	}
	return false;
}

static void uia_get_el_strings(IUIAutomationElement *el, CONTROLTYPEID *out_ct,
                               char *label, size_t lcap, char *val, size_t vcap)
{
	if (out_ct) *out_ct = 0;
	if (label && lcap > 0) label[0] = '\0';
	if (val && vcap > 0) val[0] = '\0';
	if (!el) return;

	if (out_ct)
		IUIAutomationElement_get_CurrentControlType(el, out_ct);

	if (label && lcap > 0) {
		BSTR bname = NULL;
		if (SUCCEEDED(IUIAutomationElement_get_CurrentName(el, &bname)) && bname) {
			bstr_to_utf8(bname, label, lcap);
			SysFreeString(bname);
		}
	}

	if (val && vcap > 0) {
		IUIAutomationValuePattern *vp = NULL;
		if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_ValuePatternId,
		                                                       &jv_IID_IUIAutomationValuePattern,
		                                                       (void **)&vp)) && vp) {
			BSTR bv = NULL;
			if (SUCCEEDED(IUIAutomationValuePattern_get_CurrentValue(vp, &bv)) && bv) {
				bstr_to_utf8(bv, val, vcap);
				SysFreeString(bv);
			}
			IUIAutomationValuePattern_Release(vp);
		}
	}
}

static int uia_score_label(const char *label, const char *query)
{
	if (!label || !*label || !query || !*query) return 0;
	if (_stricmp(label, query) == 0) return 100;
	size_t qlen = strlen(query);
	if (_strnicmp(label, query, qlen) == 0) return 80;
	if (desktop_core_str_contains_ci(label, query)) return 50;
	return 0;
}

typedef struct {
	unsigned long pid;
	const char   *title;
	HWND          found;
} find_hwnd_ctx_t;

static BOOL CALLBACK enum_hwnd_cb(HWND hwnd, LPARAM lp)
{
	find_hwnd_ctx_t *ctx = (find_hwnd_ctx_t *)lp;
	if (!IsWindowVisible(hwnd)) return TRUE;
	DWORD wpid = 0;
	GetWindowThreadProcessId(hwnd, &wpid);
	if (ctx->pid > 0 && wpid != (DWORD)ctx->pid) return TRUE;
	if (ctx->title && ctx->title[0]) {
		char wtitle[256] = {0};
		GetWindowTextA(hwnd, wtitle, sizeof(wtitle));
		if (!desktop_core_str_contains_ci(wtitle, ctx->title))
			return TRUE;
	}
	ctx->found = hwnd;
	return FALSE;
}

static HWND uia_find_hwnd(const a11y_win_ctx_t *win)
{
	if (win && (win->pid > 0 || (win->title && win->title[0]))) {
		find_hwnd_ctx_t ctx = { win->pid, win->title, NULL };
		EnumWindows(enum_hwnd_cb, (LPARAM)&ctx);
		if (ctx.found) return ctx.found;
	}
	return GetForegroundWindow();
}

static bool uia_open_root(const a11y_win_ctx_t *win, IUIAutomation **out_uia,
                          IUIAutomationTreeWalker **out_walker, IUIAutomationElement **out_root,
                          bool *out_uninit)
{
	*out_uia = NULL;
	*out_walker = NULL;
	*out_root = NULL;
	*out_uninit = false;

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (SUCCEEDED(hr)) *out_uninit = true;
	else if (hr != RPC_E_CHANGED_MODE) return false;

	hr = CoCreateInstance(&jv_CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
	                      &jv_IID_IUIAutomation, (void **)out_uia);
	if (FAILED(hr) || !*out_uia) {
		if (*out_uninit) { CoUninitialize(); *out_uninit = false; }
		return false;
	}

	IUIAutomation_get_ControlViewWalker(*out_uia, out_walker);
	HWND hw = uia_find_hwnd(win);
	if (hw)
		IUIAutomation_ElementFromHandle(*out_uia, (UIA_HWND)hw, out_root);
	if (!*out_root)
		IUIAutomation_GetRootElement(*out_uia, out_root);

	if (!*out_walker || !*out_root) {
		if (*out_root) { IUIAutomationElement_Release(*out_root); *out_root = NULL; }
		if (*out_walker) { IUIAutomationTreeWalker_Release(*out_walker); *out_walker = NULL; }
		IUIAutomation_Release(*out_uia);
		*out_uia = NULL;
		if (*out_uninit) { CoUninitialize(); *out_uninit = false; }
		return false;
	}
	return true;
}

static void uia_close_root(IUIAutomation *uia, IUIAutomationTreeWalker *walker,
                           IUIAutomationElement *root, bool uninit)
{
	if (root) IUIAutomationElement_Release(root);
	if (walker) IUIAutomationTreeWalker_Release(walker);
	if (uia) IUIAutomation_Release(uia);
	if (uninit) CoUninitialize();
}

typedef struct {
	bool                     inspect_mode;
	const char              *filter;
	char                    *out;
	size_t                   out_sz;
	size_t                   off;
	int                      visited;
	IUIAutomationTreeWalker *walker;
} uia_read_ctx_t;

static void uia_walk_read(IUIAutomationElement *el, int depth, uia_read_ctx_t *ctx)
{
	if (!el || depth > UIA_MAX_DEPTH || ctx->visited >= UIA_MAX_NODES ||
	    ctx->off + 256 >= ctx->out_sz)
		return;
	ctx->visited++;

	CONTROLTYPEID ct = 0;
	char label[256] = {0}, val[256] = {0};
	uia_get_el_strings(el, &ct, label, sizeof(label), val, sizeof(val));
	const char *role = uia_role_name(ct);

	RECT rc = {0, 0, 0, 0};
	bool has_rect = SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(el, &rc)) &&
	                (rc.right > rc.left && rc.bottom > rc.top);

	bool interesting = ctx->inspect_mode ? (label[0] || val[0] || uia_is_interactive_ct(ct))
	                                     : (uia_is_interactive_ct(ct) || label[0] || val[0]);
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
				             "%*s[%s] \"%s\"%s%s%s (%ld,%ld %ldx%ld)\n",
				             depth > 4 ? 8 : depth * 2, "",
				             role, label,
				             val[0] ? " val=\"" : "", val[0] ? val : "", val[0] ? "\"" : "",
				             rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
			} else if (label[0] || val[0]) {
				n = snprintf(ctx->out + ctx->off, ctx->out_sz - ctx->off,
				             "[%s] %s%s%s\n",
				             role,
				             label[0] ? label : val,
				             (label[0] && val[0] && strcmp(label, val) != 0) ? " = " : "",
				             (label[0] && val[0] && strcmp(label, val) != 0) ? val : "");
			}
			if (n > 0) {
				if ((size_t)n >= ctx->out_sz - ctx->off) ctx->off = ctx->out_sz - 1;
				else ctx->off += (size_t)n;
			}
		}
	}

	IUIAutomationElement *ch = NULL;
	if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(ctx->walker, el, &ch)) && ch) {
		while (ch && ctx->visited < UIA_MAX_NODES && ctx->off + 256 < ctx->out_sz) {
			uia_walk_read(ch, depth + 1, ctx);
			IUIAutomationElement *next = NULL;
			HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(ctx->walker, ch, &next);
			IUIAutomationElement_Release(ch);
			ch = SUCCEEDED(nhr) ? next : NULL;
		}
		if (ch) IUIAutomationElement_Release(ch);
	}
}

typedef struct {
	const char              *query;
	bool                     editable_only;
	IUIAutomationElement    *best;
	int                      best_score;
	char                     best_role[64];
	char                     best_label[256];
	int                      visited;
	IUIAutomationTreeWalker *walker;
} uia_find_ctx_t;

static void uia_walk_find(IUIAutomationElement *el, int depth, uia_find_ctx_t *ctx)
{
	if (!el || depth > UIA_MAX_DEPTH || ctx->visited >= UIA_MAX_NODES)
		return;
	ctx->visited++;

	CONTROLTYPEID ct = 0;
	char label[256] = {0}, val[128] = {0};
	uia_get_el_strings(el, &ct, label, sizeof(label), val, sizeof(val));
	const char *role = uia_role_name(ct);

	bool ok_kind = ctx->editable_only ? uia_is_editable_ct(ct, el) : true;
	if (ok_kind) {
		int sc = uia_score_label(label, ctx->query);
		int vsc = uia_score_label(val, ctx->query);
		if (vsc > sc) sc = vsc;
		if (sc > 0 && uia_is_interactive_ct(ct)) sc += 5;
		if (sc > ctx->best_score) {
			if (ctx->best) IUIAutomationElement_Release(ctx->best);
			ctx->best = el;
			IUIAutomationElement_AddRef(ctx->best);
			ctx->best_score = sc;
			snprintf(ctx->best_role, sizeof(ctx->best_role), "%s", role);
			snprintf(ctx->best_label, sizeof(ctx->best_label), "%s", label[0] ? label : val);
		}
	}

	IUIAutomationElement *ch = NULL;
	if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(ctx->walker, el, &ch)) && ch) {
		while (ch && ctx->visited < UIA_MAX_NODES) {
			uia_walk_find(ch, depth + 1, ctx);
			IUIAutomationElement *next = NULL;
			HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(ctx->walker, ch, &next);
			IUIAutomationElement_Release(ch);
			ch = SUCCEEDED(nhr) ? next : NULL;
		}
		if (ch) IUIAutomationElement_Release(ch);
	}
}

bool uia_core_read_window(const a11y_win_ctx_t *win, bool inspect_mode,
                          const char *filter, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';

	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	if (!uia_open_root(win, &uia, &walker, &root, &uninit))
		return false;

	uia_read_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.inspect_mode = inspect_mode;
	ctx.filter = filter;
	ctx.out = out;
	ctx.out_sz = out_sz;
	ctx.walker = walker;
	uia_walk_read(root, 0, &ctx);

	uia_close_root(uia, walker, root, uninit);
	return out[0] != '\0';
}

bool uia_core_click(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                    const char *query, int btn, char *out_msg, size_t out_sz)
{
	if (!query || !*query) {
		snprintf(out_msg, out_sz, "Error: empty ui click target");
		return false;
	}
	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	if (!uia_open_root(win, &uia, &walker, &root, &uninit)) {
		snprintf(out_msg, out_sz, "Error: UI Automation unavailable");
		return false;
	}

	uia_find_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ctx.walker = walker;
	uia_walk_find(root, 0, &ctx);

	bool ok = false;
	if (!ctx.best) {
		snprintf(out_msg, out_sz, "Error: ui target not found: %.160s", query);
	} else {
		if (ops && ops->is_win_focused && ops->focus_win && !ops->is_win_focused(ops->ud)) {
			ops->focus_win(ops->ud);
			Sleep(80);
		}
		if (btn != 3) {
			IUIAutomationInvokePattern *inv = NULL;
			if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.best, UIA_InvokePatternId,
			                                                       &jv_IID_IUIAutomationInvokePattern,
			                                                       (void **)&inv)) && inv) {
				ok = SUCCEEDED(IUIAutomationInvokePattern_Invoke(inv));
				IUIAutomationInvokePattern_Release(inv);
			}
		}
		if (!ok && ops && ops->click_xy) {
			RECT rc = {0, 0, 0, 0};
			if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(ctx.best, &rc)) &&
			    rc.right > rc.left && rc.bottom > rc.top) {
				ok = ops->click_xy(ops->ud, (int)((rc.left + rc.right) / 2),
				                   (int)((rc.top + rc.bottom) / 2), btn);
			}
		}
		snprintf(out_msg, out_sz, "%s [%s '%.80s']",
		         ok ? "clicked" : "Error: failed to click",
		         ctx.best_role[0] ? ctx.best_role : "Control",
		         ctx.best_label[0] ? ctx.best_label : query);
		IUIAutomationElement_Release(ctx.best);
	}

	uia_close_root(uia, walker, root, uninit);
	return ok;
}

bool uia_core_focus(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                    const char *query, char *out_msg, size_t out_sz)
{
	if (!query || !*query) {
		snprintf(out_msg, out_sz, "Error: empty ui focus target");
		return false;
	}
	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	if (!uia_open_root(win, &uia, &walker, &root, &uninit)) {
		snprintf(out_msg, out_sz, "Error: UI Automation unavailable");
		return false;
	}

	uia_find_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ctx.walker = walker;
	uia_walk_find(root, 0, &ctx);

	bool ok = false;
	if (!ctx.best) {
		snprintf(out_msg, out_sz, "Error: ui target not found: %.160s", query);
	} else {
		if (ops && ops->is_win_focused && ops->focus_win && !ops->is_win_focused(ops->ud)) {
			ops->focus_win(ops->ud);
			Sleep(80);
		}
		ok = SUCCEEDED(IUIAutomationElement_SetFocus(ctx.best));
		if (!ok && ops && ops->click_xy) {
			RECT rc = {0, 0, 0, 0};
			if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(ctx.best, &rc)) &&
			    rc.right > rc.left && rc.bottom > rc.top) {
				ok = ops->click_xy(ops->ud, (int)((rc.left + rc.right) / 2),
				                   (int)((rc.top + rc.bottom) / 2), 1);
			}
		}
		snprintf(out_msg, out_sz, "%s [%s '%.80s']",
		         ok ? "focused" : "Error: failed to focus",
		         ctx.best_role[0] ? ctx.best_role : "Control",
		         ctx.best_label[0] ? ctx.best_label : query);
		IUIAutomationElement_Release(ctx.best);
	}

	uia_close_root(uia, walker, root, uninit);
	return ok;
}

bool uia_core_prepare_input(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
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

	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	if (!uia_open_root(win, &uia, &walker, &root, &uninit)) {
		if (out_err && err_sz > 0) snprintf(out_err, err_sz, "Error: UI Automation unavailable");
		return false;
	}

	bool ok = false;
	if (target && *target) {
		uia_find_ctx_t ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.query = target;
		ctx.editable_only = true;
		ctx.walker = walker;
		uia_walk_find(root, 0, &ctx);
		if (!ctx.best) {
			memset(&ctx, 0, sizeof(ctx));
			ctx.query = target;
			ctx.walker = walker;
			uia_walk_find(root, 0, &ctx);
		}
		if (!ctx.best) {
			if (out_err && err_sz > 0)
				snprintf(out_err, err_sz, "Error: input field not found matching '%.160s'", target);
		} else {
			if (ops && ops->is_win_focused && ops->focus_win && !ops->is_win_focused(ops->ud)) {
				ops->focus_win(ops->ud);
				Sleep(80);
			}
			ok = SUCCEEDED(IUIAutomationElement_SetFocus(ctx.best));
			if (!ok && ops && ops->click_xy) {
				RECT rc = {0, 0, 0, 0};
				if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(ctx.best, &rc)) &&
				    rc.right > rc.left && rc.bottom > rc.top) {
					ok = ops->click_xy(ops->ud, (int)((rc.left + rc.right) / 2),
					                   (int)((rc.top + rc.bottom) / 2), 1);
				}
			}
			if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", ctx.best_role);
			if (out_label && label_sz > 0) snprintf(out_label, label_sz, "%s", ctx.best_label);
			if (!ok && out_err && err_sz > 0)
				snprintf(out_err, err_sz, "Error: failed to focus input field '%.160s'", target);
			IUIAutomationElement_Release(ctx.best);
		}
	} else {
		IUIAutomationElement *foc = NULL;
		if (SUCCEEDED(IUIAutomation_GetFocusedElement(uia, &foc)) && foc) {
			CONTROLTYPEID ct = 0;
			char label[128] = {0};
			uia_get_el_strings(foc, &ct, label, sizeof(label), NULL, 0);
			const char *role = uia_role_name(ct);
			ok = uia_is_editable_ct(ct, foc);
			if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", role);
			if (out_label && label_sz > 0) snprintf(out_label, label_sz, "%s", label);
			if (!ok && out_err && err_sz > 0)
				snprintf(out_err, err_sz, "Error: focused control [%s] is not editable", role);
			IUIAutomationElement_Release(foc);
		} else if (out_err && err_sz > 0) {
			snprintf(out_err, err_sz, "Error: no editable input focused");
		}
	}

	uia_close_root(uia, walker, root, uninit);
	return ok;
}

bool uia_core_set(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                  const char *query, const char *value, char *out_msg, size_t out_sz)
{
	if (!query || !*query || !value) {
		snprintf(out_msg, out_sz, "Error: usage: ui set <target> | <value>");
		return false;
	}
	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	if (!uia_open_root(win, &uia, &walker, &root, &uninit)) {
		snprintf(out_msg, out_sz, "Error: UI Automation unavailable");
		return false;
	}

	uia_find_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ctx.walker = walker;
	uia_walk_find(root, 0, &ctx);

	bool ok = false;
	if (!ctx.best) {
		snprintf(out_msg, out_sz, "Error: ui target not found: %.160s", query);
	} else {
		IUIAutomationRangeValuePattern *rv = NULL;
		IUIAutomationValuePattern *vp = NULL;
		if (strcmp(ctx.best_role, "Slider") == 0 &&
		    SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.best, UIA_RangeValuePatternId,
		                                                       &jv_IID_IUIAutomationRangeValuePattern,
		                                                       (void **)&rv)) && rv) {
			ok = SUCCEEDED(IUIAutomationRangeValuePattern_SetValue(rv, strtod(value, NULL)));
			IUIAutomationRangeValuePattern_Release(rv);
		}
		if (!ok &&
		    SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.best, UIA_ValuePatternId,
		                                                       &jv_IID_IUIAutomationValuePattern,
		                                                       (void **)&vp)) && vp) {
			int wlen = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
			if (wlen > 0) {
				WCHAR *wbuf = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
				if (wbuf) {
					MultiByteToWideChar(CP_UTF8, 0, value, -1, wbuf, wlen);
					BSTR bval = SysAllocString(wbuf);
					if (bval) {
						ok = SUCCEEDED(IUIAutomationValuePattern_SetValue(vp, bval));
						SysFreeString(bval);
					}
					free(wbuf);
				}
			}
			IUIAutomationValuePattern_Release(vp);
		}
		if (!ok && ops && ops->send_key && ops->type_text &&
		    SUCCEEDED(IUIAutomationElement_SetFocus(ctx.best))) {
			ops->send_key(ops->ud, "ctrl+a");
			ok = ops->type_text(ops->ud, value, false);
		}
		snprintf(out_msg, out_sz, "%s [%s '%.80s'] = '%.80s'",
		         ok ? "set" : "Error: failed to set",
		         ctx.best_role[0] ? ctx.best_role : "Control",
		         ctx.best_label[0] ? ctx.best_label : query,
		         value);
		IUIAutomationElement_Release(ctx.best);
	}

	uia_close_root(uia, walker, root, uninit);
	return ok;
}
