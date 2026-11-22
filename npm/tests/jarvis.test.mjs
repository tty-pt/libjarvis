import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  cleanPhrase,
  extractStreamingAnswer,
  popTtsClause,
  isDestructiveCommand,
  isHallucination,
  extractWakeCommand,
  writeEarconWav,
} from "../dist/index.js";

test("cleanPhrase folds UTF-8 diacritics and normalizes spacing in Native and WASM", () => {
  const input = "  Olá, Jarbas!   Ação rápida  ";
  assert.equal(cleanPhrase(input, { wasm: false }), "ola jarbas acao rapida");
  assert.equal(cleanPhrase(input, { wasm: true }), "ola jarbas acao rapida");
});

test("extractStreamingAnswer and popTtsClause work across Native and WASM", () => {
  const frag = '```json\n{"op": "answer_question", "args": {"answer": "Water boils at 100 degrees Celsius at standard pressure';
  assert.ok(extractStreamingAnswer(frag, { wasm: false }).startsWith("Water boils at 100 degrees Celsius"));
  assert.ok(extractStreamingAnswer(frag, { wasm: true }).startsWith("Water boils at 100 degrees Celsius"));

  const text = "Linux is a family of open-source Unix-like operating systems based on the Linux kernel.";
  const popNat = popTtsClause(text, true, { wasm: false });
  const popWsm = popTtsClause(text, true, { wasm: true });
  assert.equal(popNat.ready, true);
  assert.equal(popWsm.ready, true);
  assert.ok(popNat.clause.length >= 15 && popNat.clause.length <= 54);
  assert.equal(popNat.clause, popWsm.clause);
});

test("safety and voice utilities (isDestructiveCommand, isHallucination, extractWakeCommand, writeEarconWav)", () => {
  assert.equal(isDestructiveCommand("rm -rf /tmp/build", { wasm: false }), true);
  assert.equal(isDestructiveCommand("rm -rf /tmp/build", { wasm: true }), true);
  assert.equal(isDestructiveCommand("ls -la", { wasm: false }), false);
  assert.equal(isDestructiveCommand("ls -la", { wasm: true }), false);

  assert.equal(isHallucination("vvvvvvvv", { wasm: false }), true);
  assert.equal(isHallucination("vvvvvvvv", { wasm: true }), true);
  assert.equal(isHallucination("open a terminal", { wasm: false }), false);

  const wakeNat = extractWakeCommand("Hey Jarvis, switch to workspace 3", "jarvis", { wasm: false });
  const wakeWsm = extractWakeCommand("Hey Jarvis, switch to workspace 3", "jarvis", { wasm: true });
  assert.equal(wakeNat.detected, true);
  assert.equal(wakeNat.command, "switch to workspace 3");
  assert.deepEqual(wakeNat, wakeWsm);

  const tmpWav = path.join(os.tmpdir(), `jarvis_test_${process.pid}.wav`);
  assert.equal(writeEarconWav(tmpWav, false), true);
  const stat = fs.statSync(tmpWav);
  assert.ok(stat.size > 44);
  fs.unlinkSync(tmpWav);
});
