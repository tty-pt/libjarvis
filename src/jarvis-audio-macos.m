#include "jarvis-audio.h"
#include "jarvis-platform.h"

#include <ttypt/stele.h>

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef kAudioObjectPropertyElementMain
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

#define CAP_BUF_BYTES    4096
#define CAP_NUM_BUFS     4
#define CAP_RING_BYTES   (256 * 1024)

#define PLAY_BUF_BYTES   32768
#define PLAY_NUM_BUFS    8

static atomic_int g_active_play_streams = 0;
static float g_duck_saved_vol = -1.0f;

struct jarvis_audio_cap {
	AudioQueueRef         queue;
	AudioQueueBufferRef   bufs[CAP_NUM_BUFS];
	int                   sample_rate;
	int                   channels;
	uint8_t               ring[CAP_RING_BYTES];
	size_t                r_head;
	size_t                r_tail;
	size_t                r_used;
	pthread_mutex_t       mu;
	pthread_cond_t        cv;
	int                   closing;
};

struct jarvis_audio_play {
	AudioQueueRef         queue;
	AudioQueueBufferRef   bufs[PLAY_NUM_BUFS];
	uint8_t               in_use[PLAY_NUM_BUFS];
	int                   sample_rate;
	int                   channels;
	int                   pending_bufs;
	int                   finished;
	int                   aborted;
	int                   started;
	pthread_mutex_t       mu;
	pthread_cond_t        cv;
};

