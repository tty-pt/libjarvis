#ifndef JARVIS_REN_IPC_H
#define JARVIS_REN_IPC_H

#include <ttypt/stele.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	char   buf[8192];
	size_t len;
	size_t pos;
} jarvis_ren_reader_t;

const char   *jarvis_ren_endpoint(void);
void          jarvis_ren_cleanup_stale(void);
int           jarvis_ren_resolve_mods(const char *wm_hint, const char *rc_mods,
                                      char *out_csv, size_t cap);
stl_socket_t  jarvis_ren_connect(int timeout_ms);
stl_socket_t  jarvis_ren_ensure_session(const char *workdir,
                                        const char *extra_mods_csv,
                                        const char *wm_hint);
void          jarvis_ren_reader_init(jarvis_ren_reader_t *r);
int           jarvis_ren_read_line(stl_socket_t fd, jarvis_ren_reader_t *r,
                                   char *out_line, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_REN_IPC_H */
