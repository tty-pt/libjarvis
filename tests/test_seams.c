#define _GNU_SOURCE
#include "../src/jarvis-audio.h"
#include "../src/jarvis-platform.h"
#include "../src/jarvis-ren-ipc.h"

#include <ttypt/stele.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_exec_script(const char *path, const char *body)
{
	FILE *fp = fopen(path, "w");
	assert(fp != NULL);
	fputs(body, fp);
	fclose(fp);
	assert(chmod(path, 0755) == 0);
}

static void assert_no_children(void)
{
	int st = 0;
	int reaped = 0;
	bool gone = false;
	for (int i = 0; i < 40; i++) {
		pid_t w = waitpid(-1, &st, WNOHANG);
		if (w < 0 && errno == ECHILD) {
			gone = true;
			break;
		}
		if (w > 0) reaped++;
		stl_sleep_ms(10);
	}
	assert(gone);
	assert(reaped == 0);
}

static void *ipc_server_thread(void *arg)
{
	stl_ipc_listener_t *l = (stl_ipc_listener_t *)arg;
	stl_socket_t cfd = stl_ipc_accept(l);
	assert(cfd != STL_INVALID_SOCKET);

	jarvis_ren_reader_t r;
	jarvis_ren_reader_init(&r);
	char line[256];
	int rc = jarvis_ren_read_line(cfd, &r, line, sizeof(line));
	assert(rc == 1);
	assert(strcmp(line, "PING_SEAM") == 0);

	const char *reply = "CLAUSE\tHello seam\t0\nDONE\t1\tok\n";
	jarvis_plat_write_sock_all(cfd, reply, strlen(reply));
	stl_sock_close(cfd);
	return NULL;
}

