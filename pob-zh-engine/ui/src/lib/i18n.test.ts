import { describe, expect, it } from "vitest";
import { isEnglish, loadLocale, locale, setEnglish, t } from "./i18n";

describe("i18n", () => {
  it("substitutes {vars} and falls back to the key itself", () => {
    expect(t("import.items", { n: 3 })).toBe("3 items");
    expect(t("no.such.key")).toBe("no.such.key");
  });
  it("leaves unknown placeholders alone", () => {
    expect(t("tree.pointsN", {})).toBe("{n} point(s)");
  });
  it("outside the host the locale is the dev default and loadLocale resolves without fetching", async () => {
    expect(locale()).toBe("zh-rTW");
    await expect(loadLocale()).resolves.toBeUndefined();
    expect(t("title.tree")).toBe("Tree"); // English built-in stays when no ui.json was loaded
  });
  // F2's half of the switch on this side: the engine turns POB's own text back
  // to English, setEnglish turns ours. Outside the host there is no locale file
  // to swap to, so the observable part is the flag and that t() keeps working.
  it("setEnglish flips the flag and keeps every key answerable", () => {
    expect(isEnglish()).toBe(false);
    setEnglish(true);
    expect(isEnglish()).toBe(true);
    expect(t("title.tree")).toBe("Tree");
    expect(t("import.items", { n: 2 })).toBe("2 items");
    setEnglish(false);
    expect(isEnglish()).toBe(false);
    expect(t("title.tree")).toBe("Tree");
  });
});
