# libjarvis

[![npm](https://img.shields.io/badge/npm-@tty--pt/jarvis-CB3837?logo=npm)](npm/README.md)
[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](LICENSE)
[![ren Brain](https://img.shields.io/badge/agent-ren%20%2B%20reflex-blueviolet)](#)
[![Voice Pipeline](https://img.shields.io/badge/voice-VAD%20%2B%20Streaming%20TTS-4B8BBE)](#)

> Low-Latency Voice Daemon (`jarvisd`), Thin IPC CLI (`jarvis`), Multilingual Voice/Ontology & Streaming TTS Library (`libjarvis.so`), Self-Contained Desktop Helpers (`desktop-core.h`), and Cross-Platform `ren` Desktop Extensions (`ren-x11`, `ren-firefox`, `ren-desktop`, `ren-sway`, `ren-hyprland`, `ren-macos`, `ren-win32`, `ren-jarvis`).

---

## 1. Architecture Overview

`libjarvis` powers the hands-free voice front-end and desktop environment plugins for `ren` (`external/axil-auren`) across Linux, macOS, and Windows:

```
Microphone -> 20ms Float32 Capture (PipeWire/Pulse AEC | CoreAudio | WASAPI)
                -> Front-End DSP (110 Hz HPF + GCC-PHAT / Coherence + optional RNNoise)
                -> Silero Neural VAD + ONNX KWS + Multi-Backend STT (Whisper / Parakeet-TDT-0.6B-v3)
                -> LocalAgreement-2 Streaming Dictation & Kokoro/Piper/Edge TTS (jarvisd)
                     │
                     ▼  (libstele IPC / /tmp/ren-jarvis-$USER.sock)
                    ren (libren)
                     ├─► Tier-0 Reflex (ren-reflex.so — ~0.1ms instant rules)
                     ├─► Tier-1 LLM Tool Agent (axil-auren)
                     ├─► Semantic Memory (ren-mm.so / libmm)
                     └─► Desktop & Accessibility Plugins:
                           ren-jarvis.so, ren-desktop.so (AT-SPI2 / AX / UIA),
                           ren-x11.so, ren-firefox.so, ren-sway.so,
                           ren-hyprland.so, ren-macos.dylib, ren-win32.dll
```

- **`jarvis` (`src/jarvis.c`)**: Thin CLI client for controlling `jarvisd` (`start`, `stop`, `restart`, `toggle`, `status`, `doctor`, `setup`, `locales`, `lockdown`, `menu`, `tts-on`, `tts-off`, `speak`, `notify`, `config`), opening interactive `dmenu` bars, and sending commands directly to `ren`.
- **`jarvisd` (`src/jarvisd.c`, `src/jarvis-stt*.c`, `src/jarvis-vad.c`, `src/jarvis-kws.c`, `src/jarvis-dsp.c`, `src/jarvis-stream.c`, `src/jarvis-tts.c`)**: Modular always-ready audio daemon with 20 ms float32 capture, Silero neural VAD, ONNX neural KWS, GCC-PHAT/coherence front-end DSP, pluggable `jarvis_stt_backend_t` (`Whisper` and `Parakeet-TDT-0.6B-v3`) with hardware profile calibration (`JARVIS_BACKEND=auto|metal|cuda|vulkan|cpu`), LocalAgreement-2 streaming dictation, lazy-loaded ONNX neural TTS (`Kokoro-82M` / `Piper`) + persistent `edge-tts-pcm --serve` + 32-slot LRU PCM cache, push-to-talk (PTT), and instant `<1ms` barge-in preemption.
- **`libjarvis.so` (`include/ttypt/jarvis.h`)**: Focused pure-C library exporting:
  - Diacritic-folding phrase cleaning (`jarvis_clean_phrase`), hallucination filtering, and destructive-command guardrails
  - Multilingual `data/ontology.json` localization, `continuation_tokens`, regional `dictation_seed` inheritance, & UI string formatting
  - Circular VAD ring buffer, wake-word extraction, and zero-dependency WAV earcon synthesis
  - Streaming JSON answer prefix & prosody-aware TTS clause segmentation
  - `~/.jarvisrc` atomic configuration helpers (`jarvis_rc_get`, `jarvis_rc_set`)
- **`desktop-core.h` (`include/ttypt/desktop-core.h`)**: Header-only desktop helpers (zombie-free double-fork detached spawn, `.desktop` + `PATH` app resolution, `/proc` child/cwd inspection, system health, URL/JSON escaping, MPRIS control).

👉 *See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [ROADMAP.md](ROADMAP.md) for subsystem architecture and STT/VAD/DSP/KWS/streaming design.*

---

## 2. Zero-Config & 1-Line Language Switching

`jarvis` works out of the box with no `~/.jarvisrc` required:
- Defaults to `auto` for `lang` (resolves `$LC_ALL` / `$LC_MESSAGES` / `$LANG` or falls back to `en`), `wake_word` (`data/ontology.json` default), `tts_voice` (`edge:*` with automatic offline ONNX fallback), `pw_target` (prefers PipeWire `jarvis_aec_source` when loaded), `wm` (detects `dwm` / `sway` / `hyprland` / `x11` / `macos` / `win32`), `whisper_model`, and `auren_url`.
- Switching language is a single line in `~/.jarvisrc`:

```ini
lang = pt-pt
tts_voice = auto    # or tts_voice = local for 100% offline Piper/Kokoro ONNX
```

Install PipeWire AEC source for speaker echo cancellation (`data/99-jarvis-aec.conf`):

```bash
jarvis setup        # checks/installs PipeWire AEC drop-in + reports missing models
jarvis doctor       # verifies daemon, ren socket, audio target, Whisper, TTS, and mods
```

---

## 3. Quickstart CLI Usage

```bash
# Control the voice daemon & diagnostics
jarvis start
jarvis restart
jarvis status
jarvis doctor
jarvis locales
jarvis toggle
jarvis menu
jarvis lockdown

# Send natural-language commands to ren over /tmp/ren-jarvis-$USER.sock
jarvis query "switch to workspace 2 and open a terminal"
jarvis dmenu
jarvis speak "Voice synthesis is ready."
jarvis config lang pt-pt
```

---

## 4. Build, Test & Install

```sh
make PREFIX="$HOME/.local" all           # optionally pass JARVIS_BACKEND=vulkan|cuda|metal|cpu
make test
make PREFIX="$HOME/.local" install
```

- Builds `lib/libjarvis.so`, `bin/jarvis`, `bin/jarvisd`, and platform-matched `mods/*/*` plugins (`ren-desktop`, `ren-jarvis`, `ren-x11`, `ren-firefox`, `ren-sway`, `ren-hyprland` on Linux/BSD; `ren-macos`, `ren-firefox` on macOS; `ren-win32`, `ren-firefox` on Windows).
- Unit tests under `tests/` cover `test_jarvis`, `test_tts`, `test_dsp`, `test_vad`, `test_kws`, `test_stream`, `test_stt_backend`, `test_ren_mods`, and `test_seams`.
- Installs headers (`<ttypt/jarvis.h>`, `<ttypt/jarvis-types.h>`, `<ttypt/desktop-core.h>`), `jarvis.pc`, binaries, `data/99-jarvis-aec.conf`, `ren-*` plugins into `$PREFIX/lib/ren/`, and cross-platform service files from `services/` (`systemd`, `openrc`, `freebsd`, `rc.d`, `launchd`, `xdg`, `windows`) via `make install-services`.

---

## 5. JavaScript & WebAssembly Bindings (`@tty-pt/jarvis`)

Colocated in [`npm/`](npm/README.md), exposing `cleanPhrase`, `extractStreamingAnswer`, `popTtsClause`, `isDestructiveCommand`, `isHallucination`, `extractWakeCommand`, `writeEarconWav`, and `loadOntologyJson` across Native Node.js N-API and `npm/dist/jarvis.wasm`:

```sh
make wasm
make npm-test
```

## License

BSD-2-Clause License. Copyright (c) 2026, Paulo André Azevedo Quirino. See `LICENSE`.
