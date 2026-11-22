export declare function loadOntologyJson(jsonStr: string): boolean;
export declare function getJarvisWasm(): WebAssembly.Instance;
export declare function cleanPhrase(text: string, options?: { wasm?: boolean }): string;
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
