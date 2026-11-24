#define _GNU_SOURCE
#include "jarvis-audio.h"
#include "jarvis-platform.h"

#include <ttypt/stele.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct jarvis_audio_cap {
	stl_proc_t proc;
	int        sample_rate;
	int        channels;
};

struct jarvis_audio_play {
	stl_proc_t proc;
	int        sample_rate;
	int        channels;
	int        done;
};

static double g_duck_saved_vol = -1.0;

static void select_pw_source(char *out, size_t cap)
{
	if (!out || cap == 0) return;
	out[0] = '\0';
	FILE *fp = popen("pw-cli list-objects Node 2>/dev/null", "r");
	if (!fp) return;
	char line[512];
	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "jarvis_aec_source")) {
			snprintf(out, cap, "jarvis_aec_source");
			break;
		}
	}
	pclose(fp);
}

jarvis_audio_cap_t *jarvis_audio_cap_open(int sample_rate, int channels)
{
	jarvis_audio_cap_t *cap = calloc(1, sizeof(*cap));
	if (!cap) return NULL;
	cap->sample_rate = sample_rate > 0 ? sample_rate : 16000;
	cap->channels = channels > 0 ? channels : 1;

	char rate_buf[32], ch_buf[16], target[128];
	snprintf(rate_buf, sizeof(rate_buf), "%d", cap->sample_rate);
	snprintf(ch_buf, sizeof(ch_buf), "%d", cap->channels);
	select_pw_source(target, sizeof(target));

	const char *argv[13];
	int i = 0;
	argv[i++] = "pw-record";
	argv[i++] = "--raw";
	if (target[0]) {
		argv[i++] = "--target";
		argv[i++] = target;
	}
	argv[i++] = "--rate";
	argv[i++] = rate_buf;
	argv[i++] = "--channels";
	argv[i++] = ch_buf;
	argv[i++] = "--format";
	argv[i++] = "s16";
	argv[i++] = "-";
	argv[i] = NULL;

	if (stl_spawn(argv, STL_SPAWN_STDOUT | STL_SPAWN_NEW_GROUP, &cap->proc) < 0) {
		free(cap);
		return NULL;
	}
	return cap;
}

