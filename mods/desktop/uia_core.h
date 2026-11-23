#ifndef JARVIS_DESKTOP_UIA_CORE_H
#define JARVIS_DESKTOP_UIA_CORE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool uia_core_read_window(const a11y_win_ctx_t *win, bool inspect_mode,
                          const char *filter, char *out, size_t out_sz);

bool uia_core_click(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                    const char *query, int btn, char *out_msg, size_t out_sz);

bool uia_core_focus(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                    const char *query, char *out_msg, size_t out_sz);

bool uia_core_prepare_input(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                            const char *target, bool is_terminal,
                            char *out_role, size_t role_sz,
                            char *out_label, size_t label_sz,
                            char *out_err, size_t err_sz);

bool uia_core_set(const a11y_win_ctx_t *win, const a11y_ops_t *ops,
                  const char *query, const char *value, char *out_msg, size_t out_sz);

bool uia_core_browser_probe_doc(a11y_browser_doc_t *out_doc);
bool uia_core_browser_collect(const a11y_ops_t *ops, const char *filter_query,
                              bool read_mode, size_t skip_chars,
                              char *out, size_t out_sz);
bool uia_core_browser_switch_tab(const a11y_ops_t *ops, const char *query,
                                 char *out_tab_title, size_t title_sz);
bool uia_core_browser_click(const a11y_ops_t *ops, const char *query, int btn,
                            char *out_role, size_t role_sz,
                            char *out_msg, size_t out_sz);
bool uia_core_browser_ensure_editable(const a11y_ops_t *ops, bool focus_if_needed,
                                      a11y_browser_input_t *out_info,
                                      char *out_err, size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_DESKTOP_UIA_CORE_H */