static void cap_input_cb(
	void *inUserData,
	AudioQueueRef inAQ,
	AudioQueueBufferRef inBuffer,
	const AudioTimeStamp *inStartTime,
	UInt32 inNumberPacketDescriptions,
	const AudioStreamPacketDescription *inPacketDescs)
{
	(void)inStartTime;
	(void)inNumberPacketDescriptions;
	(void)inPacketDescs;
	jarvis_audio_cap_t *cap = (jarvis_audio_cap_t *)inUserData;
	if (!cap) return;

	pthread_mutex_lock(&cap->mu);
	if (!cap->closing && inBuffer && inBuffer->mAudioDataByteSize > 0) {
		const uint8_t *src = (const uint8_t *)inBuffer->mAudioData;
		size_t n = inBuffer->mAudioDataByteSize;
		if (n > CAP_RING_BYTES) {
			src += (n - CAP_RING_BYTES);
			n = CAP_RING_BYTES;
		}
		if (cap->r_used + n > CAP_RING_BYTES) {
			size_t drop = (cap->r_used + n) - CAP_RING_BYTES;
			cap->r_tail = (cap->r_tail + drop) % CAP_RING_BYTES;
			cap->r_used -= drop;
		}
		size_t first = CAP_RING_BYTES - cap->r_head;
		if (first > n) first = n;
		memcpy(cap->ring + cap->r_head, src, first);
		if (n > first)
			memcpy(cap->ring, src + first, n - first);
		cap->r_head = (cap->r_head + n) % CAP_RING_BYTES;
		cap->r_used += n;
		pthread_cond_signal(&cap->cv);
	}
	int closing = cap->closing;
	pthread_mutex_unlock(&cap->mu);

	if (!closing && inBuffer)
		AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

jarvis_audio_cap_t *jarvis_audio_cap_open(int sample_rate, int channels)
{
	jarvis_audio_cap_t *cap = (jarvis_audio_cap_t *)calloc(1, sizeof(*cap));
	if (!cap) return NULL;
	cap->sample_rate = sample_rate > 0 ? sample_rate : 16000;
	cap->channels = channels > 0 ? channels : 1;
	pthread_mutex_init(&cap->mu, NULL);
	pthread_cond_init(&cap->cv, NULL);

	AudioStreamBasicDescription fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.mSampleRate = (Float64)cap->sample_rate;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	fmt.mBitsPerChannel = 16;
	fmt.mChannelsPerFrame = (UInt32)cap->channels;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = (UInt32)(cap->channels * 2);
	fmt.mBytesPerPacket = fmt.mBytesPerFrame;

	OSStatus st = AudioQueueNewInput(&fmt, cap_input_cb, cap, NULL, NULL, 0, &cap->queue);
	if (st != noErr || !cap->queue) {
		pthread_cond_destroy(&cap->cv);
		pthread_mutex_destroy(&cap->mu);
		free(cap);
		return NULL;
	}

	for (int i = 0; i < CAP_NUM_BUFS; i++) {
		if (AudioQueueAllocateBuffer(cap->queue, CAP_BUF_BYTES, &cap->bufs[i]) != noErr) {
			AudioQueueDispose(cap->queue, true);
			pthread_cond_destroy(&cap->cv);
			pthread_mutex_destroy(&cap->mu);
			free(cap);
			return NULL;
		}
		AudioQueueEnqueueBuffer(cap->queue, cap->bufs[i], 0, NULL);
	}

	if (AudioQueueStart(cap->queue, NULL) != noErr) {
		AudioQueueDispose(cap->queue, true);
		pthread_cond_destroy(&cap->cv);
		pthread_mutex_destroy(&cap->mu);
		free(cap);
		return NULL;
	}
	return cap;
}

int jarvis_audio_cap_read(jarvis_audio_cap_t *cap, int16_t *dst, size_t n_samples)
{
	if (!cap || !dst || n_samples == 0) return -1;
	size_t need = n_samples * sizeof(int16_t);
	uint8_t *bp = (uint8_t *)dst;
	size_t got = 0;

	pthread_mutex_lock(&cap->mu);
	while (got < need) {
		if (cap->r_used > 0) {
			size_t chunk = cap->r_used;
			if (chunk > (need - got)) chunk = need - got;
			size_t first = CAP_RING_BYTES - cap->r_tail;
			if (first > chunk) first = chunk;
			memcpy(bp + got, cap->ring + cap->r_tail, first);
			if (chunk > first)
				memcpy(bp + got + first, cap->ring, chunk - first);
			cap->r_tail = (cap->r_tail + chunk) % CAP_RING_BYTES;
			cap->r_used -= chunk;
			got += chunk;
		} else {
			if (cap->closing) {
				pthread_mutex_unlock(&cap->mu);
				return 0;
			}
			pthread_cond_wait(&cap->cv, &cap->mu);
		}
	}
	pthread_mutex_unlock(&cap->mu);
	return (int)n_samples;
}

void jarvis_audio_cap_close(jarvis_audio_cap_t *cap)
{
	if (!cap) return;
	pthread_mutex_lock(&cap->mu);
	cap->closing = 1;
	pthread_cond_broadcast(&cap->cv);
	pthread_mutex_unlock(&cap->mu);

	if (cap->queue) {
		AudioQueueStop(cap->queue, true);
		AudioQueueDispose(cap->queue, true);
	}
	pthread_cond_destroy(&cap->cv);
	pthread_mutex_destroy(&cap->mu);
	free(cap);
}

static void play_output_cb(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
	(void)inAQ;
	jarvis_audio_play_t *play = (jarvis_audio_play_t *)inUserData;
	if (!play) return;

	pthread_mutex_lock(&play->mu);
	for (int i = 0; i < PLAY_NUM_BUFS; i++) {
		if (play->bufs[i] == inBuffer) {
			play->in_use[i] = 0;
			break;
		}
	}
	if (play->pending_bufs > 0)
		play->pending_bufs--;
	pthread_cond_broadcast(&play->cv);
	pthread_mutex_unlock(&play->mu);
}

jarvis_audio_play_t *jarvis_audio_play_open(int sample_rate, int channels, const char *app_name)
{
	(void)app_name;
	jarvis_audio_play_t *play = (jarvis_audio_play_t *)calloc(1, sizeof(*play));
	if (!play) return NULL;
	play->sample_rate = sample_rate > 0 ? sample_rate : 24000;
	play->channels = channels > 0 ? channels : 1;
	pthread_mutex_init(&play->mu, NULL);
	pthread_cond_init(&play->cv, NULL);

	AudioStreamBasicDescription fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.mSampleRate = (Float64)play->sample_rate;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	fmt.mBitsPerChannel = 16;
	fmt.mChannelsPerFrame = (UInt32)play->channels;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = (UInt32)(play->channels * 2);
	fmt.mBytesPerPacket = fmt.mBytesPerFrame;

	if (AudioQueueNewOutput(&fmt, play_output_cb, play, NULL, NULL, 0, &play->queue) != noErr ||
	    !play->queue) {
		pthread_cond_destroy(&play->cv);
		pthread_mutex_destroy(&play->mu);
		free(play);
		return NULL;
	}

	for (int i = 0; i < PLAY_NUM_BUFS; i++) {
		if (AudioQueueAllocateBuffer(play->queue, PLAY_BUF_BYTES, &play->bufs[i]) != noErr) {
			AudioQueueDispose(play->queue, true);
			pthread_cond_destroy(&play->cv);
			pthread_mutex_destroy(&play->mu);
			free(play);
			return NULL;
		}
	}
	AudioQueueSetParameter(play->queue, kAudioQueueParam_Volume, 1.0f);
	atomic_fetch_add(&g_active_play_streams, 1);
	return play;
}

int jarvis_audio_play_write(jarvis_audio_play_t *play, const int16_t *samples, size_t n_samples)
{
	if (!play || !samples) return -1;
	if (n_samples == 0) return 0;

	const uint8_t *ptr = (const uint8_t *)samples;
	size_t rem = n_samples * sizeof(int16_t);

	while (rem > 0) {
		pthread_mutex_lock(&play->mu);
		int slot = -1;
		while (!play->aborted && !play->finished) {
			for (int i = 0; i < PLAY_NUM_BUFS; i++) {
				if (!play->in_use[i]) {
					slot = i;
					break;
				}
			}
			if (slot >= 0) break;
			pthread_cond_wait(&play->cv, &play->mu);
		}
		if (play->aborted || play->finished || slot < 0) {
			pthread_mutex_unlock(&play->mu);
			return -1;
		}

		size_t chunk = rem > PLAY_BUF_BYTES ? PLAY_BUF_BYTES : rem;
		AudioQueueBufferRef buf = play->bufs[slot];
		memcpy(buf->mAudioData, ptr, chunk);
		buf->mAudioDataByteSize = (UInt32)chunk;
		play->in_use[slot] = 1;
		play->pending_bufs++;
		int need_start = !play->started;
		if (need_start) play->started = 1;
		pthread_mutex_unlock(&play->mu);

		if (AudioQueueEnqueueBuffer(play->queue, buf, 0, NULL) != noErr) {
			pthread_mutex_lock(&play->mu);
			play->in_use[slot] = 0;
			if (play->pending_bufs > 0) play->pending_bufs--;
			pthread_mutex_unlock(&play->mu);
			return -1;
		}
		if (need_start)
			AudioQueueStart(play->queue, NULL);

		ptr += chunk;
		rem -= chunk;
	}
	return 0;
}

void jarvis_audio_play_finish(jarvis_audio_play_t *play)
{
	if (!play) return;
	pthread_mutex_lock(&play->mu);
	play->finished = 1;
	pthread_cond_broadcast(&play->cv);
	pthread_mutex_unlock(&play->mu);
}

int jarvis_audio_play_poll_done(jarvis_audio_play_t *play)
{
	if (!play) return 1;
	pthread_mutex_lock(&play->mu);
	int done = (play->aborted || (play->finished && play->pending_bufs == 0)) ? 1 : 0;
	pthread_mutex_unlock(&play->mu);
	return done;
}

void jarvis_audio_play_abort(jarvis_audio_play_t *play)
{
	if (!play) return;
	pthread_mutex_lock(&play->mu);
	play->aborted = 1;
	play->finished = 1;
	pthread_cond_broadcast(&play->cv);
	pthread_mutex_unlock(&play->mu);

	if (play->queue)
		AudioQueueStop(play->queue, true);
}

void jarvis_audio_play_close(jarvis_audio_play_t *play)
{
	if (!play) return;
	pthread_mutex_lock(&play->mu);
	play->finished = 1;
	while (!play->aborted && play->pending_bufs > 0)
		pthread_cond_wait(&play->cv, &play->mu);
	pthread_mutex_unlock(&play->mu);

	if (play->queue) {
		AudioQueueStop(play->queue, true);
		AudioQueueDispose(play->queue, true);
	}
	atomic_fetch_sub(&g_active_play_streams, 1);
	pthread_cond_destroy(&play->cv);
	pthread_mutex_destroy(&play->mu);
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
	const char *argv[] = { "afplay", wav_path, NULL };
	(void)stl_spawn_detached(argv);
}

static AudioDeviceID get_default_audio_device(AudioObjectPropertySelector sel)
{
	AudioDeviceID dev = kAudioObjectUnknown;
	UInt32 sz = sizeof(dev);
	AudioObjectPropertyAddress addr = {
		sel,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &sz, &dev) != noErr)
		return kAudioObjectUnknown;
	return dev;
}

