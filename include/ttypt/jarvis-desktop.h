#ifndef TTYPT_JARVIS_DESKTOP_H
#define TTYPT_JARVIS_DESKTOP_H

/**
 * @file jarvis-desktop.h
 * @brief Platform-neutral Linux/POSIX desktop & media helpers for libjarvis.
 */

#include <ttypt/jarvis-types.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void jarvis_desktop_run_cmd_capture(const char *cmd, char *out, size_t out_sz);
pid_t jarvis_desktop_spawn_detached_sh(const char *cmd);
pid_t jarvis_desktop_spawn_detached_argv(char *const argv[]);
void jarvis_desktop_url_encode(const char *src, char *dst, size_t dst_sz);
void jarvis_desktop_json_escape(const char *src, char *dst, size_t dst_sz);

size_t jarvis_desktop_get_child_pids(unsigned long pid, unsigned long *out_pids, size_t max_pids);
void jarvis_desktop_read_proc_comm(unsigned long pid, char *out, size_t out_sz);
void jarvis_desktop_read_proc_cmdline(unsigned long pid, char *out, size_t out_sz);
bool jarvis_desktop_is_shell_name(const char *comm);
void jarvis_desktop_resolve_pid_cwd_fg(unsigned long pid, char *out_cwd, size_t cwd_sz, char *out_fg, size_t fg_sz);
bool jarvis_desktop_is_idle_shell_pid(unsigned long pid);
bool jarvis_desktop_is_ssh_pid(unsigned long pid);

void jarvis_desktop_populate_system_health(char *out, size_t out_sz);
void jarvis_desktop_populate_tmux_panes(jarvis_world_t *w, bool active_only, int lines);
void jarvis_desktop_populate_apps(jarvis_world_t *w);
const char *jarvis_desktop_resolve_app_exec(const char *identifier);
bool jarvis_desktop_mpris_control(const char *action);

void jarvis_desktop_populate_defaults(jarvis_world_t *w);
int jarvis_desktop_execute_fallback(const jarvis_dsl_t *dsl, jarvis_adapter_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_JARVIS_DESKTOP_H */
