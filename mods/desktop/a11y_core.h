#ifndef JARVIS_DESKTOP_A11Y_CORE_H
#define JARVIS_DESKTOP_A11Y_CORE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	unsigned long pid;
	const char   *cls;
	const char   *title;
	int           wx, wy, ww, wh;
	int           sw, sh;
	bool          has_win;
	bool          has_scr;
} a11y_win_ctx_t;

typedef struct {
	bool (*focus_win)(void *ud);
	bool (*is_win_focused)(void *ud);
	bool (*click_xy)(void *ud, int x, int y, int btn);
	bool (*send_key)(void *ud, const char *combo);
	bool (*type_text)(void *ud, const char *text, bool submit);
	void *ud;
} a11y_ops_t;

#if defined(__APPLE__)
#include "ax_core.h"

static inline bool a11y_core_read_window(const a11y_win_ctx_t *win, bool inspect_mode,
                                         const char *filter, char *out, size_t out_sz)
{
	return ax_core_read_window(win, inspect_mode, filter, out, out_sz);
}

static inline bool a11y_core_click(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                   const char *query, int btn, char *out_msg, size_t out_sz)
{
	return ax_core_click(win, ops, query, btn, out_msg, out_sz);
}

static inline bool a11y_core_focus(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                   const char *query, char *out_msg, size_t out_sz)
{
	return ax_core_focus(win, ops, query, out_msg, out_sz);
}

static inline bool a11y_core_prepare_input(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                           const char *target, bool is_terminal,
                                           char *out_role, size_t role_sz,
                                           char *out_label, size_t label_sz,
                                           char *out_err, size_t err_sz)
{
	return ax_core_prepare_input(win, ops, target, is_terminal,
	                             out_role, role_sz, out_label, label_sz, out_err, err_sz);
}

static inline bool a11y_core_set(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                 const char *query, const char *value, char *out_msg, size_t out_sz)
{
	return ax_core_set(win, ops, query, value, out_msg, out_sz);
}

#elif defined(_WIN32)
#include "uia_core.h"

static inline bool a11y_core_read_window(const a11y_win_ctx_t *win, bool inspect_mode,
                                         const char *filter, char *out, size_t out_sz)
{
	return uia_core_read_window(win, inspect_mode, filter, out, out_sz);
}

static inline bool a11y_core_click(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                   const char *query, int btn, char *out_msg, size_t out_sz)
{
	return uia_core_click(win, ops, query, btn, out_msg, out_sz);
}

static inline bool a11y_core_focus(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                   const char *query, char *out_msg, size_t out_sz)
{
	return uia_core_focus(win, ops, query, out_msg, out_sz);
}

static inline bool a11y_core_prepare_input(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                           const char *target, bool is_terminal,
                                           char *out_role, size_t role_sz,
                                           char *out_label, size_t label_sz,
                                           char *out_err, size_t err_sz)
{
	return uia_core_prepare_input(win, ops, target, is_terminal,
	                              out_role, role_sz, out_label, label_sz, out_err, err_sz);
}

static inline bool a11y_core_set(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                 const char *query, const char *value, char *out_msg, size_t out_sz)
{
	return uia_core_set(win, ops, query, value, out_msg, out_sz);
}

#else
#include "atspi_core.h"

static inline void a11y_to_atspi_win(const a11y_win_ctx_t *w, atspi_win_ctx_t *aw)
{
	if (!w) {
		aw->pid = 0; aw->cls = NULL; aw->title = NULL;
		aw->wx = aw->wy = aw->ww = aw->wh = 0;
		aw->sw = aw->sh = 0;
		aw->has_win = false; aw->has_scr = false;
		return;
	}
	aw->pid     = w->pid;
	aw->cls     = w->cls;
	aw->title   = w->title;
	aw->wx      = w->wx;
	aw->wy      = w->wy;
	aw->ww      = w->ww;
	aw->wh      = w->wh;
	aw->sw      = w->sw;
	aw->sh      = w->sh;
	aw->has_win = w->has_win;
	aw->has_scr = w->has_scr;
}

static inline void a11y_to_atspi_ops(const a11y_ops_t *o, atspi_ops_t *ao)
{
	ao->focus_win      = o ? o->focus_win : NULL;
	ao->is_win_focused = o ? o->is_win_focused : NULL;
	ao->click_xy       = o ? o->click_xy : NULL;
	ao->send_key       = o ? o->send_key : NULL;
	ao->type_text      = o ? o->type_text : NULL;
	ao->ud             = o ? o->ud : NULL;
}

static inline bool a11y_core_read_window(const a11y_win_ctx_t *win, bool inspect_mode,
                                         const char *filter, char *out, size_t out_sz)
{
	atspi_win_ctx_t aw;
	a11y_to_atspi_win(win, &aw);
	return atspi_core_read_window(&aw, inspect_mode, filter, out, out_sz);
}

static inline bool a11y_core_click(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                   const char *query, int btn, char *out_msg, size_t out_sz)
{
	atspi_win_ctx_t aw;
	atspi_ops_t ao;
	a11y_to_atspi_win(win, &aw);
	a11y_to_atspi_ops(ops, &ao);
	return atspi_core_click(&aw, &ao, query, btn, out_msg, out_sz);
}

static inline bool a11y_core_focus(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                   const char *query, char *out_msg, size_t out_sz)
{
	atspi_win_ctx_t aw;
	atspi_ops_t ao;
	a11y_to_atspi_win(win, &aw);
	a11y_to_atspi_ops(ops, &ao);
	return atspi_core_focus(&aw, &ao, query, out_msg, out_sz);
}

static inline bool a11y_core_prepare_input(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                           const char *target, bool is_terminal,
                                           char *out_role, size_t role_sz,
                                           char *out_label, size_t label_sz,
                                           char *out_err, size_t err_sz)
{
	atspi_win_ctx_t aw;
	atspi_ops_t ao;
	a11y_to_atspi_win(win, &aw);
	a11y_to_atspi_ops(ops, &ao);
	return atspi_core_prepare_input(&aw, &ao, target, is_terminal,
	                                out_role, role_sz, out_label, label_sz, out_err, err_sz);
}

static inline bool a11y_core_set(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                                 const char *query, const char *value, char *out_msg, size_t out_sz)
{
	atspi_win_ctx_t aw;
	atspi_ops_t ao;
	a11y_to_atspi_win(win, &aw);
	a11y_to_atspi_ops(ops, &ao);
	return atspi_core_set(&aw, &ao, query, value, out_msg, out_sz);
}
#endif

#endif /* JARVIS_DESKTOP_A11Y_CORE_H */
