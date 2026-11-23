# libjarvis Architecture Specification

`libjarvis` is a real-time, low-latency, pure-C voice assistant engine and desktop orchestrator. It couples streaming PipeWire microphone capture, local speech recognition (Whisper), speech synthesis (TTS), local ontology intent classification, and asynchronous LLM agent dispatch (`ren`).

---

## 1. System Topology & Process Boundaries

```
[ Microphone (PipeWire / ALSA) ]
               │
               ▼
   ┌───────────────────────┐
   │        jarvisd        │  <--- Audio Daemon
   │  • PipeWire Capture   │
   │  • WebRTC / AEC Filter│
   │  • Energy + Silero VAD│
   │  • Whisper STT Worker │
   └───────────┬───────────┘
               │
               ▼ Utterance Text
   ┌───────────────────────┐
   │     libjarvis API     │
   │  • Ontology Matching  │
   │  • Direct UI Intents  │
   └───────────┬───────────┘
               │
               ├──[ Direct Reflex (<1ms) ]──► ren-reflex.so ─► ren-dwm.so / ren-x11.so / ren-sway.so
               │
               └──[ Complex Inquiry / Task ]
                       │
                       ▼ UNIX Domain Socket (/tmp/ren-jarvis-$USER.sock)
               ┌───────────────────────┐
               │    ren (tmux session) │  <--- Interactive Agent
               │  • axil-auren (:4242) │
               │  • ren-mm.so (libmm)  │
               │  • ren-jarvis.so      │
               └───────────────────────┘
```

---

## 2. Audio Processing Pipeline

1. **Acoustic Echo Cancellation (AEC)**:
   - When speech synthesis plays through speakers, PipeWire routes the audio output to the AEC reference stream (`jarvis_aec_source` via `data/99-jarvis-aec.conf`, selected automatically when `pw_target = auto`).
   - Prevents the assistant from hearing and transcribing its own voice.
2. **Adaptive VAD & Speculative Whisper**:
   - Adaptive noise-floor energy + spectral/ZCR VAD (`jarvis-voice.c`) triggers speculative `libwhisper` decoding on speech frames before `cmd_silence_ms` (default `750ms`, early completion at `>= 500ms` when speculative hypothesis stabilizes).
3. **Whisper Transcription**:
   - Audio segments are passed to local `libwhisper` running with Vulkan/CPU acceleration and dynamic `audio_ctx` sizing proportional to captured utterance duration.

---

## 3. Intent Resolution & Dispatch Hierarchy

When an utterance is transcribed, `jarvisd` streams it to `ren` (`/tmp/ren-jarvis-$USER.sock`), which evaluates intents in a strict latency-ordered hierarchy:

1. **Tier-0 Deterministic Reflex (`ren-reflex.so`, < 1ms)**:
   - Pure-C normalized EN/PT pattern matching (with UTF-8 Latin diacritic folding, polite filler stripping, and optional `~/.config/ren/reflex.tsv` overrides).
   - Dispatches directly via `ren_call_tool()` to loaded desktop modules (`ren-dwm.so`, `ren-x11.so`, `ren-sway.so`, `ren-hyprland.so`, `ren-firefox.so`, `ren-desktop.so`, `ren-jarvis.so`).
2. **Tier-1 Agentic Dispatch (`libren` + `axil-auren`, < 50ms TTFT)**:
   - Questions, multi-clause utterances, or unmatched requests fail open to the Tier-1 LLM (`fast` / `smart` / local GGUF), which queries `ren-mm.so` (`libmm`) for memory/codebase context and executes tools iteratively while streaming clauses back to `jarvisd` for TTS synthesis (`edge-tts-pcm --serve` + LRU PCM cache, or offline Piper/Kokoro ONNX).

---

## 4. Window Manager Decoupling (`ren-<wm>.so`)

`ren` and `jarvisd` interact with desktop environments exclusively through dynamically loaded `libxylem` modules:
- Module naming: `ren-<wm>.so` (e.g. `ren-dwm.so`, `ren-sway.so`, `ren-hyprland.so`, `ren-x11.so`), loaded from `$PREFIX/lib/ren/` based on `wm = auto` detection.
- Capabilities: Workspace/tag switching, window focus/close/fullscreen/floating, app launching, media control, browser tabs/accessibility (`ren-firefox.so`), and desktop state observation (`ren-desktop.so`).
- Zero compile-time coupling: `jarvisd` and core `libjarvis` contain no direct X11, Wayland, or `dwm` IPC code.

---

## 5. Configuration & Environment Reference

All runtime settings default to zero-config `auto` behavior; `~/.jarvisrc` (or `$JARVISRC` / `~/.config/jarvis/jarvisrc`) is entirely optional and typically needs at most a single `lang` line when your OS `LANG` differs:

| Key / Env | Default | Purpose |
| :--- | :--- | :--- |
| `lang` / `JARVIS_LANG` | `auto` (`LC_*`/`LANG` -> `en`) | Sets Whisper STT language, `ontology.json` wake-word/UI strings, and TTS voice |
| `tts_voice` / `JARVIS_VOICE` | `auto` (`local` for offline) | Voice override (`edge:*`, `piper:*`, Kokoro ID, or `local` for offline Piper/Kokoro) |
| `pw_target` | `auto` (`jarvis_aec_source`) | PipeWire capture target; auto-selects AEC node when present |
| `wm` | `auto` | Window manager backend (`dwm`, `sway`, `hyprland`, `x11`) |
| `whisper_model` | `auto` | Path or auto-discovered GGML model matching selected language |
| `cmd_silence_ms` | `750` | Post-speech endpointing silence window (`steer_silence_ms` defaults to `620`) |
| `steer_require_wake` | `0` | Require wake word during active steering (`0` = direct follow-up) |