int jarvis_audio_cap_read(jarvis_audio_cap_t *cap, int16_t *dst, size_t n_samples)
{
	if (!cap || !dst || n_samples == 0) return -1;
	int fd = cap->proc.stdout_fd;
	if (fd < 0) return -1;

	size_t total = 0;
	size_t need_bytes = n_samples * sizeof(int16_t);
	uint8_t *bp = (uint8_t *)dst;

	while (total < need_bytes) {
		ssize_t r = read(fd, bp + total, need_bytes - total);
		if (r < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (r == 0) return 0;
		total += (size_t)r;
	}
	return (int)n_samples;
}

void jarvis_audio_cap_close(jarvis_audio_cap_t *cap)
{
	if (!cap) return;
	if (cap->proc.stdout_fd >= 0) {
		close(cap->proc.stdout_fd);
		cap->proc.stdout_fd = -1;
	}
	stl_proc_kill(&cap->proc, 1);
	int ec = 0;
	stl_proc_wait(&cap->proc, &ec, -1);
	stl_proc_close(&cap->proc);
	free(cap);
}

jarvis_audio_play_t *jarvis_audio_play_open(int sample_rate, int channels, const char *app_name)
{
	jarvis_audio_play_t *play = calloc(1, sizeof(*play));
	if (!play) return NULL;
	play->sample_rate = sample_rate > 0 ? sample_rate : 24000;
	play->channels = channels > 0 ? channels : 1;

	char rate_arg[32], ch_arg[32], prop_arg[128];
	snprintf(rate_arg, sizeof(rate_arg), "--rate=%d", play->sample_rate);
	snprintf(ch_arg, sizeof(ch_arg), "--channels=%d", play->channels);
	snprintf(prop_arg, sizeof(prop_arg), "application.name=%s",
	         (app_name && *app_name) ? app_name : "jarvis-tts");

	const char *argv[] = {
		"pw-cat", "-p", "--raw", rate_arg,
		ch_arg, "--format=s16",
		"--media-role=Communication", "-P", prop_arg, "-", NULL
	};

	if (stl_spawn(argv, STL_SPAWN_STDIN | STL_SPAWN_NEW_GROUP, &play->proc) < 0) {
		free(play);
		return NULL;
	}
#ifdef F_SETPIPE_SZ
	if (play->proc.stdin_fd >= 0) {
		(void)fcntl(play->proc.stdin_fd, F_SETPIPE_SZ, 1048576);
	}
#endif
	return play;
}

int jarvis_audio_play_write(jarvis_audio_play_t *play, const int16_t *samples, size_t n_samples)
{
	if (!play || !samples) return -1;
	if (n_samples == 0) return 0;

	const uint8_t *ptr = (const uint8_t *)samples;
	size_t rem = n_samples * sizeof(int16_t);
	while (rem > 0) {
		int fd = __atomic_load_n(&play->proc.stdin_fd, __ATOMIC_SEQ_CST);
		if (fd < 0) return -1;
		size_t chunk = rem > 32768 ? 32768 : rem;
		ssize_t nw = write(fd, ptr, chunk);
		if (nw < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (nw == 0) return -1;
		ptr += nw;
		rem -= (size_t)nw;
	}
	return 0;
}

void jarvis_audio_play_finish(jarvis_audio_play_t *play)
{
	if (!play) return;
	int fd = __atomic_exchange_n(&play->proc.stdin_fd, -1, __ATOMIC_SEQ_CST);
	if (fd >= 0) close(fd);
}

int jarvis_audio_play_poll_done(jarvis_audio_play_t *play)
{
	if (!play) return 1;
	if (play->done) return 1;
	int ec = 0;
	int rc = stl_proc_wait(&play->proc, &ec, 0);
	if (rc == 1) return 0; /* still running */
	play->done = 1;
	return 1;
}

void jarvis_audio_play_abort(jarvis_audio_play_t *play)
{
	if (!play) return;
	jarvis_audio_play_finish(play);
	stl_proc_kill(&play->proc, 1);
}

void jarvis_audio_play_close(jarvis_audio_play_t *play)
{
	if (!play) return;
	jarvis_audio_play_finish(play);
	if (!play->done) {
		int ec = 0;
		stl_proc_wait(&play->proc, &ec, -1);
		play->done = 1;
	}
	stl_proc_close(&play->proc);
	free(play);
}

int jarvis_audio_play_rate(const jarvis_audio_play_t *play)
{
	return play ? play->sample_rate : 0;
}

void jarvis_audio_play_chime_async(const char *wav_path)
{
	if (!wav_path || !*wav_path || !jarvis_plat_is_readable(wav_path))
		return;
	const char *argv[] = { "pw-play", wav_path, NULL };
	(void)stl_spawn_detached(argv);
}

void jarvis_audio_unmute_mic(void)
{
	const char *argv[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "0", NULL };
	(void)stl_spawn_detached(argv);
}

void jarvis_audio_duck(void)
{
	if (g_duck_saved_vol >= 0.0)
		return;
	FILE *fp = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	if (!fp) return;
	char out[128] = {0};
	if (fgets(out, sizeof(out), fp)) {
		double vol = -1.0;
		if (sscanf(out, "Volume: %lf", &vol) == 1 && vol > 0.08) {
			g_duck_saved_vol = vol;
			double ducked = vol * 0.28;
			if (ducked < 0.06) ducked = 0.06;
			char vstr[32];
			snprintf(vstr, sizeof(vstr), "%.2f", ducked);
			const char *argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", vstr, NULL };
			stl_proc_t p;
			if (stl_spawn(argv, 0, &p) == 0) {
				int ec = 0;
				stl_proc_wait(&p, &ec, -1);
				stl_proc_close(&p);
			}
		}
	}
	pclose(fp);
}

void jarvis_audio_restore(void)
{
	if (g_duck_saved_vol < 0.0)
		return;
	char vstr[32];
	snprintf(vstr, sizeof(vstr), "%.2f", g_duck_saved_vol);
	g_duck_saved_vol = -1.0;
	const char *argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", vstr, NULL };
	(void)stl_spawn_detached(argv);
}

int jarvis_audio_is_external_playing(void)
{
	FILE *fp = popen("pactl list sink-inputs 2>/dev/null", "r");
	if (!fp) return 0;

	char line[512];
	int in_block = 0;
	int corked_no = 0;
	int is_jarvis_tts = 0;
	int external_playing = 0;

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "Sink Input #", 12) == 0) {
			if (in_block && corked_no && !is_jarvis_tts) {
				external_playing = 1;
				break;
			}
			in_block = 1;
			corked_no = 0;
			is_jarvis_tts = 0;
		} else if (in_block) {
			if (strstr(line, "Corked: no"))
				corked_no = 1;
			if (strstr(line, "jarvis-tts") || strstr(line, "jarvis_tts") ||
			    strstr(line, "speech-dispatcher"))
				is_jarvis_tts = 1;
		}
	}
	if (in_block && corked_no && !is_jarvis_tts)
		external_playing = 1;

	pclose(fp);
	return external_playing;
}

bool jarvis_audio_has_capture_backend(void)
{
	char path[512];
	return jarvis_plat_find_in_path("pw-record", path, sizeof(path)) == 0;
}

const char *jarvis_audio_backend_name(void)
{
	return "pipewire";
}
