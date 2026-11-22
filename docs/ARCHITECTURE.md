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
               ├──[ Direct Intent (Media/Volume/Window) ]──► mod_dwm.so / X11
               │
               └──[ Complex Inquiry / Task ]
                       │
                       ▼ UNIX Domain Socket (/tmp/ren-jarvis-$USER.sock)
               ┌───────────────────────┐
               │    ren (tmux session) │  <--- Interactive Agent
               │  • axil-auren (:4242) │
               │  • mod_mm (libmm.so)  │
               │  • mod_jarvis         │
               └───────────────────────┘
```

---

## 2. Audio Processing Pipeline

1. **Acoustic Echo Cancellation (AEC)**:
   - When speech synthesis plays through speakers, PipeWire routes the audio output to the AEC reference stream.
   - Prevents the assistant from hearing and transcribing its own voice.
2. **Dual-Stage Voice Activity Detection (VAD)**:
   - Energy thresholding for initial wake detection.
   - Deep neural VAD (Silero) for high-confidence speech isolation.
3. **Whisper Transcription**:
   - Audio segments are passed to local `libwhisper` running with CPU/GPU/OpenVINO acceleration.
   - Streaming token emissions allow early intent matching before the utterance finishes.

---

## 3. Intent Resolution & Dispatch Hierarchy

When an utterance is transcribed, `libjarvis` evaluates intents in a strict latency-ordered hierarchy:

1. **Local Ontology Fast Path (< 5ms)**:
   - Exact rule and phrase matching against `ontology.json`.
   - Controls desktop actions: volume up/down, workspace switching, brightness, media pause/play.
   - Dispatched immediately via the window manager module (`mod_dwm.so`, `mod_sway.so`, etc.).
2. **Stored Workflow Invocations (< 15ms)**:
   - Checks if the utterance matches a remembered workflow key in `libmm` or `~/.jarvis/memory.json`.
   - Executes recorded macro sequences directly.
3. **Agentic Dispatch to `ren` via Socket (< 50ms to first token)**:
   - If the intent requires reasoning, coding, codebase inspection, or conversation, `jarvisd` writes the text to `/tmp/ren-jarvis-$USER.sock`.
   - `ren` executes in an attached or detached `tmux` session, queries `libmm` for codebase intelligence and lore, and streams synthesized clauses back to `jarvisd` for spoken TTS output.

---

## 4. Window Manager Decoupling (`mod_<wm>`)

`libjarvis` interacts with desktop environments exclusively through dynamically loaded Xylem modules:
- Module signature: `mod_<wm>.so` (e.g. `mod_dwm.so`).
- Functions: Window tiling, tag selection, client focus, floating layout toggles.
- Zero compile-time coupling: `libjarvis` contains no direct X11 or Wayland API calls.
