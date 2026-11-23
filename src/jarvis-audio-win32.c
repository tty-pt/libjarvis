#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "jarvis-audio.h"
#include "jarvis-platform.h"

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_BUF_BYTES    4096
#define CAP_NUM_BUFS     4
#define CAP_RING_BYTES   (256 * 1024)

#define PLAY_BUF_BYTES   32768
#define PLAY_NUM_BUFS    8

static const GUID jv_CLSID_MMDeviceEnumerator = {
	0xbcde0395, 0xe52f, 0x467c, {0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}
};
static const GUID jv_IID_IMMDeviceEnumerator = {
	0xa95664d2, 0x9614, 0x4f35, {0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}
};
static const GUID jv_IID_IAudioEndpointVolume = {
	0x5cdf2c82, 0x841e, 0x4546, {0x97,0x22,0x0c,0xf7,0x40,0x78,0x22,0x9a}
};
static const GUID jv_IID_IAudioSessionManager2 = {
	0x77aa99a0, 0x1bd6, 0x484f, {0x8b,0xc7,0x2c,0x65,0x4c,0x9a,0x9b,0x6f}
};
static const GUID jv_IID_IAudioSessionControl2 = {
	0xbfb7ff88, 0x7239, 0x4fc9, {0x8f,0xa2,0x07,0xc9,0x50,0xbe,0x9c,0x6d}
};

static float g_duck_saved_vol = -1.0f;

struct jarvis_audio_cap {
	HWAVEIN            hwi;
	HANDLE             ev;
	HANDLE             thread;
	WAVEHDR            hdrs[CAP_NUM_BUFS];
	uint8_t            bufs[CAP_NUM_BUFS][CAP_BUF_BYTES];
	int                sample_rate;
	int                channels;
	uint8_t            ring[CAP_RING_BYTES];
	size_t             r_head;
	size_t             r_tail;
	size_t             r_used;
	CRITICAL_SECTION   cs;
	CONDITION_VARIABLE cv;
	volatile LONG      closing;
};

struct jarvis_audio_play {
	HWAVEOUT      hwo;
	HANDLE        ev;
	WAVEHDR       hdrs[PLAY_NUM_BUFS];
	uint8_t       bufs[PLAY_NUM_BUFS][PLAY_BUF_BYTES];
	uint8_t       in_use[PLAY_NUM_BUFS];
	int           sample_rate;
	int           channels;
	volatile LONG finished;
	volatile LONG aborted;
};

static DWORD WINAPI cap_pump_thread(LPVOID param)
{
	jarvis_audio_cap_t *cap = (jarvis_audio_cap_t *)param;
	while (!InterlockedCompareExchange(&cap->closing, 0, 0)) {
		WaitForSingleObject(cap->ev, 50);
		if (InterlockedCompareExchange(&cap->closing, 0, 0))
			break;
		for (int i = 0; i < CAP_NUM_BUFS; i++) {
			if (!(cap->hdrs[i].dwFlags & WHDR_DONE))
				continue;
			DWORD bytes = cap->hdrs[i].dwBytesRecorded;
			if (bytes > 0) {
				const uint8_t *src = cap->bufs[i];
				size_t n = (size_t)bytes;
				EnterCriticalSection(&cap->cs);
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
				WakeConditionVariable(&cap->cv);
				LeaveCriticalSection(&cap->cs);
			}
			cap->hdrs[i].dwBytesRecorded = 0;
			cap->hdrs[i].dwFlags &= ~WHDR_DONE;
			if (!InterlockedCompareExchange(&cap->closing, 0, 0))
				waveInAddBuffer(cap->hwi, &cap->hdrs[i], sizeof(WAVEHDR));
		}
	}
	return 0;
}

