# libjarvis

[![npm](https://img.shields.io/badge/npm-@tty--pt/jarvis-CB3837?logo=npm)](npm/README.md)
[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](LICENSE)
[![ren Brain](https://img.shields.io/badge/agent-ren%20%2B%20reflex-blueviolet)](#)
[![Voice Pipeline](https://img.shields.io/badge/voice-VAD%20%2B%20Streaming%20TTS-4B8BBE)](#)

> Low-Latency Voice Daemon (`jarvisd`), Thin Unix-Socket CLI (`jarvis`), Multilingual Voice/Ontology & Streaming TTS Library (`libjarvis.so`), Self-Contained Desktop Helpers (`desktop-core.h`), and Modular `ren` Desktop Extensions (`ren-x11`, `ren-firefox`, `ren-desktop`, `ren-sway`, `ren-hyprland`, `ren-jarvis`).

---

## 1. Architecture Overview

`libjarvis` powers the hands-free voice front-end and desktop environment plugins for `ren` (`external/axil-auren`):

```
Microphone -> PipeWire -> jarvisd (adaptive VAD + Whisper Vulkan ASR + Kokoro/Piper/Edge TTS)
                             │
                             ▼  (/tmp/ren-jarvis-$USER.sock inside tmux session "jarvis")
                            ren (libren)
                             ├─► Tier-0 Reflex (ren-reflex.so — ~0.1ms instant rules)
                             ├─► Tier-1 LLM Tool Agent (axil-auren)
                             ├─► Semantic Memory (ren-mm.so / libmm)
                             └─► Desktop Plugins:
                                   ren-jarvis.so, ren-desktop.so, ren-x11.so,
                                   ren-firefox.so, ren-sway.so, ren-hyprland.so
```

- **`jarvis` (`src/jarvis.c`)**: Thin CLI client for controlling `jarvisd` (`start`, `stop`, `toggle`, `status`, `lockdown`, `menu`, `tts-on`, `tts-off`, `speak`, `notify`, `config`), opening interactive `dmenu` bars, and sending commands directly to `/tmp/ren-jarvis-$USER.sock`.
- **`jarvisd` (`src/jarvisd.c`, `src/jarvis-tts.c`)**: Always-ready PipeWire audio daemon with circular float32 PCM buffer, Whisper Vulkan decoding, lazy-loaded ONNX neural TTS (`Kokoro-82M` / `Piper`) + streaming `edge-tts-pcm`, and instant `<1ms` barge-in preemption.
- **`libjarvis.so` (`include/ttypt/jarvis.h`)**: Focused pure-C library exporting:
  - Diacritic-folding phrase cleaning (`jarvis_clean_phrase`), hallucination filtering, and destructive-command guardrails
  - Multilingual `ontology.json` localization & UI string formatting
  - Circular VAD ring buffer, wake-word extraction, and zero-dependency WAV earcon synthesis
  - Streaming JSON answer prefix & prosody-aware TTS clause segmentation
  - `~/.jarvisrc` atomic configuration helpers (`jarvis_rc_get`, `jarvis_rc_set`)
- **`desktop-core.h` (`include/ttypt/desktop-core.h`)**: Header-only POSIX/Linux helpers (zombie-free double-fork detached spawn, `.desktop` + `PATH` app resolution, `/proc` child/cwd inspection, system health, URL/JSON escaping, MPRIS control).

---

## 2. Quickstart CLI Usage

```bash
# Control the voice daemon
jarvis start
jarvis status
jarvis toggle
jarvis menu
jarvis lockdown

# Send natural-language commands to ren over /tmp/ren-jarvis-$USER.sock
jarvis query "switch to workspace 2 and open a terminal"
jarvis dmenu
jarvis speak "Voice synthesis is ready."
jarvis config wake_word Jarbas
```

---

## 3. Build, Test & Install

```sh
make PREFIX="$HOME/.local" all
make test
make PREFIX="$HOME/.local" install
```

- Builds `lib/libjarvis.so`, `bin/jarvis`, `bin/jarvisd`, and all 6 `mods/*/*.so` (`ren-desktop.so`, `ren-jarvis.so`, `ren-x11.so`, `ren-firefox.so`, `ren-sway.so`, `ren-hyprland.so`).
- Installs headers (`<ttypt/jarvis.h>`, `<ttypt/jarvis-types.h>`, `<ttypt/desktop-core.h>`), `jarvis.pc`, binaries, `ren-*.so` plugins into `$PREFIX/lib/ren/`, and systemd/XDG service files.

---

## 4. JavaScript & WebAssembly Bindings (`@tty-pt/jarvis`)

Colocated in [`npm/`](npm/README.md), exposing `cleanPhrase`, `extractStreamingAnswer`, `popTtsClause`, `isDestructiveCommand`, `isHallucination`, `extractWakeCommand`, `writeEarconWav`, and `loadOntologyJson` across Native Node.js N-API and `npm/dist/jarvis.wasm`:

```sh
make wasm
make npm-test
```

## License

BSD-2-Clause License. Copyright (c) 2026, Paulo André Azevedo Quirino. See `LICENSE`.
