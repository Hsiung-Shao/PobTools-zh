import { describe, expect, it } from "vitest";
import { parsePobText, stripPobText } from "./pobtext";

describe("parsePobText", () => {
  it("splits palette escapes into coloured spans", () => {
    const spans = parsePobText("^7生命: ^xFF7070123");
    expect(spans).toEqual([
      { text: "生命: ", color: "var(--fg-0)" },
      { text: "123", color: "var(--c-life)" },
    ]);
  });
  it("keeps a bare caret that is not an escape", () => {
    expect(parsePobText("a ^ b")).toEqual([{ text: "a ^ b", color: null }]);
  });
  it("maps unknown hex through color-mix so light themes can deepen it", () => {
    const [s] = parsePobText("^x123456x");
    expect(s.color).toContain("#123456");
  });
  it("strips every escape", () => {
    expect(stripPobText("^8Player:^7 ^x33FF77ok")).toBe("Player: ok");
  });
});