jarvis_audio_cap_t *jarvis_audio_cap_open(int sample_rate, int channels)
{
	jarvis_audio_cap_t *cap = (jarvis_audio_cap_t *)calloc(1, sizeof(*cap));
	if (!cap) return NULL;
	cap->sample_rate = sample_rate > 0 ? sample_rate : 16000;
	cap->channels = channels > 0 ? channels : 1;
	InitializeCriticalSection(&cap->cs);
	InitializeConditionVariable(&cap->cv);

	cap->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!cap->ev) {
		DeleteCriticalSection(&cap->cs);
		free(cap);
		return NULL;
	}

	WAVEFORMATEX wfx;
	memset(&wfx, 0, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = (WORD)cap->channels;
	wfx.nSamplesPerSec = (DWORD)cap->sample_rate;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

	if (waveInOpen(&cap->hwi, WAVE_MAPPER, &wfx, (DWORD_PTR)cap->ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		CloseHandle(cap->ev);
		DeleteCriticalSection(&cap->cs);
		free(cap);
		return NULL;
	}

	for (int i = 0; i < CAP_NUM_BUFS; i++) {
		cap->hdrs[i].lpData = (LPSTR)cap->bufs[i];
		cap->hdrs[i].dwBufferLength = CAP_BUF_BYTES;
		waveInPrepareHeader(cap->hwi, &cap->hdrs[i], sizeof(WAVEHDR));
		waveInAddBuffer(cap->hwi, &cap->hdrs[i], sizeof(WAVEHDR));
	}

	cap->thread = CreateThread(NULL, 0, cap_pump_thread, cap, 0, NULL);
	if (!cap->thread || waveInStart(cap->hwi) != MMSYSERR_NOERROR) {
		jarvis_audio_cap_close(cap);
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

	EnterCriticalSection(&cap->cs);
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
			if (InterlockedCompareExchange(&cap->closing, 0, 0)) {
				LeaveCriticalSection(&cap->cs);
				return 0;
			}
			SleepConditionVariableCS(&cap->cv, &cap->cs, 50);
		}
	}
	LeaveCriticalSection(&cap->cs);
	return (int)n_samples;
}

void jarvis_audio_cap_close(jarvis_audio_cap_t *cap)
{
	if (!cap) return;
	InterlockedExchange(&cap->closing, 1);
	if (cap->ev) SetEvent(cap->ev);
	EnterCriticalSection(&cap->cs);
	WakeAllConditionVariable(&cap->cv);
	LeaveCriticalSection(&cap->cs);

	if (cap->hwi) {
		waveInStop(cap->hwi);
		waveInReset(cap->hwi);
	}
	if (cap->thread) {
		WaitForSingleObject(cap->thread, 2000);
		CloseHandle(cap->thread);
	}
	if (cap->hwi) {
		for (int i = 0; i < CAP_NUM_BUFS; i++) {
			if (cap->hdrs[i].dwFlags & WHDR_PREPARED)
				waveInUnprepareHeader(cap->hwi, &cap->hdrs[i], sizeof(WAVEHDR));
		}
		waveInClose(cap->hwi);
	}
	if (cap->ev) CloseHandle(cap->ev);
	DeleteCriticalSection(&cap->cs);
	free(cap);
}

