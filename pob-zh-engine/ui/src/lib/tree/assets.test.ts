import { describe, expect, it } from "vitest";
import { ART_SCALE, Sprites, type SpriteManifest } from "./assets";

const manifest: SpriteManifest = {
  version: "3_29",
  assets: { PSSkillFrame: { file: "TreeData/3_29/frame-3.png", x: 0, y: 0, w: 50, h: 50 }, icon: { file: "TreeData/3_29/skills-3.jpg", x: 10, y: 20, w: 30, h: 30 } },
  disabled: { icon: { file: "TreeData/3_29/skills-disabled-3.jpg", x: 10, y: 20, w: 30, h: 30 } },
  sheets: { "TreeData/3_29/frame-3.png": { w: 512, h: 512 } },
  missingSheets: [],
};

describe("Sprites", () => {
  const s = new Sprites(manifest, () => {});
  it("looks up rects by POB's asset names, grey variants from the disabled sheet", () => {
    expect(s.has("PSSkillFrame")).toBe(true);
    expect(s.has("nope")).toBe(false);
    expect(s.rect("icon")!.file).toContain("skills-3.jpg");
    expect(s.rect("icon", true)!.file).toContain("disabled");
    expect(s.rect("PSSkillFrame", true)!.file).toContain("frame-3.png"); // no grey variant -> normal one
  });
  it("builds the virtual-host URL with each path segment encoded", () => {
    expect(Sprites.url("TreeData/3_29/a b.png")).toBe("https://pob.pobtools/TreeData/3_29/a%20b.png");
  });
  it("uses POB's 1.33 art scale", () => expect(ART_SCALE).toBeCloseTo(1.33));
});

describe("hostUrl", () => {
  it("names a WebView2 virtual host, or takes the browser fallback's full URL as is", async () => {
    const { hostUrl } = await import("$lib/bridge");
    const base = { game: "poe2", locale: "zh-rTW", exeDir: "", pobDir: "", version: "t" };
    expect(hostUrl("pob", "TreeData/a.png", { ...base, hosts: { app: "app.pobtools", pob: "pob.pobtools", data: "d", fonts: "f" } })).toBe("https://pob.pobtools/TreeData/a.png");
    expect(hostUrl("cache", "tree/x-1.png", { ...base, hosts: { app: "a", pob: "p", data: "d", fonts: "f", cache: "http://127.0.0.1:5000/t/abc/~cache" } })).toBe("http://127.0.0.1:5000/t/abc/~cache/tree/x-1.png");
  });
});
