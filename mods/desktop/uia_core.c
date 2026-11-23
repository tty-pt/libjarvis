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
#define UIA_WEB_MAX_NODES 3500
#define UIA_WEB_MAX_DEPTH 28

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
static const GUID jv_IID_IUIAutomationScrollItemPattern = {
	0xb488300f, 0xd015, 0x4f19, {0x9c,0x29,0xbb,0x59,0x5e,0x36,0x45,0xef}
};
static const GUID jv_IID_IUIAutomationSelectionItemPattern = {
	0xa8efa66a, 0x0fda, 0x421a, {0x91,0x94,0x38,0x02,0x1f,0x35,0x78,0xea}
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

static void uia_get_hwnd_exe(HWND hwnd, char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';
	if (!hwnd) return;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid) return;
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h) {
		char path[MAX_PATH] = {0};
		DWORD sz = sizeof(path);
		if (QueryFullProcessImageNameA(h, 0, path, &sz) && sz > 0) {
			const char *base = strrchr(path, '\\');
			snprintf(out, cap, "%s", base ? base + 1 : path);
			char *dot = strrchr(out, '.');
			if (dot && _stricmp(dot, ".exe") == 0) *dot = '\0';
		}
		CloseHandle(h);
	}
	if (!out[0])
		GetWindowTextA(hwnd, out, (int)cap);
}

static BOOL CALLBACK enum_browser_hwnd_cb(HWND hwnd, LPARAM lp)
{
	HWND *out_hwnd = (HWND *)lp;
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
	LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
	if (ex & WS_EX_TOOLWINDOW) return TRUE;
	char exe[128] = {0};
	uia_get_hwnd_exe(hwnd, exe, sizeof(exe));
	if (desktop_core_is_browser_name(exe)) {
		*out_hwnd = hwnd;
		return FALSE;
	}
	return TRUE;
}

static HWND uia_find_browser_hwnd(void)
{
	HWND fg = GetForegroundWindow();
	if (fg && IsWindowVisible(fg) && !IsIconic(fg)) {
		char exe[128] = {0};
		uia_get_hwnd_exe(fg, exe, sizeof(exe));
		if (desktop_core_is_browser_name(exe))
			return fg;
	}
	HWND found = NULL;
	EnumWindows(enum_browser_hwnd_cb, (LPARAM)&found);
	return found;
}

static bool uia_open_browser_root(IUIAutomation **out_uia, IUIAutomationTreeWalker **out_walker,
                                  IUIAutomationElement **out_root, DWORD *out_pid,
                                  RECT *out_win_rc, int *out_sw, int *out_sh,
                                  bool *out_uninit)
{
	*out_uia = NULL;
	*out_walker = NULL;
	*out_root = NULL;
	if (out_pid) *out_pid = 0;
	if (out_win_rc) memset(out_win_rc, 0, sizeof(*out_win_rc));
	if (out_sw) *out_sw = GetSystemMetrics(SM_CXSCREEN);
	if (out_sh) *out_sh = GetSystemMetrics(SM_CYSCREEN);
	*out_uninit = false;

	HWND hw = uia_find_browser_hwnd();
	if (!hw) return false;

	if (out_pid) GetWindowThreadProcessId(hw, out_pid);
	if (out_win_rc) GetWindowRect(hw, out_win_rc);

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
	IUIAutomation_ElementFromHandle(*out_uia, (UIA_HWND)hw, out_root);
	if (!*out_walker || !*out_root) {
		uia_close_root(*out_uia, *out_walker, *out_root, *out_uninit);
		*out_uia = NULL;
		*out_walker = NULL;
		*out_root = NULL;
		*out_uninit = false;
		return false;
	}
	return true;
}

static void uia_clean_text(const char *raw, char *clean, size_t clean_sz)
{
	if (!clean || clean_sz == 0) return;
	size_t cw = 0;
	if (raw) {
		for (const unsigned char *p = (const unsigned char *)raw; *p && cw + 2 < clean_sz; ) {
			if (*p == '\n' || *p == '\r' || *p == '\t') {
				if (cw > 0 && clean[cw - 1] != ' ') clean[cw++] = ' ';
				p++;
				continue;
			}
			clean[cw++] = (char)*p++;
		}
	}
	while (cw > 0 && clean[cw - 1] == ' ') cw--;
	clean[cw] = '\0';
}

