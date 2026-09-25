import { describe, expect, it } from "vitest";
import { TextHistory, type TextSnap } from "./textHistory";

const s = (text: string, at = text.length): TextSnap => ({ text, start: at, end: at });

describe("TextHistory", () => {
  it("typing close together is one step; a pause starts the next", () => {
    const h = new TextHistory(200, 1000);
    h.record(s(""), 0);
    h.record(s("a"), 100);
    h.record(s("ab"), 200);
    h.record(s("abc"), 5000);
    expect(h.undo(s("abcd"))?.text).toBe("abc");
    expect(h.undo(s("abc"))?.text).toBe("");
    expect(h.undo(s(""))).toBeNull();
  });
  it("redo walks forward again and a new change drops it", () => {
    const h = new TextHistory(200, 1000);
    h.record(s(""), 0);
    const back = h.undo(s("x"))!;
    expect(back.text).toBe("");
    expect(h.redo(back)?.text).toBe("x");
    h.undo(s("x"));
    h.record(s(""), 9000);
    expect(h.canRedo).toBe(false);
  });
  it("a forced step (colour button) never joins the typing around it", () => {
    const h = new TextHistory(200, 1000);
    h.record(s(""), 0);
    h.record(s("ab"), 100, true);
    h.record(s("^xFF0000ab"), 150);
    expect(h.undo(s("^xFF0000abc"))?.text).toBe("^xFF0000ab");
    expect(h.undo(s("^xFF0000ab"))?.text).toBe("ab");
    expect(h.undo(s("ab"))?.text).toBe("");
  });
  it("keeps the selection it was given and caps its length", () => {
    const h = new TextHistory(3, 0);
    for (let i = 0; i < 5; i++) h.record({ text: String(i), start: i, end: i + 1 }, i * 10);
    expect(h.undo(s("5"))).toEqual({ text: "4", start: 4, end: 5 });
    h.undo(s("4"));
    h.undo(s("3"));
    expect(h.canUndo).toBe(false);
  });
  it("reset forgets everything (another build's notes)", () => {
    const h = new TextHistory();
    h.record(s("a"), 0);
    h.reset();
    expect(h.canUndo).toBe(false);
  });
});
