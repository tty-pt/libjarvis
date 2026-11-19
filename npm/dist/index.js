import { loadWasmModule, loadNativeAddon, getSubstrate } from "@tty-pt/stele";

const nativeBinding = loadNativeAddon(import.meta.url, "jarvis");
let wasmInstance = null;

export function getJarvisWasm() {
  if (wasmInstance) return wasmInstance;
  wasmInstance = loadWasmModule(new URL("./jarvis.wasm", import.meta.url));
  return wasmInstance;
}

function allocUtf8(wasm, str) {
  const bytes = new TextEncoder().encode(str || "");
  const ptr = wasm.exports.malloc(bytes.length + 1);
  const mem = new Uint8Array(getSubstrate().memory.buffer);
  mem.set(bytes, ptr);
  mem[ptr + bytes.length] = 0;
  return ptr;
}

function readUtf8(ptr, maxLen = 65536) {
  const mem = new Uint8Array(getSubstrate().memory.buffer);
  let end = ptr;
  const limit = Math.min(mem.length, ptr + maxLen);
  while (end < limit && mem[end] !== 0) end++;
  return new TextDecoder().decode(mem.subarray(ptr, end));
}

/**
 * Deterministically solve an utterance into a compiled Action DSL object.
 */
export function solveClause(utterance, surface = "any", options = {}) {
  if (!utterance) return null;
  if (!options.wasm && nativeBinding && nativeBinding.solve) {
    const json = nativeBinding.solve(String(utterance), String(surface || "any"));
    return json ? JSON.parse(json) : null;
  }

  const wasm = getJarvisWasm();
  const uPtr = allocUtf8(wasm, String(utterance));
  const sPtr = allocUtf8(wasm, String(surface || "any"));
  const outSz = 8192;
  const outPtr = wasm.exports.malloc(outSz);
  const n = wasm.exports.jarvis_wasm_solve_json(uPtr, sPtr, outPtr, outSz);
  const res = n > 0 ? JSON.parse(readUtf8(outPtr, n)) : null;
  wasm.exports.free(uPtr);
  wasm.exports.free(sPtr);
  wasm.exports.free(outPtr);
  return res;
}

/**
 * Normalize and heal an arbitrary LLM JSON payload into a canonical Action DSL object.
 */
export function normalizeDslJson(rawJson, options = {}) {
  if (!rawJson) return null;
  const rawStr = typeof rawJson === "string" ? rawJson : JSON.stringify(rawJson);
  if (!options.wasm && nativeBinding && nativeBinding.normalizeDsl) {
    const json = nativeBinding.normalizeDsl(rawStr);
    return json ? JSON.parse(json) : null;
  }

  const wasm = getJarvisWasm();
  const rPtr = allocUtf8(wasm, rawStr);
  const outSz = 8192;
  const outPtr = wasm.exports.malloc(outSz);
  const n = wasm.exports.jarvis_wasm_normalize_dsl_json(rPtr, outPtr, outSz);
  const res = n > 0 ? JSON.parse(readUtf8(outPtr, n)) : null;
  wasm.exports.free(rPtr);
  wasm.exports.free(outPtr);
  return res;
}

/**
 * Split a compound natural language utterance into atomic clauses.
 */
export function splitClauses(utterance, options = {}) {
  if (!utterance) return [];
  if (!options.wasm && nativeBinding && nativeBinding.splitClauses) {
    return nativeBinding.splitClauses(String(utterance));
  }

  const wasm = getJarvisWasm();
  const uPtr = allocUtf8(wasm, String(utterance));
  const outSz = 8192;
  const outPtr = wasm.exports.malloc(outSz);
  const n = wasm.exports.jarvis_wasm_split_clauses_json(uPtr, outPtr, outSz);
  const res = n > 0 ? JSON.parse(readUtf8(outPtr, n)) : [];
  wasm.exports.free(uPtr);
  wasm.exports.free(outPtr);
  return res;
}

/**
 * Extract incremental streaming answer prefix from an incomplete LLM JSON response.
 */
export function extractStreamingAnswer(rawJsonFragment, options = {}) {
  if (!rawJsonFragment) return "";
  if (!options.wasm && nativeBinding && nativeBinding.extractStreamingAnswer) {
    return nativeBinding.extractStreamingAnswer(String(rawJsonFragment));
  }

  const wasm = getJarvisWasm();
  const rPtr = allocUtf8(wasm, String(rawJsonFragment));
  const outSz = 4096;
  const outPtr = wasm.exports.malloc(outSz);
  const n = wasm.exports.jarvis_wasm_extract_streaming_answer(rPtr, outPtr, outSz);
  const res = n > 0 ? readUtf8(outPtr, n) : "";
  wasm.exports.free(rPtr);
  wasm.exports.free(outPtr);
  return res;
}