int main(void)
{
	stl_ignore_pipe();
	stl_sock_init();
	printf("Running test_seams (audio, platform, ren-ipc abstractions)...\n");

	char tmpdir[] = "/tmp/test_jarvis_seams_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	const char *old_path = getenv("PATH");
	const char *old_home = getenv("HOME");
	char *saved_path = old_path ? strdup(old_path) : NULL;
	char *saved_home = old_home ? strdup(old_home) : NULL;

	/* 1. Platform paths, exec/readable checks, WAV parser */
	char fake_home[256], local_dir[280], share_dir[300], piper_dir[320], onnx_file[360];
	snprintf(fake_home, sizeof(fake_home), "%s/home", tmpdir);
	snprintf(local_dir, sizeof(local_dir), "%s/.local", fake_home);
	snprintf(share_dir, sizeof(share_dir), "%s/share", local_dir);
	snprintf(piper_dir, sizeof(piper_dir), "%s/piper", share_dir);
	snprintf(onnx_file, sizeof(onnx_file), "%s/sample.onnx", piper_dir);
	assert(stl_mkdir_p(piper_dir) == 0);
	{
		FILE *fp = fopen(onnx_file, "wb");
		assert(fp != NULL);
		fputs("dummy", fp);
		fclose(fp);
	}
	assert(stl_setenv("HOME", fake_home, 1) == 0);

	char data_out[512];
	assert(jarvis_plat_data_file("piper", "sample.onnx", data_out, sizeof(data_out)) == 0);
	assert(strcmp(data_out, onnx_file) == 0);
	assert(jarvis_plat_is_readable(onnx_file) == 1);
	assert(jarvis_plat_is_exec(onnx_file) == 0);
	assert(jarvis_plat_is_exec(fake_home) == 0);

	char state_p[256];
	assert(jarvis_plat_state_path("jarvis-seam-test", ".state", state_p, sizeof(state_p)) == 0);
	assert(strstr(state_p, "jarvis-seam-test-") != NULL);

	/* Singleton lock acquire & release */
	jarvis_plat_lock_t *lk1 = jarvis_plat_singleton_acquire("jarvis-seam-locktest");
	assert(lk1 != NULL);
	jarvis_plat_lock_t *lk2 = jarvis_plat_singleton_acquire("jarvis-seam-locktest");
	assert(lk2 == NULL);
	jarvis_plat_singleton_release(lk1);
	jarvis_plat_lock_t *lk3 = jarvis_plat_singleton_acquire("jarvis-seam-locktest");
	assert(lk3 != NULL);
	jarvis_plat_singleton_release(lk3);

	/* Clean singleton lock file created in tmp */
	char lk_path[256];
	jarvis_plat_state_path("jarvis-seam-locktest", ".lock", lk_path, sizeof(lk_path));
	unlink(lk_path);

	/* WAV parser: mono 22050Hz */
	uint8_t wav[44 + 4] = {
		'R','I','F','F', 40,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
		0x22,0x56,0,0, 0x44,0xAC,0,0, 2,0, 16,0,
		'd','a','t','a', 4,0,0,0,
		0x34,0x12, 0x78,0x56
	};
	int16_t *pcm = NULL;
	size_t nsamp = 0;
	int srate = 0;
	assert(jarvis_plat_read_wav_pcm16(wav, sizeof(wav), &pcm, &nsamp, &srate) == 0);
	assert(nsamp == 2 && srate == 22050);
	assert(pcm[0] == 0x1234 && pcm[1] == 0x5678);
	free(pcm);
	printf("  [PASS] platform paths, singleton lock, exec checks, and WAV parser\n");

	/* 2. Audio seam: pw-cat playback, finish, abort, pw-record capture, zero zombies */
	char pw_cat[320], pw_rec[320], pw_cli[320], bytes_log[320];
	snprintf(pw_cat, sizeof(pw_cat), "%s/pw-cat", tmpdir);
	snprintf(pw_rec, sizeof(pw_rec), "%s/pw-record", tmpdir);
	snprintf(pw_cli, sizeof(pw_cli), "%s/pw-cli", tmpdir);
	snprintf(bytes_log, sizeof(bytes_log), "%s/pw.bytes", tmpdir);

	char body[1024];
	snprintf(body, sizeof(body),
	         "#!/bin/sh\n"
	         "if [ \"$1\" = \"-p\" ]; then wc -c >> '%s'; else sleep 1; fi\n",
	         bytes_log);
	write_exec_script(pw_cat, body);
	write_exec_script(pw_rec, "#!/bin/sh\nprintf '\\001\\000\\002\\000\\003\\000\\004\\000'\nsleep 1\n");
	write_exec_script(pw_cli, "#!/bin/sh\necho 'node.name = \"jarvis_aec_source\"'\n");

	char new_path[4096];
	snprintf(new_path, sizeof(new_path), "%s:%s", tmpdir, saved_path ? saved_path : "/usr/bin:/bin");
	stl_setenv("PATH", new_path, 1);

	assert(jarvis_audio_has_capture_backend() == true);
	assert(strcmp(jarvis_audio_backend_name(), "pipewire") == 0);

	jarvis_audio_play_t *play = jarvis_audio_play_open(24000, 1, "jarvis-seam");
	assert(play != NULL);
	assert(jarvis_audio_play_rate(play) == 24000);
	int16_t samples[128];
	for (int i = 0; i < 128; i++) samples[i] = (int16_t)i;
	assert(jarvis_audio_play_write(play, samples, 128) == 0);
	jarvis_audio_play_finish(play);
	jarvis_audio_play_close(play);

	/* Verify abort path stops cleanly and reaps without hanging */
	jarvis_audio_play_t *play2 = jarvis_audio_play_open(22050, 1, "jarvis-seam");
	assert(play2 != NULL);
	assert(jarvis_audio_play_write(play2, samples, 128) == 0);
	jarvis_audio_play_abort(play2);
	jarvis_audio_play_close(play2);

	/* Verify capture seam */
	jarvis_audio_cap_t *cap = jarvis_audio_cap_open(16000, 1);
	assert(cap != NULL);
	int16_t cap_buf[4] = {0};
	assert(jarvis_audio_cap_read(cap, cap_buf, 4) == 4);
	assert(cap_buf[0] == 1 && cap_buf[1] == 2 && cap_buf[2] == 3 && cap_buf[3] == 4);
	jarvis_audio_cap_close(cap);

	assert_no_children();
	printf("  [PASS] audio capture, playback, abort, and zero-zombie cleanup\n");

	/* 3. Ren IPC seam */
	char mods_csv[512];
	assert(jarvis_ren_resolve_mods("x11", NULL, mods_csv, sizeof(mods_csv)) == 0);
	assert(strstr(mods_csv, "ren-jarvis") != NULL);

	char sock_ep[320];
	snprintf(sock_ep, sizeof(sock_ep), "unix:%s/ren-seam.sock", tmpdir);
	stl_setenv("JARVIS_REN_SOCK", sock_ep + 5, 1);
	assert(strstr(jarvis_ren_endpoint(), "ren-seam.sock") != NULL);

	stl_ipc_listener_t listener;
	assert(stl_ipc_listen(jarvis_ren_endpoint(), 4, &listener) == 0);

	pthread_t tid;
	assert(pthread_create(&tid, NULL, ipc_server_thread, &listener) == 0);

	stl_socket_t cfd = jarvis_ren_connect(2000);
	assert(cfd != STL_INVALID_SOCKET);
	assert(jarvis_plat_sock_peer_closed(cfd) == 0);
	jarvis_plat_write_sock_all(cfd, "PING_SEAM\n", 10);

	jarvis_ren_reader_t rr;
	jarvis_ren_reader_init(&rr);
	char line[256];
	assert(jarvis_ren_read_line(cfd, &rr, line, sizeof(line)) == 1);
	assert(strcmp(line, "CLAUSE\tHello seam\t0") == 0);
	assert(jarvis_ren_read_line(cfd, &rr, line, sizeof(line)) == 1);
	assert(strcmp(line, "DONE\t1\tok") == 0);
	assert(jarvis_ren_read_line(cfd, &rr, line, sizeof(line)) == 0);

	stl_sock_close(cfd);
	pthread_join(tid, NULL);
	stl_ipc_close_listener(&listener);
	printf("  [PASS] ren-ipc connect, buffered line read, and peer-closed detection\n");

	/* Cleanup */
	stl_unsetenv("JARVIS_REN_SOCK");
	if (saved_path) { stl_setenv("PATH", saved_path, 1); free(saved_path); }
	if (saved_home) { stl_setenv("HOME", saved_home, 1); free(saved_home); }

	unlink(pw_cat);
	unlink(pw_rec);
	unlink(pw_cli);
	unlink(bytes_log);
	unlink(onnx_file);
	rmdir(piper_dir);
	rmdir(share_dir);
	rmdir(local_dir);
	rmdir(fake_home);
	rmdir(tmpdir);
	return 0;
}
