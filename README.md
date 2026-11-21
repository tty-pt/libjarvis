# libjarvis

[![npm](https://img.shields.io/badge/npm-@tty--pt/jarvis-CB3837?logo=npm)](npm/README.md)
[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](LICENSE)
[![Tier-0 Solver](https://img.shields.io/badge/solver-Tier--0%20~0.1ms-16A34A)](#)
[![ReAct Agent](https://img.shields.io/badge/agent-ReAct%20%2B%20Memory-blueviolet)](#)
[![Voice Pipeline](https://img.shields.io/badge/voice-VAD%20%2B%20Streaming%20TTS-4B8BBE)](#)

> Universal Compositional Semantic Solver, Cross-Platform Intent Intermediate Representation (Intent IR), Multi-Turn ReAct Agent Orchestrator, Low-Latency Voice Pipeline, and `libxylem` Environment Adapters in pure C99 and WebAssembly. Canonical headers are `<ttypt/jarvis.h>` and `<ttypt/jarvis-types.h>`.

---

## 1. Quickstart

### The `jarvis` CLI Tool (< 30 Seconds)
Solve and execute natural language commands across desktop window managers (`dwm`, `sway`, `hyprland`), terminals (`tmux`, POSIX shell), browsers (`qutebrowser`, `firefox`), web applications (`site`), and game worlds (`NeverDark`):

#### English Usage:
```bash
# 1. Deterministic Tier-0 command execution (~0.1 ms, zero LLM round-trip):
jarvis "switch to workspace two and open a terminal there"
jarvis "let's listen to some miles davis"
jarvis "scroll up 8"
jarvis "what time is it"

# 2. Dry-run inspection (prints compiled Intent IR + Action DSL JSON without executing):
jarvis --dry-run "move this window to workspace three"

# 3. Teach a persistent reusable workflow and invoke it later:
jarvis "remember morning setup as switch to workspace 2 and open a terminal"
jarvis "run morning setup"

# 4. Direct neural voice synthesis output:
jarvis speak "Hello! Jarvis voice pipeline is ready and standing by."

# 5. Multi-turn observation & reasoning across terminal panes and browser accessibility:
jarvis "what error is showing in the other terminal"
jarvis "open that file at the failing line"
```

#### Portuguese Usage (Uso em Português):
```bash
# 1. Execução determinística Tier-0 (~0.1 ms, sem latência de LLM):
jarvis "muda para o workspace dois e abre um terminal aí"
jarvis "vamos ouvir miles davis"
jarvis "sobe 8"
jarvis "que horas são"

# 2. Inspeção em modo de teste (--dry-run: gera Intent IR + Action DSL sem executar):
jarvis --dry-run "move esta janela para o workspace três"

# 3. Memorizar um fluxo de trabalho reutilizável e executá-lo:
jarvis "lembra configuração matinal como muda para o workspace 2 e abre um terminal"
jarvis "executa configuração matinal"

# 4. Saída direta por síntese de voz neural:
jarvis speak "Olá! O Jarvis está pronto e configurado em português."

# 5. Diagnóstico e raciocínio multi-turno (terminais, navegador e sistema):
jarvis "que erro está a aparecer no outro terminal"
jarvis "abre esse ficheiro na linha do erro"
```

### In-Memory C API
Deterministically solve a natural language utterance into Intent IR and Action DSL, or run the full stateful orchestrator in under 20 lines:

```c
#include <stdio.h>
#include <ttypt/jarvis.h>

int main(void) {
    jarvis_config_t cfg;
    jarvis_config_init(&cfg);
    cfg.dry_run = true;

    jarvis_engine_t *eng = jarvis_engine_create(&cfg);
    jarvis_process_result_t res;

    if (jarvis_process(eng, "switch to workspace two and open a terminal", "any", &res)) {
        for (size_t i = 0; i < res.step_count; i++) {
            printf("Step %zu: clause=\"%s\" -> op=%s (score=%.2f)\n",
                   i, res.steps[i].resolved_clause,
                   res.steps[i].dsl.op, res.steps[i].ir.score);
        }
    }

    jarvis_engine_destroy(eng);
    return 0;
}
```

Compile and link:
```sh
cc demo.c $(pkg-config --cflags --libs jarvis) -o demo
```

---

## 2. Architecture & Compositional Pipeline

`libjarvis` decouples natural language understanding from environment-specific execution through a four-stage pipeline and `libxylem` (`XY_DECL` / `XY_IMPL`) dynamic hooks:

```
Utterance ("switch to workspace two and open a terminal there")
  │
  ├─► 1. Clause Splitter & Anaphora Resolver (jarvis_split_clauses, jarvis_resolve_anaphora)
  │      Splits compound junctions while protecting "and press enter" & dictation spans;
  │      propagates deictic references ("it", "that", "there") across clauses and turns.
  │
  ├─► 2. Tier-0 Deterministic Compositional Solver (~0.1 ms, jarvis_solve_clause)
  │      Matches normalized ASR phrases against live ontology + environment entities
  │      (windows, tabs, directories, apps, spoken numbers) -> Platform-Neutral Intent IR.
  │
  ├─► 3. Tier-1 / Tier-2 ReAct Agent Fallback (axil-auren remote inference, qwen3.5-4b)
  │      Invoked only for out-of-distribution queries or multi-step diagnostics
  │      (e.g. inspecting live terminal tracebacks or browser AT-SPI2 accessibility trees).
  │
  └─► 4. Xylem Environment Adapters (jarvis_xy_observe, jarvis_xy_execute, jarvis_xy_confirm)
         Dispatches compiled Action DSL to loaded modules (dwm, mod_sway.so, mod_hyprland.so,
         browser, tmux, site, NeverDark) with destructive-command confirmation guardrails.
```

### The `libxylem` Two-Header Rule
To enforce strict encapsulation across dynamically loaded `.so` adapter modules:
1. **`<ttypt/jarvis-types.h>` (Shared Types Header)**: Defines `jarvis_world_t`, `jarvis_ir_t`, `jarvis_dsl_t`, `jarvis_adapter_result_t`, and `jarvis_context_t`. Adapter plugins (such as `mods/sway/mod_sway.c` and `mods/hyprland/mod_hyprland.c`) include `<ttypt/jarvis-types.h>` and `<ttypt/xy-mod.h>` to implement `XY_IMPL(int, jarvis_xy_execute, ...)`.
2. **`<ttypt/jarvis.h>` (Caller & Hook Declaration Header)**: Declares `XY_DECL` hooks and orchestrator functions for host applications.

---

## 3. Core API Reference

| Function / Hook | Signature | Description |
| :--- | :--- | :--- |
| `jarvis_engine_create` | `jarvis_engine_t *jarvis_engine_create(const jarvis_config_t *cfg)` | Initialize stateful orchestrator, ontology, context, and workflow memory |
| `jarvis_process` | `bool jarvis_process(eng, utterance, surface, &out_res)` | End-to-end utterance execution (Solver $\rightarrow$ ReAct $\rightarrow$ Xylem Adapter) |
| `jarvis_solve_clause` | `bool jarvis_solve_clause(ont, clause, surface, world, &out_ir)` | Deterministic ~0.1 ms compositional clause solver producing `jarvis_ir_t` |
| `jarvis_compile_ir_to_dsl` | `bool jarvis_compile_ir_to_dsl(ir, surface, ctx, world, &out_dsl)` | Compile platform-neutral Intent IR into executable `jarvis_dsl_t` |
| `jarvis_split_clauses` | `size_t jarvis_split_clauses(ont, utterance, out_clauses, max)` | Split multi-clause utterances while preserving `"and press enter"` and quotes |
| `jarvis_normalize_dsl_json` | `bool jarvis_normalize_dsl_json(json_str, &out_dsl)` | Heal LLM JSON operation aliases (`run_command`, `goto_workspace`) into canonical DSL |
| `jarvis_memory_remember` | `bool jarvis_memory_remember(eng, key, value)` | Persist a user-taught workflow or preference into long-term memory |
| `jarvis_vad_push_frame` | `bool jarvis_vad_push_frame(vad, frame_samples, count)` | Push 20 ms 16 kHz PCM frame into circular ring buffer and update adaptive VAD |
| `jarvis_pop_ready_tts_clause` | `bool jarvis_pop_ready_tts_clause(unspoken, first, clause, ...)` | Segment streaming LLM answers into ~1.0s TTFA spoken clauses |
| `jarvis_xy_observe` | `XY_DECL(int, jarvis_xy_observe, jarvis_world_t *, world)` | Xylem hook for environment adapters to populate live windows, tabs, and panes |
| `jarvis_xy_execute` | `XY_DECL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, ...)` | Xylem hook for environment adapters (`dwm`, `sway`, `hyprland`, `site`) to run DSL ops |
| `jarvis_xy_confirm` | `XY_DECL(int, jarvis_xy_confirm, const char *, description)` | Xylem confirmation gate triggered before destructive shell commands |

---

## 4. Low-Latency Voice Pipeline & Window Manager Modules

- **Adaptive VAD & Wake-Word Pipeline (`jarvis-voice.c`)**:
  - 16 kHz `float32` circular ring buffer with pre-speech lookback (`jarvis_vad_get_samples`), adaptive noise-floor tracking (`jarvis_vad_push_frame`), wake-word command extraction (`jarvis_voice_extract_wake_command`), lockdown phrase detection, and zero-dependency WAV earcon chime synthesis (`jarvis_voice_write_earcon_wav`).
  - Incremental streaming TTS answer extractor (`jarvis_extract_streaming_answer_prefix`) and prosodic clause splitter (`jarvis_pop_ready_tts_clause`) delivering ~1.0s Time-To-First-Audio while keeping the microphone active for live barge-in.
- **Pluggable Window Manager Modules (`mods/sway/mod_sway.so`, `mods/hyprland/mod_hyprland.so`)**:
  - Built-in X11/`dwm` adapter plus dynamically loadable Wayland `libxylem` plugins for **Sway** (`swaymsg` IPC) and **Hyprland** (`hyprctl` IPC), supporting workspace switching, window movement, fullscreen/floating layout toggles, directional focus, and live client tree observation.

---

## 5. English & Portuguese Language Support (`en` / `pt`)

`libjarvis` and its companion voice pipeline (`jarvisd`, `dwm-whisperd`, `mod_tts.so`) provide first-class, end-to-end support for both **English (`en`)** and **Portuguese (`pt`)**: speech recognition, wake-word activation, natural language understanding, Action DSL synthesis, and high-fidelity neural text-to-speech.

### 5.1 Bilingual Spoken Wake-Words & Hands-Free Interaction

Jarvis listens continuously via low-power adaptive Voice Activity Detection (VAD):
- **English Spoken Wake-Word**: **`Jarvis`**
  - *"Jarvis, what time is it?"*
  - *"Jarvis, switch to workspace two and open a terminal there."*
  - *"Jarvis, explain quantum physics in simple terms."*
- **Portuguese Spoken Wake-Word**: **`Jarbas`** (or **`Jarvis`**)
  - *"Jarbas, que horas são?"*
  - *"Jarbas, muda para o workspace dois e abre um terminal aí."*
  - *"Jarbas, explica-me física quântica de forma simples."*

Configure the wake word in `~/.jarvisrc`:
```ini
wake_word = Jarbas       # Options: Jarbas, Jarvis (case-insensitive)
```

### 5.2 Speech Recognition: Keyboard Sync, Explicit Language, and Whisper Models

Configure the speech-to-text language in `~/.config/dwm-voice/lang` or `~/.jarvisrc`:
- **`kbd` (Default — Dynamic Keyboard Sync)**: Automatically follows your active X11/Wayland keyboard layout (`Mod+Space` switches between `US — English` and `PT — Português`), dynamically steering Whisper recognition without restarting daemons.
- **`en` (English Only)**:
  ```bash
  mkdir -p ~/.config/dwm-voice
  printf 'en\n' > ~/.config/dwm-voice/lang
  ```
- **`pt` (Portuguese Only — Português)**:
  ```bash
  mkdir -p ~/.config/dwm-voice
  printf 'pt\n' > ~/.config/dwm-voice/lang
  ```
- **`auto` (Multilingual Auto-Detect)**:
  ```bash
  printf 'auto\n' > ~/.config/dwm-voice/lang
  ```

#### Recommended Whisper GGML Models (`~/.local/share/whisper/`)
- **English-Only Ultra-Fast**: `ggml-base.en-q8_0.bin` (78 MB) or `ggml-small.en-q5_1.bin` (182 MB).
- **Portuguese & Multilingual**: Always use multilingual models (without `.en` suffix) so Portuguese accents and vocabulary transcribe with 100% accuracy:
  ```bash
  curl -L -o ~/.local/share/whisper/ggml-small-q5_1.bin \
    https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin
  printf '%s/.local/share/whisper/ggml-small-q5_1.bin\n' "$HOME" > ~/.config/dwm-voice/model
  ```

### 5.3 Neural Text-to-Speech (TTS) Voice Engines

`mod_tts.so` supports three tiers of neural voices, switchable on-the-fly via `tts_voice` in `~/.jarvisrc` or `~/.config/dwm-voice/tts_voice`:

| Engine | Voice Identifier | Description & Characteristics |
| :--- | :--- | :--- |
| **Edge-TTS (Streaming Neural)** | `edge:pt-PT-RaquelNeural` (or `raquel`) | **Ultra-realistic Portuguese voice.** Human-grade prosody, natural breathing, and pitch inflections. |
| **Edge-TTS (Streaming Neural)** | `edge:pt-PT-DuarteNeural` (or `duarte`) | Warm European Portuguese male neural speaker. |
| **Edge-TTS (Streaming Neural)** | `edge:en-US-JennyNeural` / `edge:en-US-GuyNeural` | Exceptionally fluid American English neural speakers. |
| **Piper ONNX (Offline Neural)** | `piper:pt_PT-miro` | Offline European Portuguese male voice (~60ms latency, ~15MB RAM). |
| **Piper ONNX (Offline Neural)** | `piper:pt_PT-tugao-medium` | Distinctive European Portuguese local voice. |
| **Piper ONNX (Offline Neural)** | `piper:gb` / `piper:us` | Fast British English (`northern_english_male`) or US English (`lessac`). |
| **Kokoro-82M ONNX (CPU)** | `pm_tuga`, `pm_alex` | Lightweight local Portuguese CPU voices via `kokoro-pt-pt.onnx`. |
| **Kokoro-82M ONNX (CPU)** | `bm_george`, `bm_daniel`, `am_michael` | Lightweight local English CPU voices via `kokoro-v1.0.int8.onnx`. |

Configure your voice in `~/.jarvisrc`:
```ini
# Ultra-realistic Portuguese neural voice (default recommended):
tts_voice = edge:pt-PT-RaquelNeural

# Or offline Piper Portuguese:
# tts_voice = piper:pt_PT-miro

# Or offline British English:
# tts_voice = piper:gb
```

Test speech immediately from the terminal:
```bash
jarvis speak "Com certeza! O novo sistema de síntese neural de voz está operacional."
jarvis speak "Certainly! High-fidelity neural voice synthesis is now active."
```

### 5.4 Low-Latency Streaming Prosody & Zero-Gap Pipelining (TTFA)

Unlike traditional TTS systems that chop sentences into rigid 38-character micro-fragments (causing robotic pitch drops and buffer underrun pauses between clauses), `libjarvis` uses a two-tiered prosody and streaming architecture:
1. **Low Time-To-First-Audio (TTFA ~300–500 ms)**:
   When synthesis starts, `libjarvis` identifies early punctuation boundaries (`!`, `?`, `.`, `,`) in the opening 3–54 characters. Short natural greetings (*"Olá!"*, *"Com certeza!"*, *"Sim, claro."*, *"Certainly!"*) are popped and dispatched to playback immediately.
2. **Pipelined Background Prefetching (0 ms Inter-Sentence Gap)**:
   While the first clause plays through your speakers (providing a 1.5–3.0 second playback runway), the background worker thread (`tts_worker_fn`) synthesizes the next complete sentence in parallel.
3. **Full-Sentence Prosody Units (up to 240 chars)**:
   Subsequent sentences are preserved whole up to 240 characters. Modern neural models retain complete sentence context, producing natural declarative cadence, rising interrogatives, and authentic human breathing.

### 5.5 Hardware Audio Capture & Echo-Prevention Discipline

When running hands-free voice agents on laptops where the speakers are located inches from the built-in microphone, avoid naive WebRTC software acoustic echo cancellation (AEC) filters that attenuate vocal frequencies and crush VAD signal-to-noise ratios.

Recommended `~/.jarvisrc` audio settings:
```ini
# 1. Point pw_target directly to your physical analog microphone:
pw_target = alsa_input.pci-0000_00_1f.3.analog-stereo

# 2. Disable conversational follow-up window to prevent speaker bleed loops:
followup_ms = 0
conv_followup_ms = 0
```
- **Crisp Speech Capture**: Bypassing aggressive AEC filters maintains full microphone dynamics (+20 dB gain compared to WebRTC filtering), allowing VAD and Whisper to decode speech cleanly at natural speaking volume.
- **Zero Self-Echo Loops**: With `followup_ms = 0`, Jarvis returns directly to standby mode as soon as speech finishes, preventing laptop speaker audio from triggering self-echo loops. It only responds when explicitly addressed by wake-word (*"Jarbas"* / *"Jarvis"*).

### 5.6 Bilingual Custom Ontology & Action DSL Examples

`libjarvis` features a bilingual compositional grammar (`data/ontology.json`). Both Portuguese and English phrases map directly into canonical platform-neutral Action DSL:

| Spoken Utterance (English) | Spoken Utterance (Portuguese) | Compiled Action DSL |
| :--- | :--- | :--- |
| `"switch to workspace 2"` | `"muda para o workspace 2"` | `{"op":"workspace_switch","target":2}` |
| `"open a terminal"` | `"abre um terminal"` | `{"op":"app_launch","target":"terminal"}` |
| `"what time is it"` | `"que horas são"` | `{"op":"get_time"}` |
| `"let's listen to miles davis"` | `"vamos ouvir miles davis"` | `{"op":"media_play","query":"miles davis"}` |
| `"scroll down 5"` | `"desce 5"` | `{"op":"input_scroll","direction":"down","amount":5}` |
| `"fullscreen this window"` | `"maximiza esta janela"` | `{"op":"window_fullscreen"}` |
| `"close this tab"` | `"fecha este separador"` | `{"op":"browser_tab_close"}` |

---

## 6. Installation & Build

### Prebuilt Packages
Prebuilt packages for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD are distributed via [tty.pt packages](https://github.com/tty-pt/ci/blob/main/docs/install.md). Use **libjarvis** as the package name.

### Build from Source
```sh
make                  # builds lib/libjarvis.so, bin/jarvis, mod_sway.so, and mod_hyprland.so
make test             # runs all 9 C parity, solver, ReAct, accessibility, and multi-turn suites
sudo make install     # installs headers, shared libraries, CLI binary, and pkg-config file
```

---

## 7. JavaScript & WebAssembly Bindings (`@tty-pt/jarvis`)

Official Node.js (N-API) and Browser (WebAssembly) bindings are colocated in [`npm/`](npm/README.md):
- **Universal Runtime**: Native Node-API addon (`jarvis_node.node`) with automatic fallback to the freestanding WebAssembly reactor (`dist/jarvis.wasm`) mounted on the `@tty-pt/stele` shared-memory substrate.
- **Full Solver & Engine Parity**: Exposes `solveClause`, `splitClauses`, `normalizeDslJson`, `extractStreamingAnswer`, `popTtsClause`, `isDestructiveCommand`, `extractWakeCommand`, `writeEarconWav`, and the stateful `JarvisEngine` class identically across C, Node.js, and browser WebAssembly.
- **C Development Headers**: Install via `npm install @tty-pt/jarvis` and consume canonical headers from `node_modules/@tty-pt/jarvis/include`.

```sh
make wasm             # compiles npm/dist/jarvis.wasm via clang --target=wasm32
make npm-test         # builds N-API + WASM targets and runs the Node.js parity test suite
```

👉 *For complete TypeScript/JavaScript usage and WebAssembly integration, see [npm/README.md](npm/README.md).*

## License

BSD 2-Clause License. Copyright (c) 2026, Paulo André Azevedo Quirino. See `LICENSE`.
