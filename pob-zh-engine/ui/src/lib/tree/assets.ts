// Tree art: the sprite sheets and single PNGs under the POB install, indexed
// by the names POB uses (bridge `tree_assets`, which reads sprites.lua's own
// coordinates). Files come straight from the live install through the
// host's pob.pobtools virtual host, so a POB update that ships new art is
// picked up on the next load with no build step.

import { api, hostInfo } from "$lib/bridge";

export interface SpriteRect {
  file: string;
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface SpriteManifest {
  version: string;
  assets: Record<string, SpriteRect>;
  /** Grey ("Inactive") variants of the icon sheets. */
  disabled: Record<string, SpriteRect>;
  sheets: Record<string, { w: number; h: number }>;
  missingSheets: string[];
  /** Standalone images PassiveTreeView loads by path (jewel radius rings), by its field name. */
  images?: Record<string, string>;
}

/** POB draws every tree sprite at its sheet size × 1.33 tree units. */
export const ART_SCALE = 1.33;

export class Sprites {
  private images = new Map<string, { img: HTMLImageElement; ready: boolean }>();

  constructor(
    readonly manifest: SpriteManifest,
    private repaint: () => void,
  ) {}

  static async load(version: string, repaint: () => void): Promise<Sprites> {
    const m = await api.treeAssets(version);
    return new Sprites(m, repaint);
  }

  static url(file: string): string {
    return `https://${hostInfo.hosts.pob}/${file.split("/").map(encodeURIComponent).join("/")}`;
  }

  has(name: string): boolean {
    return name in this.manifest.assets;
  }

  rect(name: string, grey = false): SpriteRect | undefined {
    if (grey) return this.manifest.disabled[name] ?? this.manifest.assets[name];
    return this.manifest.assets[name];
  }

  private image(file: string): HTMLImageElement | null {
    let slot = this.images.get(file);
    if (!slot) {
      const img = new Image();
      slot = { img, ready: false };
      this.images.set(file, slot);
      img.onload = () => {
        slot!.ready = true;
        this.repaint();
      };
      img.src = Sprites.url(file);
    }
    return slot.ready ? slot.img : null;
  }

  warm(...names: string[]) {
    for (const n of names) {
      const r = this.rect(n);
      if (r) this.image(r.file);
    }
  }

  /** Draws `name` centred at (x, y) with half extents hw/hh in screen pixels. */
  draw(ctx: CanvasRenderingContext2D, name: string, x: number, y: number, hw: number, hh: number, grey = false): boolean {
    const r = this.rect(name, grey);
    if (!r) return false;
    const img = this.image(r.file);
    if (!img) return false;
    ctx.drawImage(img, r.x, r.y, r.w, r.h, x - hw, y - hh, hw * 2, hh * 2);
    return true;
  }

  /** Draws `name` at POB's DrawAsset size: sheet pixels × 1.33 × zoom. */
  drawArt(ctx: CanvasRenderingContext2D, name: string, x: number, y: number, zoom: number, mirrored = false): boolean {
    const r = this.rect(name);
    if (!r) return false;
    const hw = r.w * ART_SCALE * zoom;
    const hh = r.h * ART_SCALE * zoom;
    if (!mirrored) return this.draw(ctx, name, x, y, hw, hh);
    // Half image: the top half as-is, the bottom half flipped (PoB's isHalf).
    const ok = this.draw(ctx, name, x, y - hh, hw, hh);
    ctx.save();
    ctx.translate(x, y + hh);
    ctx.scale(1, -1);
    this.draw(ctx, name, 0, 0, hw, hh);
    ctx.restore();
    return ok;
  }

  /**
   * Draws one of the standalone images (manifest.images) centred at (x, y)
   * as a `half`×2 square, rotated by `angle` radians — PassiveTreeView's
   * DrawImageRotated for the jewel radius rings.
   */
  drawImage(ctx: CanvasRenderingContext2D, key: string, x: number, y: number, half: number, angle = 0, alpha = 1): boolean {
    const file = this.manifest.images?.[key];
    if (!file) return false;
    const img = this.image(file);
    if (!img) return false;
    ctx.save();
    ctx.translate(x, y);
    if (angle) ctx.rotate(angle);
    ctx.globalAlpha = alpha;
    ctx.drawImage(img, -half, -half, half * 2, half * 2);
    ctx.restore();
    return true;
  }

  /**
   * Covers w×h with `name` repeated every `step` pixels, one drawImage per
   * tile. Not a CanvasPattern: filling with one during a window resize takes
   * WebView2's renderer down after a hundred or so steps.
   */
  cover(ctx: CanvasRenderingContext2D, name: string, w: number, h: number, step: number): boolean {
    const r = this.rect(name);
    if (!r) return false;
    const img = this.image(r.file);
    if (!img) return false;
    for (let y = 0; y < h; y += step) for (let x = 0; x < w; x += step) ctx.drawImage(img, r.x, r.y, r.w, r.h, x, y, step, step);
    return true;
  }
}
