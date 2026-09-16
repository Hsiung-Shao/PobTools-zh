import { describe, expect, it } from "vitest";
import { pobPlain, pobRuns } from "./pobtext";

describe("pobRuns", () => {
  it("splits palette and literal colour codes into runs", () => {
    expect(pobRuns("^7生命: ^xFF7070123")).toEqual([
      { text: "生命: ", color: "var(--fg-0)" },
      { text: "123", color: "var(--c-life)" },
    ]);
  });
  it("leaves a caret that is not a code alone", () => {
    expect(pobRuns("a ^ b ^z")).toEqual([{ text: "a ^ b ^z", color: null }]);
  });
  it("passes an unknown literal colour through as hex", () => {
    expect(pobRuns("^x123456x")[0].color).toBe("#123456");
  });
  it("keeps a run empty-free when codes are adjacent", () => {
    expect(pobRuns("^7^x88FFFFES")).toEqual([{ text: "ES", color: "var(--c-es)" }]);
  });
  it("strips every code", () => {
    expect(pobPlain("^8Player:^7 ^x33FF77ok")).toBe("Player: ok");
  });
});
