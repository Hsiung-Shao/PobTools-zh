import { describe, expect, it } from "vitest";
import { clampPreview } from "./previewClamp";

describe("clampPreview 預覽高度夾限", () => {
  it("預覽高夾到容器高減 140", () => {
    expect(clampPreview(900, 500)).toBe(360);
  });

  it("容器過矮時不低於最小值", () => {
    expect(clampPreview(300, 200)).toBe(120);
  });
});