static bool uia_in_viewport(int x, int y, const RECT *win_rc, int sw, int sh)
{
	if (x < 0 || y < 0) return false;
	if (win_rc && win_rc->right > win_rc->left && win_rc->bottom > win_rc->top) {
		if (x < win_rc->left || x >= win_rc->right ||
		    y < win_rc->top || y >= win_rc->bottom) {
			return false;
		}
	}
	if (sw > 0 && sh > 0 && (x >= sw || y >= sh)) return false;
	return true;
}

static bool uia_is_web_interactive(CONTROLTYPEID ct)
{
	return uia_is_interactive_ct(ct) || ct == UIA_DataItemControlTypeId;
}

static bool uia_is_web_text(CONTROLTYPEID ct)
{
	switch (ct) {
	case UIA_TextControlTypeId:
	case UIA_HyperlinkControlTypeId:
	case UIA_ButtonControlTypeId:
	case UIA_EditControlTypeId:
	case UIA_ComboBoxControlTypeId:
	case UIA_DataItemControlTypeId:
	case UIA_ListItemControlTypeId:
		return true;
	default:
		return false;
	}
}

static void uia_walk_browser_probe(IUIAutomationTreeWalker *walker, IUIAutomationElement *el,
                                   int depth, int *visited, a11y_browser_doc_t *st)
{
	if (!el || !st || st->found || depth > UIA_WEB_MAX_DEPTH || *visited >= 1200)
		return;
	(*visited)++;

	CONTROLTYPEID ct = 0;
	IUIAutomationElement_get_CurrentControlType(el, &ct);
	if (ct == UIA_DocumentControlTypeId) {
		BOOL offscreen = FALSE;
		IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);
		if (!offscreen) {
			st->found = true;
			st->showing = true;
			st->defunct = false;
			st->busy = false;
			uia_get_el_strings(el, NULL, st->title, sizeof(st->title), st->url, sizeof(st->url));
			IUIAutomationElement *ch = NULL;
			if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(walker, el, &ch)) && ch) {
				int cnt = 0;
				while (ch && cnt < 64) {
					cnt++;
					IUIAutomationElement *next = NULL;
					HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(walker, ch, &next);
					IUIAutomationElement_Release(ch);
					ch = SUCCEEDED(nhr) ? next : NULL;
				}
				if (ch) IUIAutomationElement_Release(ch);
				st->child_count = cnt;
			}
		}
		return;
	}

	IUIAutomationElement *ch = NULL;
	if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(walker, el, &ch)) && ch) {
		int idx = 0;
		while (ch && !st->found && idx < 64) {
			idx++;
			uia_walk_browser_probe(walker, ch, depth + 1, visited, st);
			IUIAutomationElement *next = NULL;
			HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(walker, ch, &next);
			IUIAutomationElement_Release(ch);
			ch = SUCCEEDED(nhr) ? next : NULL;
		}
		if (ch) IUIAutomationElement_Release(ch);
	}
}

bool uia_core_browser_probe_doc(a11y_browser_doc_t *out_doc)
{
	if (!out_doc) return false;
	memset(out_doc, 0, sizeof(*out_doc));

	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	if (!uia_open_browser_root(&uia, &walker, &root, NULL, NULL, NULL, NULL, &uninit))
		return false;

	int visited = 0;
	uia_walk_browser_probe(walker, root, 0, &visited, out_doc);
	if (out_doc->found && !out_doc->title[0])
		uia_get_el_strings(root, NULL, out_doc->title, sizeof(out_doc->title), NULL, 0);

	uia_close_root(uia, walker, root, uninit);
	return out_doc->found;
}

typedef struct {
	const char              *filter;
	bool                     read_mode;
	size_t                   skip_chars;
	size_t                   seen_chars;
	bool                     truncated;
	char                    *out;
	size_t                   out_sz;
	size_t                   off;
	int                      visited;
	RECT                     win_rc;
	int                      sw, sh;
	IUIAutomationTreeWalker *walker;
} uia_bwalk_ctx_t;

static void uia_bemit_line(uia_bwalk_ctx_t *ctx, const char *line)
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

