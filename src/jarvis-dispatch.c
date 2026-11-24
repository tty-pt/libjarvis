#define _GNU_SOURCE
#include "jarvis-dispatch.h"
#include "jarvis-ren-ipc.h"
#include "jarvis-tts.h"
#include "jarvis-platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void
trim_inplace(char *s)
{
	if (!s) return;
	char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

int
jarvis_dispatch_send_ren_cmd(const char *cmd, char *out_reply, size_t out_sz, int (*is_active_fn)(void))
{
	if (out_reply && out_sz > 0)
		out_reply[0] = '\0';
	if (!cmd || !*cmd)
		return -1;

	stl_socket_t fd = STL_INVALID_SOCKET;
	for (int attempt = 0; attempt < 15; attempt++) {
		fd = jarvis_ren_connect(0);
		if (fd != STL_INVALID_SOCKET)
			break;
		if (is_active_fn && !is_active_fn())
			return -1;
		stl_sleep_ms(40);
	}
	if (fd == STL_INVALID_SOCKET)
		return -1;

	stl_sock_set_timeout(fd, 2000, 2000);

	char msg[4096];
	int len = snprintf(msg, sizeof(msg), "%s\n", cmd);
	if (len <= 0) {
		stl_sock_close(fd);
		return -1;
	}
	jarvis_plat_write_sock_all(fd, msg, (size_t)len);

	if (out_reply && out_sz > 0) {
		char buf[256];
		ssize_t n = stl_sock_recv(fd, buf, sizeof(buf) - 1, 0);
		if (n > 0) {
			buf[n] = '\0';
			trim_inplace(buf);
			size_t blen = strlen(buf);
			if (blen >= out_sz)
				blen = out_sz - 1;
			memcpy(out_reply, buf, blen);
			out_reply[blen] = '\0';
		}
	}
	stl_sock_close(fd);
	return 0;
}

bool
jarvis_dispatch_to_ren(const char *utterance, const jarvis_ontology_t *ont, const char *wake_buf,
                       int *out_started_media, int *out_conv_exited,
                       char *out_err, size_t err_sz, int (*is_stopped_fn)(void))
{
	if (out_err && err_sz > 0)
		out_err[0] = '\0';

	stl_socket_t fd = jarvis_ren_connect(0);
	if (fd == STL_INVALID_SOCKET) {
		char rc_mods[512] = {0};
		jarvis_rc_get("ren_mods", NULL, rc_mods, sizeof(rc_mods));
		fd = jarvis_ren_ensure_session(NULL, rc_mods[0] ? rc_mods : NULL, NULL);
		if (fd == STL_INVALID_SOCKET) {
			if (out_err && err_sz > 0)
				snprintf(out_err, err_sz, "ren unavailable");
			return false;
		}
	}

	if (is_stopped_fn && is_stopped_fn()) {
		stl_sock_close(fd);
		if (out_err && err_sz > 0)
			snprintf(out_err, err_sz, "interrupted");
		return false;
	}

	char req[4096];
	int req_len = snprintf(req, sizeof(req), "%s\n", utterance);
	if (req_len <= 0) {
		stl_sock_close(fd);
		return false;
	}
	jarvis_plat_write_sock_all(fd, req, (size_t)req_len);

	bool any_event = false;
	bool ok = true;
	char line[4096];
	jarvis_ren_reader_t rr;
	jarvis_ren_reader_init(&rr);
	while (jarvis_ren_read_line(fd, &rr, line, sizeof(line)) > 0) {
		if (line[0] == '\0')
			continue;

		char *tab1 = strchr(line, '\t');
		if (!tab1)
			continue;
		*tab1 = '\0';
		const char *ev = line;
		char *arg1 = tab1 + 1;
		char *tab2 = strchr(arg1, '\t');
		const char *arg2 = "";
		if (tab2) {
			*tab2 = '\0';
			arg2 = tab2 + 1;
		}

		any_event = true;
		if (strcmp(ev, "TIER0") == 0) {
			if (strcmp(arg1, "media_play") == 0 && out_started_media)
				*out_started_media = 1;
			if (strcmp(arg1, "conversation_control") == 0 && out_conv_exited)
				*out_conv_exited = 1;
			char title[256], body[1024], ntitle[128];
			jarvis_ontology_format_ui(ont, "notify_title", wake_buf,
			                          "  {Wake}", ntitle, sizeof(ntitle));
			snprintf(title, sizeof(title), "%s → %s (tier-0)", ntitle, arg1[0] ? arg1 : "action");
			snprintf(body, sizeof(body), "“%.350s”\n▸ %.550s", utterance, arg2);
			jarvis_plat_notify_osd(title, body, 1);
		} else if (strcmp(ev, "TOOL") == 0) {
			if (strcmp(arg1, "media") == 0 && strncasecmp(arg2, "play", 4) == 0 && out_started_media)
				*out_started_media = 1;
			char title[256], body[1024];
			snprintf(title, sizeof(title), "  ren → %s", arg1);
			snprintf(body, sizeof(body), "▸ %.550s", arg2);
			jarvis_plat_notify_osd(title, body, 0);
		} else if (strcmp(ev, "CLAUSE") == 0) {
			if (arg1[0]) {
				int is_final = (strcmp(arg2, "1") == 0) ? 1 : 0;
				jarvis_tts_speak_clause("ren", arg1, is_final);
			}
		} else if (strcmp(ev, "STEER") == 0) {
			/* Steering OSD is emitted immediately by caller */
		} else if (strcmp(ev, "DONE") == 0) {
			ok = (strcmp(arg1, "1") == 0);
			if (!ok && out_err && err_sz > 0)
				snprintf(out_err, err_sz, "%s", arg2[0] ? arg2 : "request failed");
			break;
		}
	}

	stl_sock_close(fd);
	if ((!any_event || !ok) && out_err && err_sz > 0 && !out_err[0])
		snprintf(out_err, err_sz, any_event ? "request failed" : "ren disconnected");
	return any_event && ok;
}