jarvis_audio_play_t *jarvis_audio_play_open(int sample_rate, int channels, const char *app_name)
{
	(void)app_name;
	jarvis_audio_play_t *play = (jarvis_audio_play_t *)calloc(1, sizeof(*play));
	if (!play) return NULL;
	play->sample_rate = sample_rate > 0 ? sample_rate : 24000;
	play->channels = channels > 0 ? channels : 1;
	play->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!play->ev) {
		free(play);
		return NULL;
	}

	WAVEFORMATEX wfx;
	memset(&wfx, 0, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = (WORD)play->channels;
	wfx.nSamplesPerSec = (DWORD)play->sample_rate;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

	if (waveOutOpen(&play->hwo, WAVE_MAPPER, &wfx, (DWORD_PTR)play->ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		CloseHandle(play->ev);
		free(play);
		return NULL;
	}
	for (int i = 0; i < PLAY_NUM_BUFS; i++) {
		play->hdrs[i].lpData = (LPSTR)play->bufs[i];
		play->hdrs[i].dwBufferLength = PLAY_BUF_BYTES;
		waveOutPrepareHeader(play->hwo, &play->hdrs[i], sizeof(WAVEHDR));
	}
	return play;
}

static void play_reap_done(jarvis_audio_play_t *play)
{
	for (int i = 0; i < PLAY_NUM_BUFS; i++) {
		if (play->in_use[i] && (play->hdrs[i].dwFlags & WHDR_DONE))
			play->in_use[i] = 0;
	}
}

int jarvis_audio_play_write(jarvis_audio_play_t *play, const int16_t *samples, size_t n_samples)
{
	if (!play || !samples) return -1;
	if (n_samples == 0) return 0;

	const uint8_t *ptr = (const uint8_t *)samples;
	size_t rem = n_samples * sizeof(int16_t);

	while (rem > 0) {
		if (InterlockedCompareExchange(&play->aborted, 0, 0) ||
		    InterlockedCompareExchange(&play->finished, 0, 0))
			return -1;

		play_reap_done(play);
		int slot = -1;
		for (int i = 0; i < PLAY_NUM_BUFS; i++) {
			if (!play->in_use[i]) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			WaitForSingleObject(play->ev, 25);
			continue;
		}

		size_t chunk = rem > PLAY_BUF_BYTES ? PLAY_BUF_BYTES : rem;
		memcpy(play->bufs[slot], ptr, chunk);
		play->hdrs[slot].dwBufferLength = (DWORD)chunk;
		play->hdrs[slot].dwFlags &= ~WHDR_DONE;
		play->in_use[slot] = 1;

		if (waveOutWrite(play->hwo, &play->hdrs[slot], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
			play->in_use[slot] = 0;
			return -1;
		}
		ptr += chunk;
		rem -= chunk;
	}
	return 0;
}

void jarvis_audio_play_finish(jarvis_audio_play_t *play)
{
	if (!play) return;
	InterlockedExchange(&play->finished, 1);
	if (play->ev) SetEvent(play->ev);
}

int jarvis_audio_play_poll_done(jarvis_audio_play_t *play)
{
	if (!play) return 1;
	if (InterlockedCompareExchange(&play->aborted, 0, 0))
		return 1;
	play_reap_done(play);
	if (!InterlockedCompareExchange(&play->finished, 0, 0))
		return 0;
	for (int i = 0; i < PLAY_NUM_BUFS; i++) {
		if (play->in_use[i]) return 0;
	}
	return 1;
}

void jarvis_audio_play_abort(jarvis_audio_play_t *play)
{
	if (!play) return;
	InterlockedExchange(&play->aborted, 1);
	InterlockedExchange(&play->finished, 1);
	if (play->ev) SetEvent(play->ev);
	if (play->hwo) waveOutReset(play->hwo);
}

void jarvis_audio_play_close(jarvis_audio_play_t *play)
{
	if (!play) return;
	InterlockedExchange(&play->finished, 1);
	while (!jarvis_audio_play_poll_done(play))
		WaitForSingleObject(play->ev, 25);

	if (play->hwo) {
		waveOutReset(play->hwo);
		for (int i = 0; i < PLAY_NUM_BUFS; i++) {
			if (play->hdrs[i].dwFlags & WHDR_PREPARED)
				waveOutUnprepareHeader(play->hwo, &play->hdrs[i], sizeof(WAVEHDR));
		}
		waveOutClose(play->hwo);
	}
	if (play->ev) CloseHandle(play->ev);
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
	PlaySoundA(wav_path, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

static IAudioEndpointVolume *open_default_endpoint_vol(EDataFlow flow, bool *need_uninit)
{
	if (need_uninit) *need_uninit = false;
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr) && need_uninit) *need_uninit = true;

	IMMDeviceEnumerator *en = NULL;
	hr = CoCreateInstance(&jv_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
	                      &jv_IID_IMMDeviceEnumerator, (void **)&en);
	if (FAILED(hr) || !en) {
		if (need_uninit && *need_uninit) { CoUninitialize(); *need_uninit = false; }
		return NULL;
	}
	IMMDevice *dev = NULL;
	hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, eConsole, &dev);
	IMMDeviceEnumerator_Release(en);
	if (FAILED(hr) || !dev) {
		if (need_uninit && *need_uninit) { CoUninitialize(); *need_uninit = false; }
		return NULL;
	}
	IAudioEndpointVolume *vol = NULL;
	hr = IMMDevice_Activate(dev, &jv_IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&vol);
	IMMDevice_Release(dev);
	if (FAILED(hr) || !vol) {
		if (need_uninit && *need_uninit) { CoUninitialize(); *need_uninit = false; }
		return NULL;
	}
	return vol;
}

void jarvis_audio_unmute_mic(void)
{
	bool need_uninit = false;
	IAudioEndpointVolume *vol = open_default_endpoint_vol(eCapture, &need_uninit);
	if (!vol) return;
	IAudioEndpointVolume_SetMute(vol, FALSE, NULL);
	IAudioEndpointVolume_Release(vol);
	if (need_uninit) CoUninitialize();
}

void jarvis_audio_duck(void)
{
	if (g_duck_saved_vol >= 0.0f) return;
	bool need_uninit = false;
	IAudioEndpointVolume *vol = open_default_endpoint_vol(eRender, &need_uninit);
	if (!vol) return;

	float cur = -1.0f;
	if (SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(vol, &cur)) && cur > 0.08f) {
		g_duck_saved_vol = cur;
		float ducked = cur * 0.28f;
		if (ducked < 0.06f) ducked = 0.06f;
		IAudioEndpointVolume_SetMasterVolumeLevelScalar(vol, ducked, NULL);
	}
	IAudioEndpointVolume_Release(vol);
	if (need_uninit) CoUninitialize();
}