/**
 * Pop the next ready spoken clause for low-latency streaming TTS.
 */
export function popTtsClause(unspoken, isFirstClause = false, options = {}) {
  if (!unspoken) return { ready: false, clause: "", remainder: "" };
  if (!options.wasm && nativeBinding && nativeBinding.popTtsClause) {
    return nativeBinding.popTtsClause(String(unspoken), Boolean(isFirstClause));
  }

  const wasm = getJarvisWasm();
  const uPtr = allocUtf8(wasm, String(unspoken));
  const cPtr = wasm.exports.malloc(2048);
  const rPtr = wasm.exports.malloc(4096);
  const ready = wasm.exports.jarvis_wasm_pop_tts_clause(uPtr, isFirstClause ? 1 : 0, cPtr, 2048, rPtr, 4096) !== 0;
  const clause = ready ? readUtf8(cPtr, 2048) : "";
  const remainder = ready ? readUtf8(rPtr, 4096) : String(unspoken);
  wasm.exports.free(uPtr);
  wasm.exports.free(cPtr);
  wasm.exports.free(rPtr);
  return { ready, clause, remainder };
}

/**
 * Detect whether a shell command matches destructive patterns.
 */
export function isDestructiveCommand(cmd, options = {}) {
  if (!cmd) return false;
  if (!options.wasm && nativeBinding && nativeBinding.isDestructiveCommand) {
    return nativeBinding.isDestructiveCommand(String(cmd));
  }

  const wasm = getJarvisWasm();
  const cPtr = allocUtf8(wasm, String(cmd));
  const res = wasm.exports.jarvis_wasm_is_destructive(cPtr) !== 0;
  wasm.exports.free(cPtr);
  return res;
}

/**
 * Detect whether an ASR transcript is a non-speech hallucination.
 */
export function isHallucination(text, options = {}) {
  if (!text) return true;
  if (!options.wasm && nativeBinding && nativeBinding.isHallucination) {
    return nativeBinding.isHallucination(String(text));
  }

  const wasm = getJarvisWasm();
  const tPtr = allocUtf8(wasm, String(text));
  const res = wasm.exports.jarvis_wasm_is_hallucination(tPtr) !== 0;
  wasm.exports.free(tPtr);
  return res;
}

/**
 * Extract wake-word and trailing command from a voice transcript.
 */
export function extractWakeCommand(transcript, wakeWord = "jarvis", options = {}) {
  if (!transcript) return { detected: false, command: "" };
  if (!options.wasm && nativeBinding && nativeBinding.extractWakeCommand) {
    return nativeBinding.extractWakeCommand(String(transcript), String(wakeWord || "jarvis"));
  }

  const wasm = getJarvisWasm();
  const tPtr = allocUtf8(wasm, String(transcript));
  const wPtr = allocUtf8(wasm, String(wakeWord || "jarvis"));
  const outPtr = wasm.exports.malloc(2048);
  const detected = wasm.exports.jarvis_wasm_extract_wake_command(tPtr, wPtr, outPtr, 2048) !== 0;
  const command = detected ? readUtf8(outPtr, 2048) : "";
  wasm.exports.free(tPtr);
  wasm.exports.free(wPtr);
  wasm.exports.free(outPtr);
  return { detected, command };
}

/**
 * Synthesize a 16 kHz mono WAV earcon chime file in pure C.
 */
export function writeEarconWav(wavPath, isDone = false) {
  if (!nativeBinding || !nativeBinding.writeEarconWav) {
    throw new Error("@tty-pt/jarvis native addon is required for writeEarconWav");
  }
  return nativeBinding.writeEarconWav(String(wavPath), Boolean(isDone));
}

/**
 * Stateful Jarvis Orchestrator Engine (Solver + Memory + Context + ReAct Agent + Xylem Adapters).
 */
export class JarvisEngine {
  constructor(config = {}) {
    if (!nativeBinding || !nativeBinding.create) {
      throw new Error("@tty-pt/jarvis native addon is required for JarvisEngine");
    }
    this.handle = nativeBinding.create(config);
  }

  process(utterance, surface = "any") {
    if (!this.handle) throw new Error("JarvisEngine is closed");
    const json = nativeBinding.process(this.handle, String(utterance), String(surface || "any"));
    return JSON.parse(json);
  }

  remember(key, value) {
    if (!this.handle) throw new Error("JarvisEngine is closed");
    return nativeBinding.remember(this.handle, String(key), String(value));
  }

  recall(query = "") {
    if (!this.handle) throw new Error("JarvisEngine is closed");
    const json = nativeBinding.recall(this.handle, String(query || ""));
    return JSON.parse(json);
  }

  close() {
    if (this.handle) {
      nativeBinding.destroy(this.handle);
      this.handle = null;
    }
  }
}
