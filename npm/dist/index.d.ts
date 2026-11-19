export interface JarvisDslTarget {
  kind?: string;
  relation?: string;
  identifier?: string;
  title_contains?: string;
  class_contains?: string;
  url?: string;
  command?: string;
  query?: string;
  x?: number;
  y?: number;
  new_terminal?: boolean;
}

export interface JarvisDslArgs {
  text?: string;
  key_combo?: string;
  button?: number;
  direction?: string;
  amount?: number;
  workspace?: number;
  action?: string;
  mode?: string;
  seconds?: number;
  answer?: string;
  aspect?: string;
  key?: string;
  value?: string;
  reply?: string;
  speak?: boolean;
  background?: boolean;
  streamed_tts?: boolean;
}

export interface JarvisDsl {
  op: string;
  target: JarvisDslTarget;
  args: JarvisDslArgs;
  verify?: {
    audio_playing?: boolean;
    window_focused?: boolean;
    title_contains?: string;
  };
  meta?: {
    source_ir_cap?: string;
    source_ir_score?: number;
    inference_backend?: string;
    latency_ms?: number;
  };
}

export interface JarvisEngineConfig {
  wm?: string;
  ontologyPath?: string;
  fewShotPath?: string;
  contextPath?: string;
  memoryPath?: string;
  sessionPath?: string;
  embedCachePath?: string;
  aurenUrl?: string;
  llmModel?: string;
  inferMode?: "auto" | "llm" | "embedding" | "none";
  dryRun?: boolean;
}

export interface JarvisProcessResult {
  ok: boolean;
  utterance: string;
  error?: string;
  source: string;
  dsl: JarvisDsl;
  result: {
    handled: boolean;
    success: boolean;
    output: string;
    error: string;
  };
  steps: Array<{
    clause: string;
    resolved_clause: string;
    source: string;
    dsl?: JarvisDsl;
    result: {
      handled: boolean;
      success: boolean;
      output: string;
      error: string;
    };
  }>;
}

export declare function getJarvisWasm(): WebAssembly.Instance;
export declare function solveClause(utterance: string, surface?: string, options?: { wasm?: boolean }): JarvisDsl | null;
export declare function normalizeDslJson(rawJson: string | Record<string, unknown>, options?: { wasm?: boolean }): JarvisDsl | null;
export declare function splitClauses(utterance: string, options?: { wasm?: boolean }): string[];
export declare function extractStreamingAnswer(rawJsonFragment: string, options?: { wasm?: boolean }): string;
export declare function popTtsClause(unspoken: string, isFirstClause?: boolean, options?: { wasm?: boolean }): {
  ready: boolean;
  clause: string;
  remainder: string;
};
export declare function isDestructiveCommand(cmd: string, options?: { wasm?: boolean }): boolean;
export declare function isHallucination(text: string, options?: { wasm?: boolean }): boolean;
export declare function extractWakeCommand(transcript: string, wakeWord?: string, options?: { wasm?: boolean }): {
  detected: boolean;
  command: string;
};
export declare function writeEarconWav(wavPath: string, isDone?: boolean): boolean;

export declare class JarvisEngine {
  constructor(config?: JarvisEngineConfig);
  process(utterance: string, surface?: string): JarvisProcessResult;
  remember(key: string, value: string): boolean;
  recall(query?: string): Record<string, string>;
  close(): void;
}