static void uia_walk_browser_collect(IUIAutomationElement *el, int depth, bool in_doc,
                                     const char *filter, uia_bwalk_ctx_t *ctx)
{
	if (!el || depth > UIA_WEB_MAX_DEPTH || ctx->visited >= UIA_WEB_MAX_NODES || ctx->truncated)
		return;
	ctx->visited++;

	CONTROLTYPEID ct = 0;
	char raw_label[256] = {0}, raw_val[512] = {0};
	uia_get_el_strings(el, &ct, raw_label, sizeof(raw_label), raw_val, sizeof(raw_val));
	char label[256] = {0}, val[512] = {0};
	uia_clean_text(raw_label, label, sizeof(label));
	uia_clean_text(raw_val, val, sizeof(val));
	const char *role = uia_role_name(ct);

	bool now_in_doc = in_doc;
	const char *child_filter = filter;
	bool skip_children = false;

	if (ct == UIA_DocumentControlTypeId) {
		BOOL offscreen = FALSE;
		IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);
		if (offscreen) return;
		now_in_doc = true;
		if (child_filter && *child_filter &&
		    (desktop_core_str_contains_ci(label, child_filter) ||
		     desktop_core_str_contains_ci(val, child_filter))) {
			child_filter = NULL;
		}
		char line[900];
		snprintf(line, sizeof(line), "[document web] %.200s%s%.500s\n",
		         label, val[0] ? " url=" : "", val);
		uia_bemit_line(ctx, line);
	} else if (!in_doc && !ctx->read_mode && ct == UIA_TabItemControlTypeId && label[0]) {
		RECT rc = {0, 0, 0, 0};
		char coord[32] = {0};
		if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(el, &rc)) &&
		    rc.right > rc.left && rc.bottom > rc.top) {
			int cx = (int)((rc.left + rc.right) / 2);
			int cy = (int)((rc.top + rc.bottom) / 2);
			if (uia_in_viewport(cx, cy, &ctx->win_rc, ctx->sw, ctx->sh))
				snprintf(coord, sizeof(coord), " @(%d,%d)", cx, cy);
			else
				snprintf(coord, sizeof(coord), " *OFF-SCREEN*");
		}
		BOOL active = FALSE;
		IUIAutomationSelectionItemPattern *sel = NULL;
		if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_SelectionItemPatternId,
		                                                       &jv_IID_IUIAutomationSelectionItemPattern,
		                                                       (void **)&sel)) && sel) {
			IUIAutomationSelectionItemPattern_get_CurrentIsSelected(sel, &active);
			IUIAutomationSelectionItemPattern_Release(sel);
		}
		char line[360];
		snprintf(line, sizeof(line), "[page tab%s%s] %.200s\n",
		         coord, active ? " *ACTIVE*" : "", label);
		uia_bemit_line(ctx, line);
		skip_children = true;
	} else if (now_in_doc && !ctx->read_mode &&
	           (uia_is_web_interactive(ct) || uia_is_editable_ct(ct, el))) {
		const char *disp = label[0] ? label : val;
		bool is_input = uia_is_editable_ct(ct, el);
		bool match = true;
		if (child_filter && *child_filter && !is_input) {
			match = desktop_core_str_contains_ci(disp, child_filter) ||
			        desktop_core_str_contains_ci(role, child_filter);
		}
		if (match && (disp[0] || is_input)) {
			RECT rc = {0, 0, 0, 0};
			if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(el, &rc)) &&
			    rc.right > rc.left && rc.bottom > rc.top) {
				int cx = (int)((rc.left + rc.right) / 2);
				int cy = (int)((rc.top + rc.bottom) / 2);
				BOOL offscreen = FALSE, foc = FALSE, en = TRUE;
				IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);
				IUIAutomationElement_get_CurrentHasKeyboardFocus(el, &foc);
				IUIAutomationElement_get_CurrentIsEnabled(el, &en);
				bool in_vp = !offscreen && uia_in_viewport(cx, cy, &ctx->win_rc, ctx->sw, ctx->sh);
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
				uia_bemit_line(ctx, line);
				if (ct == UIA_ButtonControlTypeId || ct == UIA_HyperlinkControlTypeId ||
				    ct == UIA_DataItemControlTypeId || is_input) {
					skip_children = true;
				}
			}
		}
	} else if (now_in_doc && ctx->read_mode && uia_is_web_text(ct)) {
		const char *txt = label[0] ? label : val;
		if (txt[0]) {
			bool match = !child_filter || !*child_filter ||
			             desktop_core_str_contains_ci(txt, child_filter);
			if (match) {
				char line[384];
				if (ct == UIA_TextControlTypeId)
					snprintf(line, sizeof(line), "%.360s\n", txt);
				else
					snprintf(line, sizeof(line), "[%s] %.300s\n", role, txt);
				uia_bemit_line(ctx, line);
				if (ct == UIA_HyperlinkControlTypeId || ct == UIA_ButtonControlTypeId)
					skip_children = true;
			}
		}
	}

	if (skip_children || ctx->truncated) return;

	IUIAutomationElement *ch = NULL;
	if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(ctx->walker, el, &ch)) && ch) {
		int idx = 0;
		while (ch && !ctx->truncated && idx < 150) {
			idx++;
			uia_walk_browser_collect(ch, depth + 1, now_in_doc, child_filter, ctx);
			IUIAutomationElement *next = NULL;
			HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(ctx->walker, ch, &next);
			IUIAutomationElement_Release(ch);
			ch = SUCCEEDED(nhr) ? next : NULL;
		}
		if (ch) IUIAutomationElement_Release(ch);
	}
}

