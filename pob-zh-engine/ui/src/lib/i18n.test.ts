import { describe, expect, it } from "vitest";
import { loadLocale, locale, t } from "./i18n";

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
});
