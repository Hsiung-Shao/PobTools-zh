import { afterEach, describe, expect, it } from "vitest";
import { setEnglish, t } from "./i18n";
import { dataKey, lang } from "./lang.svelte";

describe("lang 版號", () => {
  afterEach(() => setEnglish(false));

  it("L1 切英文後 t() 回英文", () => {
    setEnglish(true);
    expect(t("items.paste")).toBe("Add an item");
  });
  it("L2 切換時 lang.v 恰好加一", () => {
    const before = lang.v;
    setEnglish(true);
    expect(lang.v).toBe(before + 1);
  });
  it("L3 同值重設也每次加一", () => {
    const before = lang.v;
    setEnglish(false);
    expect(lang.v).toBe(before + 1);
    setEnglish(false);
    expect(lang.v).toBe(before + 2);
  });
});

describe("dataKey", () => {
  it("D1 語言版號變了 key 就不同", () => {
    expect(dataKey(5, 0)).not.toBe(dataKey(5, 1));
  });
  it("D2 POB 版號變了 key 就不同,同輸入結果穩定", () => {
    expect(dataKey(5, 0)).not.toBe(dataKey(6, 0));
    expect(dataKey(5, 0)).toBe(dataKey(5, 0));
  });
});