bool uia_core_browser_collect(const a11y_ops_t *ops, const char *filter_query,
                              bool read_mode, size_t skip_chars,
                              char *out, size_t out_sz)
{
	(void)ops;
	if (!out || out_sz == 0) return false;
	out[0] = '\0';

	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	uia_bwalk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	if (!uia_open_browser_root(&uia, &walker, &root, NULL, &ctx.win_rc, &ctx.sw, &ctx.sh, &uninit))
		return false;

	ctx.filter = filter_query;
	ctx.read_mode = read_mode;
	ctx.skip_chars = skip_chars;
	ctx.out = out;
	ctx.out_sz = out_sz;
	ctx.walker = walker;

	uia_walk_browser_collect(root, 0, false, filter_query, &ctx);
	uia_close_root(uia, walker, root, uninit);

	if (ctx.truncated && ctx.off + 64 < out_sz) {
		snprintf(out + ctx.off, out_sz - ctx.off,
		         "\n[truncated; use 'browser read %zu' for more]", skip_chars + ctx.off);
	}
	return ctx.off > 0;
}

typedef struct {
	const char              *query;
	bool                     active_match;
	IUIAutomationElement    *found_tab;
	char                     matched_title[128];
	int                      visited;
	IUIAutomationTreeWalker *walker;
} uia_btab_ctx_t;

static void uia_walk_browser_tab(IUIAutomationElement *el, int depth, uia_btab_ctx_t *ctx)
{
	if (!el || depth > 14 || ctx->visited >= 800 || ctx->active_match)
		return;
	ctx->visited++;

	CONTROLTYPEID ct = 0;
	char raw_label[256] = {0};
	uia_get_el_strings(el, &ct, raw_label, sizeof(raw_label), NULL, 0);
	if (ct == UIA_DocumentControlTypeId) return;

	if (ct == UIA_TabItemControlTypeId && raw_label[0]) {
		char label[256] = {0};
		uia_clean_text(raw_label, label, sizeof(label));
		if (label[0] && desktop_core_str_contains_ci(label, ctx->query)) {
			BOOL active = FALSE;
			IUIAutomationSelectionItemPattern *sel = NULL;
			if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_SelectionItemPatternId,
			                                                       &jv_IID_IUIAutomationSelectionItemPattern,
			                                                       (void **)&sel)) && sel) {
				IUIAutomationSelectionItemPattern_get_CurrentIsSelected(sel, &active);
				IUIAutomationSelectionItemPattern_Release(sel);
			}
			if (active) {
				ctx->active_match = true;
				snprintf(ctx->matched_title, sizeof(ctx->matched_title), "%.120s", label);
			} else if (!ctx->found_tab) {
				ctx->found_tab = el;
				IUIAutomationElement_AddRef(ctx->found_tab);
				snprintf(ctx->matched_title, sizeof(ctx->matched_title), "%.120s", label);
			}
		}
		return;
	}

	IUIAutomationElement *ch = NULL;
	if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(ctx->walker, el, &ch)) && ch) {
		int idx = 0;
		while (ch && !ctx->active_match && idx < 64) {
			idx++;
			uia_walk_browser_tab(ch, depth + 1, ctx);
			IUIAutomationElement *next = NULL;
			HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(ctx->walker, ch, &next);
			IUIAutomationElement_Release(ch);
			ch = SUCCEEDED(nhr) ? next : NULL;
		}
		if (ch) IUIAutomationElement_Release(ch);
	}
}