void jarvis_audio_unmute_mic(void)
{
	AudioDeviceID dev = get_default_audio_device(kAudioHardwarePropertyDefaultInputDevice);
	if (dev == kAudioObjectUnknown) return;

	UInt32 unmute = 0;
	for (UInt32 ch = 0; ch <= 2; ch++) {
		AudioObjectPropertyAddress addr = {
			kAudioDevicePropertyMute,
			kAudioDevicePropertyScopeInput,
			ch
		};
		Boolean settable = false;
		if (AudioObjectHasProperty(dev, &addr) &&
		    AudioObjectIsPropertySettable(dev, &addr, &settable) == noErr && settable) {
			AudioObjectSetPropertyData(dev, &addr, 0, NULL, sizeof(unmute), &unmute);
		}
	}
}

static bool coreaudio_get_out_vol(AudioDeviceID dev, float *out_vol)
{
	AudioObjectPropertyAddress addr = {
		kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
		kAudioDevicePropertyScopeOutput,
		kAudioObjectPropertyElementMain
	};
	UInt32 sz = sizeof(float);
	if (AudioObjectHasProperty(dev, &addr) &&
	    AudioObjectGetPropertyData(dev, &addr, 0, NULL, &sz, out_vol) == noErr)
		return true;

	for (UInt32 ch = 0; ch <= 2; ch++) {
		AudioObjectPropertyAddress ch_addr = {
			kAudioDevicePropertyVolumeScalar,
			kAudioDevicePropertyScopeOutput,
			ch
		};
		if (AudioObjectHasProperty(dev, &ch_addr) &&
		    AudioObjectGetPropertyData(dev, &ch_addr, 0, NULL, &sz, out_vol) == noErr)
			return true;
	}
	return false;
}