void jarvis_audio_restore(void)
{
	if (g_duck_saved_vol < 0.0f) return;
	float saved = g_duck_saved_vol;
	g_duck_saved_vol = -1.0f;
	bool need_uninit = false;
	IAudioEndpointVolume *vol = open_default_endpoint_vol(eRender, &need_uninit);
	if (!vol) return;
	IAudioEndpointVolume_SetMasterVolumeLevelScalar(vol, saved, NULL);
	IAudioEndpointVolume_Release(vol);
	if (need_uninit) CoUninitialize();
}

int jarvis_audio_is_external_playing(void)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	bool need_uninit = SUCCEEDED(hr);
	int playing = 0;

	IMMDeviceEnumerator *en = NULL;
	hr = CoCreateInstance(&jv_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
	                      &jv_IID_IMMDeviceEnumerator, (void **)&en);
	if (SUCCEEDED(hr) && en) {
		IMMDevice *dev = NULL;
		if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) && dev) {
			IAudioSessionManager2 *mgr = NULL;
			if (SUCCEEDED(IMMDevice_Activate(dev, &jv_IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&mgr)) && mgr) {
				IAudioSessionEnumerator *se = NULL;
				if (SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(mgr, &se)) && se) {
					int count = 0;
					DWORD self_pid = GetCurrentProcessId();
					if (SUCCEEDED(IAudioSessionEnumerator_GetCount(se, &count))) {
						for (int i = 0; i < count && !playing; i++) {
							IAudioSessionControl *sc = NULL;
							if (SUCCEEDED(IAudioSessionEnumerator_GetSession(se, i, &sc)) && sc) {
								AudioSessionState st = AudioSessionStateInactive;
								if (SUCCEEDED(IAudioSessionControl_GetState(sc, &st)) &&
								    st == AudioSessionStateActive) {
									IAudioSessionControl2 *sc2 = NULL;
									if (SUCCEEDED(IAudioSessionControl_QueryInterface(sc, &jv_IID_IAudioSessionControl2, (void **)&sc2)) && sc2) {
										DWORD pid = 0;
										IAudioSessionControl2_GetProcessId(sc2, &pid);
										if (pid != 0 && pid != self_pid)
											playing = 1;
										IAudioSessionControl2_Release(sc2);
									} else {
										playing = 1;
									}
								}
								IAudioSessionControl_Release(sc);
							}
						}
					}
					IAudioSessionEnumerator_Release(se);
				}
				IAudioSessionManager2_Release(mgr);
			}
			IMMDevice_Release(dev);
		}
		IMMDeviceEnumerator_Release(en);
	}
	if (need_uninit) CoUninitialize();
	return playing;
}

bool jarvis_audio_has_capture_backend(void)
{
	return waveInGetNumDevs() > 0;
}

const char *jarvis_audio_backend_name(void)
{
	return "wasapi";
}