bool uia_core_browser_switch_tab(const a11y_ops_t *ops, const char *query,
                                 char *out_tab_title, size_t title_sz)
{
	if (out_tab_title && title_sz > 0) out_tab_title[0] = '\0';
	if (!query || !*query) return false;

	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	if (!uia_open_browser_root(&uia, &walker, &root, NULL, NULL, NULL, NULL, &uninit))
		return false;

	uia_btab_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.query = query;
	ctx.walker = walker;
	uia_walk_browser_tab(root, 0, &ctx);

	bool switched = false;
	if (ctx.active_match) {
		switched = true;
	} else if (ctx.found_tab) {
		IUIAutomationSelectionItemPattern *sel = NULL;
		if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.found_tab, UIA_SelectionItemPatternId,
		                                                       &jv_IID_IUIAutomationSelectionItemPattern,
		                                                       (void **)&sel)) && sel) {
			switched = SUCCEEDED(IUIAutomationSelectionItemPattern_Select(sel));
			IUIAutomationSelectionItemPattern_Release(sel);
		}
		if (!switched) {
			IUIAutomationInvokePattern *inv = NULL;
			if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.found_tab, UIA_InvokePatternId,
			                                                       &jv_IID_IUIAutomationInvokePattern,
			                                                       (void **)&inv)) && inv) {
				switched = SUCCEEDED(IUIAutomationInvokePattern_Invoke(inv));
				IUIAutomationInvokePattern_Release(inv);
			}
		}
		if (!switched && ops && ops->click_xy) {
			RECT rc = {0, 0, 0, 0};
			if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(ctx.found_tab, &rc)) &&
			    rc.right > rc.left && rc.bottom > rc.top) {
				switched = ops->click_xy(ops->ud, (int)((rc.left + rc.right) / 2),
				                         (int)((rc.top + rc.bottom) / 2), 1);
			}
		}
		if (switched) Sleep(160);
	}

	if (ctx.found_tab) IUIAutomationElement_Release(ctx.found_tab);
	uia_close_root(uia, walker, root, uninit);
	if (switched && out_tab_title && title_sz > 0)
		snprintf(out_tab_title, title_sz, "%s", ctx.matched_title);
	return switched;
}

typedef struct {
	const char              *query;
	IUIAutomationElement    *best;
	int                      best_score;
	int                      best_cx;
	int                      best_cy;
	bool                     best_in_vp;
	bool                     best_editable;
	char                     best_role[64];
	char                     best_label[128];
	int                      visited;
	RECT                     win_rc;
	int                      sw, sh;
	IUIAutomationTreeWalker *walker;
} uia_bfind_ctx_t;

