#ifndef JARVIS_PLATFORM_H
#define JARVIS_PLATFORM_H

#include <ttypt/stele.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jarvis_plat_lock jarvis_plat_lock_t;

/* Singleton daemon/instance lock */
jarvis_plat_lock_t *jarvis_plat_singleton_acquire(const char *name);
void                jarvis_plat_singleton_release(jarvis_plat_lock_t *lk);

/* Portable user/temp/data path resolution */
int jarvis_plat_state_path(const char *prefix, const char *suffix, char *out, size_t cap);
int jarvis_plat_data_file(const char *subdir, const char *filename, char *out, size_t cap);

/* File & PATH helpers (safe across POSIX + Win32 CRT) */
int jarvis_plat_is_readable(const char *path);
int jarvis_plat_is_exec(const char *path);
int jarvis_plat_find_in_path(const char *cmd, char *out, size_t cap);

/* Desktop OS integration */
int         jarvis_plat_notify_osd(const char *title, const char *body, int timeout_ms);
void        jarvis_plat_notify_bar(void);
int         jarvis_plat_lock_screen(const char *override_cmd);
int         jarvis_plat_open_url(const char *url);
void        jarvis_plat_kbd_lang(char *out, size_t cap);
int16_t    *jarvis_plat_tts_fallback_pcm(const char *text, const char *lang_hint,
                                         size_t *out_samples, int *out_rate);

/* Dynamic library names & stderr suppression for lazy TTS runtime */
const char *jarvis_plat_espeak_lib_name(void);
const char *jarvis_plat_ort_lib_name(int alt_index);
void        jarvis_plat_suppress_stderr_begin(int *saved_fd);
void        jarvis_plat_suppress_stderr_end(int saved_fd);

/* Portable socket helpers */
int  jarvis_plat_sock_peer_closed(stl_socket_t fd);
void jarvis_plat_write_sock_all(stl_socket_t fd, const char *buf, size_t len);
int  jarvis_plat_read_wav_pcm16(const uint8_t *wav, size_t len,
                                int16_t **out_pcm, size_t *out_samples, int *out_rate);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_PLATFORM_H */
