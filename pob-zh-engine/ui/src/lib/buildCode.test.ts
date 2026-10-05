import { describe, expect, it } from "vitest";
import { classifyClipboard, looksLikeBuildUrl, looksLikeShareCode, MIN_SHARE_CODE_LENGTH, shouldAutoFill } from "./buildCode";

// a real-shaped POB code: base64url, starts "eN" (zlib), well over 100 chars
const CODE = "eNrtXV1z2zYWfe-v4OhhOm0tEf_9eXSW-_2_-" + "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789".repeat(4);

describe("looksLikeShareCode", () => {
  it("takes base64url text of share-code length, trimmed", () => {
    expect(CODE.length).toBeGreaterThanOrEqual(MIN_SHARE_CODE_LENGTH);
    expect(looksLikeShareCode(CODE)).toBe(true);
    expect(looksLikeShareCode(`  ${CODE}\r\n`)).toBe(true);
  });
  it("takes classic base64 (+ / =) too", () => {
    expect(looksLikeShareCode("A+/=".repeat(30))).toBe(true);
  });
  it("refuses short text, whitespace inside, and other characters", () => {
    expect(looksLikeShareCode(CODE.slice(0, MIN_SHARE_CODE_LENGTH - 1))).toBe(false);
    expect(looksLikeShareCode(CODE.slice(0, 60) + " " + CODE.slice(60))).toBe(false);
    expect(looksLikeShareCode(CODE + "!")).toBe(false);
    expect(looksLikeShareCode("火球 ".repeat(60))).toBe(false);
    expect(looksLikeShareCode("")).toBe(false);
  });
  it("refuses a pasted item or a URL", () => {
    expect(looksLikeShareCode("Item Class: Helmets\nRarity: Rare\n" + "x".repeat(120))).toBe(false);
    expect(looksLikeShareCode("https://pobb.in/" + "a".repeat(120))).toBe(false);
  });
});

describe("looksLikeBuildUrl", () => {
  it("knows POB's import sites", () => {
    for (const u of [
      "https://pobb.in/abc123XYZ",
      "https://pob.codes/b/abcdef",
      "https://maxroll.gg/poe/pob/abc123",
      "https://maxroll.gg/poe2/pob/abc123",
      "https://poe.ninja/pob/abc123",
      "https://poe.ninja/poe1/pob/abc123",
      "https://poe.ninja/poe2/pob/abc123",
      "https://pastebin.com/AbCd1234",
      "https://pastebinp.com/AbCd1234",
      "https://rentry.co/abcd",
      "https://poedb.tw/pob/abcd",
      "https://poe2db.tw/pob/abcd",
      "  https://pobb.in/abc123  ",
    ]) {
      expect(looksLikeBuildUrl(u), u).toBe(true);
    }
  });
  it("refuses other links and plain http", () => {
    for (const u of ["https://www.pathofexile.com/trade", "https://pobb.in", "http://pobb.in/abc", "https://example.com/pob/abc", "pobb.in/abc", "https://pobb.in/a b"]) {
      expect(looksLikeBuildUrl(u), u).toBe(false);
    }
  });
});

describe("classifyClipboard / shouldAutoFill", () => {
  it("names the field the text belongs in", () => {
    expect(classifyClipboard(` ${CODE} `)).toEqual({ kind: "code", text: CODE });
    expect(classifyClipboard("https://pobb.in/abc")).toEqual({ kind: "url", text: "https://pobb.in/abc" });
    expect(classifyClipboard("hello")).toBeNull();
    expect(classifyClipboard("")).toBeNull();
    expect(classifyClipboard(null)).toBeNull();
  });
  it("fills only an empty field, and not twice with the same text", () => {
    const found = classifyClipboard(CODE);
    expect(shouldAutoFill(found, "", "")).toBe(true);
    expect(shouldAutoFill(found, "  ", "")).toBe(true);
    expect(shouldAutoFill(found, "something typed", "")).toBe(false);
    expect(shouldAutoFill(found, "", CODE)).toBe(false);
    expect(shouldAutoFill(null, "", "")).toBe(false);
  });
});