static void uia_walk_browser_find(IUIAutomationElement *el, int depth, bool in_doc, uia_bfind_ctx_t *ctx)
{
	if (!el || depth > UIA_WEB_MAX_DEPTH || ctx->visited >= UIA_WEB_MAX_NODES)
		return;
	ctx->visited++;

	CONTROLTYPEID ct = 0;
	char raw_label[256] = {0}, raw_val[128] = {0};
	uia_get_el_strings(el, &ct, raw_label, sizeof(raw_label), raw_val, sizeof(raw_val));
	bool now_in_doc = in_doc;
	if (ct == UIA_DocumentControlTypeId) {
		BOOL offscreen = FALSE;
		IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);
		if (offscreen) return;
		now_in_doc = true;
	}

	if (ct != UIA_WindowControlTypeId && ct != UIA_DocumentControlTypeId) {
		char label[256] = {0}, val[128] = {0};
		uia_clean_text(raw_label, label, sizeof(label));
		uia_clean_text(raw_val, val, sizeof(val));
		const char *role = uia_role_name(ct);
		const char *disp = label[0] ? label : val;
		int sc = uia_score_label(disp, ctx->query);
		if (sc == 0 && uia_is_web_interactive(ct) &&
		    desktop_core_str_contains_ci(role, ctx->query)) {
			sc = 35;
		}
		if (sc > 0) {
			if (now_in_doc) sc += 20;
			if (uia_is_web_interactive(ct)) sc += 30;
			RECT rc = {0, 0, 0, 0};
			if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(el, &rc)) &&
			    rc.right > rc.left && rc.bottom > rc.top) {
				int cx = (int)((rc.left + rc.right) / 2);
				int cy = (int)((rc.top + rc.bottom) / 2);
				BOOL offscreen = FALSE;
				IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);
				bool in_vp = !offscreen && uia_in_viewport(cx, cy, &ctx->win_rc, ctx->sw, ctx->sh);
				if (in_vp) sc += 25;
				if (sc > ctx->best_score) {
					if (ctx->best) IUIAutomationElement_Release(ctx->best);
					ctx->best = el;
					IUIAutomationElement_AddRef(ctx->best);
					ctx->best_score = sc;
					ctx->best_cx = cx;
					ctx->best_cy = cy;
					ctx->best_in_vp = in_vp;
					ctx->best_editable = uia_is_editable_ct(ct, el);
					snprintf(ctx->best_role, sizeof(ctx->best_role), "%.63s", role);
					snprintf(ctx->best_label, sizeof(ctx->best_label), "%.120s", disp[0] ? disp : role);
				}
			}
		}
	}

	IUIAutomationElement *ch = NULL;
	if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(ctx->walker, el, &ch)) && ch) {
		int idx = 0;
		while (ch && ctx->visited < UIA_WEB_MAX_NODES && idx < 100) {
			idx++;
			uia_walk_browser_find(ch, depth + 1, now_in_doc, ctx);
			IUIAutomationElement *next = NULL;
			HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(ctx->walker, ch, &next);
			IUIAutomationElement_Release(ch);
			ch = SUCCEEDED(nhr) ? next : NULL;
		}
		if (ch) IUIAutomationElement_Release(ch);
	}
}

bool uia_core_browser_click(const a11y_ops_t *ops, const char *query, int btn,
                            char *out_role, size_t role_sz,
                            char *out_msg, size_t out_sz)
{
	if (out_role && role_sz > 0) out_role[0] = '\0';
	if (out_msg && out_sz > 0) out_msg[0] = '\0';
	if (!query || !*query) return false;

	if (ops && ops->focus_win) ops->focus_win(ops->ud);

	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	bool uninit = false;
	uia_bfind_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	if (!uia_open_browser_root(&uia, &walker, &root, NULL, &ctx.win_rc, &ctx.sw, &ctx.sh, &uninit))
		return false;

	ctx.query = query;
	ctx.walker = walker;
	uia_walk_browser_find(root, 0, false, &ctx);

	if (!ctx.best) {
		uia_close_root(uia, walker, root, uninit);
		return false;
	}
	if (out_role && role_sz > 0) snprintf(out_role, role_sz, "%s", ctx.best_role);

	if (!ctx.best_in_vp) {
		IUIAutomationScrollItemPattern *scr = NULL;
		if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.best, UIA_ScrollItemPatternId,
		                                                       &jv_IID_IUIAutomationScrollItemPattern,
		                                                       (void **)&scr)) && scr) {
			IUIAutomationScrollItemPattern_ScrollIntoView(scr);
			IUIAutomationScrollItemPattern_Release(scr);
			Sleep(180);
			RECT rc = {0, 0, 0, 0};
			BOOL offscreen = FALSE;
			IUIAutomationElement_get_CurrentIsOffscreen(ctx.best, &offscreen);
			if (!offscreen &&
			    SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(ctx.best, &rc)) &&
			    rc.right > rc.left && rc.bottom > rc.top) {
				int cx = (int)((rc.left + rc.right) / 2);
				int cy = (int)((rc.top + rc.bottom) / 2);
				if (uia_in_viewport(cx, cy, &ctx.win_rc, ctx.sw, ctx.sh)) {
					ctx.best_cx = cx;
					ctx.best_cy = cy;
					ctx.best_in_vp = true;
				}
			}
		}
		if (!ctx.best_in_vp && btn != 3) {
			IUIAutomationInvokePattern *inv = NULL;
			if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.best, UIA_InvokePatternId,
			                                                       &jv_IID_IUIAutomationInvokePattern,
			                                                       (void **)&inv)) && inv) {
				bool did = SUCCEEDED(IUIAutomationInvokePattern_Invoke(inv));
				IUIAutomationInvokePattern_Release(inv);
				if (did) {
					Sleep(90);
					if (out_msg && out_sz > 0) {
						snprintf(out_msg, out_sz, "activated [%s] '%.64s' via native action",
						         ctx.best_role, ctx.best_label);
					}
					IUIAutomationElement_Release(ctx.best);
					uia_close_root(uia, walker, root, uninit);
					return true;
				}
			}
		}
		if (!ctx.best_in_vp) {
			if (out_msg && out_sz > 0) {
				snprintf(out_msg, out_sz,
				         "Error: element [%s] '%.64s' is off-screen (outside viewport); scroll or filter first",
				         ctx.best_role, ctx.best_label);
			}
			IUIAutomationElement_Release(ctx.best);
			uia_close_root(uia, walker, root, uninit);
			return false;
		}
	}

	if (ctx.best_editable)
		IUIAutomationElement_SetFocus(ctx.best);

	bool ok = false;
	if (ops && ops->click_xy)
		ok = ops->click_xy(ops->ud, ctx.best_cx, ctx.best_cy, btn);
	if (!ok && btn != 3) {
		IUIAutomationInvokePattern *inv = NULL;
		if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(ctx.best, UIA_InvokePatternId,
		                                                       &jv_IID_IUIAutomationInvokePattern,
		                                                       (void **)&inv)) && inv) {
			ok = SUCCEEDED(IUIAutomationInvokePattern_Invoke(inv));
			IUIAutomationInvokePattern_Release(inv);
		}
	}
	Sleep(90);
	if (out_msg && out_sz > 0) {
		snprintf(out_msg, out_sz, "%s [%s] '%.64s' at (%d,%d)",
		         ok ? "clicked" : "Error: failed to click",
		         ctx.best_role, ctx.best_label, ctx.best_cx, ctx.best_cy);
	}
	IUIAutomationElement_Release(ctx.best);
	uia_close_root(uia, walker, root, uninit);
	return ok;
}