static void coreaudio_set_out_vol(AudioDeviceID dev, float vol)
{
	AudioObjectPropertyAddress addr = {
		kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
		kAudioDevicePropertyScopeOutput,
		kAudioObjectPropertyElementMain
	};
	Boolean settable = false;
	if (AudioObjectHasProperty(dev, &addr) &&
	    AudioObjectIsPropertySettable(dev, &addr, &settable) == noErr && settable) {
		AudioObjectSetPropertyData(dev, &addr, 0, NULL, sizeof(vol), &vol);
		return;
	}
	for (UInt32 ch = 0; ch <= 2; ch++) {
		AudioObjectPropertyAddress ch_addr = {
			kAudioDevicePropertyVolumeScalar,
			kAudioDevicePropertyScopeOutput,
			ch
		};
		if (AudioObjectHasProperty(dev, &ch_addr) &&
		    AudioObjectIsPropertySettable(dev, &ch_addr, &settable) == noErr && settable) {
			AudioObjectSetPropertyData(dev, &ch_addr, 0, NULL, sizeof(vol), &vol);
		}
	}
}

void jarvis_audio_duck(void)
{
	if (g_duck_saved_vol >= 0.0f) return;
	AudioDeviceID dev = get_default_audio_device(kAudioHardwarePropertyDefaultOutputDevice);
	if (dev == kAudioObjectUnknown) return;

	float vol = -1.0f;
	if (!coreaudio_get_out_vol(dev, &vol) || vol <= 0.08f) return;
	g_duck_saved_vol = vol;
	float ducked = vol * 0.28f;
	if (ducked < 0.06f) ducked = 0.06f;
	coreaudio_set_out_vol(dev, ducked);
}

void jarvis_audio_restore(void)
{
	if (g_duck_saved_vol < 0.0f) return;
	float saved = g_duck_saved_vol;
	g_duck_saved_vol = -1.0f;
	AudioDeviceID dev = get_default_audio_device(kAudioHardwarePropertyDefaultOutputDevice);
	if (dev == kAudioObjectUnknown) return;
	coreaudio_set_out_vol(dev, saved);
}

int jarvis_audio_is_external_playing(void)
{
	if (atomic_load(&g_active_play_streams) > 0)
		return 0;

	AudioDeviceID dev = get_default_audio_device(kAudioHardwarePropertyDefaultOutputDevice);
	if (dev == kAudioObjectUnknown) return 0;

	UInt32 running = 0;
	UInt32 sz = sizeof(running);
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyDeviceIsRunningSomewhere,
		kAudioDevicePropertyScopeOutput,
		kAudioObjectPropertyElementMain
	};
	if (AudioObjectHasProperty(dev, &addr) &&
	    AudioObjectGetPropertyData(dev, &addr, 0, NULL, &sz, &running) == noErr) {
		return running ? 1 : 0;
	}
	return 0;
}

bool jarvis_audio_has_capture_backend(void)
{
	return get_default_audio_device(kAudioHardwarePropertyDefaultInputDevice) != kAudioObjectUnknown;
}

const char *jarvis_audio_backend_name(void)
{
	return "coreaudio";
}
