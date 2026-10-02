// Placement of a floating panel (the breakdown pop-ups of the Calcs tab and the
// sidebar) inside the window. The panel is sized by its content first and only
// then placed, so it never has to be squeezed next to the cursor: it goes where
// it fits and keeps fitting when the window is resized.

/**
 * Where the panel may sit on one axis. `after` is where its start edge goes
 * when it is placed after the anchor (right of / below it); `before` is where
 * its end edge goes when it is placed before the anchor (left of / above it).
 */
export interface AxisAnchor {
  after: number;
  before: number;
}

export interface FloatAnchor {
  x: AxisAnchor;
  y: AxisAnchor;
}

/** Gap kept between the panel and the window edges. */
export const FLOAT_MARGIN = 10;

/**
 * One axis: after the anchor when that fits, otherwise before it, otherwise
 * pushed back inside the window. A panel larger than the window starts at the
 * margin (its far side is cut by the CSS max size, see the callers).
 */
export function placeAxis(a: AxisAnchor, size: number, view: number, margin = FLOAT_MARGIN): number {
  const lo = margin;
  const hi = view - margin;
  if (a.after >= lo && a.after + size <= hi) return a.after;
  if (a.before - size >= lo && a.before <= hi) return a.before - size;
  return Math.max(lo, Math.min(a.after, hi - size));
}

export function placeFloat(
  anchor: FloatAnchor,
  size: { w: number; h: number },
  view: { w: number; h: number },
  margin = FLOAT_MARGIN,
): { x: number; y: number } {
  return {
    x: Math.round(placeAxis(anchor.x, size.w, view.w, margin)),
    y: Math.round(placeAxis(anchor.y, size.h, view.h, margin)),
  };
}

/** A point anchor (the mouse), with the panel offset from it by a gap. */
export function pointAnchor(x: number, y: number, gapX = 16, gapY = 14): FloatAnchor {
  return { x: { after: x + gapX, before: x - gapX }, y: { after: y + gapY, before: y - gapY } };
}

/**
 * Svelte action for a `position: fixed` panel: measures it after it has been
 * laid out and writes `left` / `top`, then does it again whenever the anchor
 * changes, the window is resized, or the panel's own size changes (new
 * content, fonts arriving, wrapping that changed with the window width).
 * The panel's CSS caps it at the window size minus the margin; if the content
 * is still taller than that, `data-fit-tall` is set so the caller can let the
 * body scroll as a last resort.
 */
export function fitFloat(node: HTMLElement, anchor: FloatAnchor) {
  let current = anchor;
  const place = () => {
    const r = node.getBoundingClientRect();
    const view = { w: window.innerWidth, h: window.innerHeight };
    const p = placeFloat(current, { w: r.width, h: r.height }, view);
    node.style.left = `${p.x}px`;
    node.style.top = `${p.y}px`;
    node.toggleAttribute("data-fit-tall", r.height >= view.h - 2 * FLOAT_MARGIN - 1);
  };
  place();
  const ro = new ResizeObserver(place);
  ro.observe(node);
  window.addEventListener("resize", place);
  return {
    update(next: FloatAnchor) {
      current = next;
      place();
    },
    destroy() {
      ro.disconnect();
      window.removeEventListener("resize", place);
    },
  };
}