typedef struct {
	char role[64];
	char label[128];
	int  cx;
	int  cy;
	bool multi_line;
} uia_bedit_cand_t;

typedef struct {
	a11y_browser_input_t     focused;
	uia_bedit_cand_t         cands[16];
	int                      n_cands;
	int                      visited;
	RECT                     win_rc;
	int                      sw, sh;
	IUIAutomationTreeWalker *walker;
} uia_binput_ctx_t;

static void uia_walk_browser_inputs(IUIAutomationElement *el, int depth, bool in_doc, uia_binput_ctx_t *ctx)
{
	if (!el || depth > UIA_WEB_MAX_DEPTH || ctx->visited >= 2800 || ctx->focused.found)
		return;
	ctx->visited++;

	CONTROLTYPEID ct = 0;
	IUIAutomationElement_get_CurrentControlType(el, &ct);
	bool now_in_doc = in_doc;
	if (ct == UIA_DocumentControlTypeId) {
		BOOL offscreen = FALSE;
		IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);
		if (offscreen) return;
		now_in_doc = true;
	}

	bool skip_children = false;
	if (now_in_doc && ct != UIA_DocumentControlTypeId && uia_is_editable_ct(ct, el)) {
		BOOL en = TRUE;
		IUIAutomationElement_get_CurrentIsEnabled(el, &en);
		if (en) {
			BOOL foc = FALSE, offscreen = FALSE;
			IUIAutomationElement_get_CurrentHasKeyboardFocus(el, &foc);
			IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);
			char raw_label[128] = {0}, label[128] = {0};
			uia_get_el_strings(el, NULL, raw_label, sizeof(raw_label), NULL, 0);
			uia_clean_text(raw_label, label, sizeof(label));
			const char *role = uia_role_name(ct);
			const char *disp = label[0] ? label : "(unlabeled)";
			RECT rc = {0, 0, 0, 0};
			bool have_c = SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(el, &rc)) &&
			              rc.right > rc.left && rc.bottom > rc.top;
			int cx = have_c ? (int)((rc.left + rc.right) / 2) : 0;
			int cy = have_c ? (int)((rc.top + rc.bottom) / 2) : 0;
			bool in_vp = have_c && !offscreen && uia_in_viewport(cx, cy, &ctx->win_rc, ctx->sw, ctx->sh);
			if (foc) {
				ctx->focused.found = true;
				ctx->focused.editable = true;
				snprintf(ctx->focused.role, sizeof(ctx->focused.role), "%.63s", role);
				snprintf(ctx->focused.label, sizeof(ctx->focused.label), "%.127s", disp);
				ctx->focused.cx = cx;
				ctx->focused.cy = cy;
			} else if (in_vp && ctx->n_cands < 16) {
				uia_bedit_cand_t *c = &ctx->cands[ctx->n_cands++];
				snprintf(c->role, sizeof(c->role), "%.63s", role);
				snprintf(c->label, sizeof(c->label), "%.127s", disp);
				c->cx = cx;
				c->cy = cy;
				c->multi_line = (ct == UIA_EditControlTypeId && (rc.bottom - rc.top) >= 48);
			}
			skip_children = true;
		}
	}

	if (skip_children || ctx->focused.found) return;

	IUIAutomationElement *ch = NULL;
	if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(ctx->walker, el, &ch)) && ch) {
		int idx = 0;
		while (ch && !ctx->focused.found && idx < 100) {
			idx++;
			uia_walk_browser_inputs(ch, depth + 1, now_in_doc, ctx);
			IUIAutomationElement *next = NULL;
			HRESULT nhr = IUIAutomationTreeWalker_GetNextSiblingElement(ctx->walker, ch, &next);
			IUIAutomationElement_Release(ch);
			ch = SUCCEEDED(nhr) ? next : NULL;
		}
		if (ch) IUIAutomationElement_Release(ch);
	}
}

bool uia_core_browser_ensure_editable(const a11y_ops_t *ops, bool focus_if_needed,
                                      a11y_browser_input_t *out_info,
                                      char *out_err, size_t err_sz)
{
	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (out_err && err_sz > 0) out_err[0] = '\0';

	if (focus_if_needed && ops && ops->is_win_focused && !ops->is_win_focused(ops->ud)) {
		if (ops->focus_win) ops->focus_win(ops->ud);
		Sleep(40);
		if (!ops->is_win_focused(ops->ud)) {
			if (out_err && err_sz > 0)
				snprintf(out_err, err_sz, "Error: browser window is not focused");
			return false;
		}
	}

	IUIAutomation *uia = NULL;
	IUIAutomationTreeWalker *walker = NULL;
	IUIAutomationElement *root = NULL;
	DWORD bpid = 0;
	bool uninit = false;
	uia_binput_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	if (!uia_open_browser_root(&uia, &walker, &root, &bpid, &ctx.win_rc, &ctx.sw, &ctx.sh, &uninit)) {
		if (out_err && err_sz > 0)
			snprintf(out_err, err_sz, "Error: browser accessibility unavailable");
		return false;
	}

	/* Fast check against UIA focused element when it belongs to the browser process */
	IUIAutomationElement *foc = NULL;
	if (SUCCEEDED(IUIAutomation_GetFocusedElement(uia, &foc)) && foc) {
		int fpid = 0;
		IUIAutomationElement_get_CurrentProcessId(foc, &fpid);
		if ((DWORD)fpid == bpid) {
			CONTROLTYPEID ct = 0;
			char raw_label[128] = {0}, label[128] = {0};
			uia_get_el_strings(foc, &ct, raw_label, sizeof(raw_label), NULL, 0);
			if (uia_is_editable_ct(ct, foc)) {
				uia_clean_text(raw_label, label, sizeof(label));
				RECT rc = {0, 0, 0, 0};
				bool have_c = SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(foc, &rc)) &&
				              rc.right > rc.left && rc.bottom > rc.top;
				ctx.focused.found = true;
				ctx.focused.editable = true;
				snprintf(ctx.focused.role, sizeof(ctx.focused.role), "%.63s", uia_role_name(ct));
				snprintf(ctx.focused.label, sizeof(ctx.focused.label), "%.127s",
				         label[0] ? label : "(unlabeled)");
				ctx.focused.cx = have_c ? (int)((rc.left + rc.right) / 2) : 0;
				ctx.focused.cy = have_c ? (int)((rc.top + rc.bottom) / 2) : 0;
			}
		}
		IUIAutomationElement_Release(foc);
	}

	if (!ctx.focused.found) {
		ctx.walker = walker;
		uia_walk_browser_inputs(root, 0, false, &ctx);
	}
	uia_close_root(uia, walker, root, uninit);

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
		const uia_bedit_cand_t *c = &ctx.cands[chosen];
		if (ops && ops->click_xy) {
			ops->click_xy(ops->ud, c->cx, c->cy, 1);
			Sleep(80);
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

