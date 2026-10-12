// Pure logic behind the shared drop-down (components/Select.svelte): where its
// list opens, keyboard movement, first-letter jumps and the search filter.
// Kept free of the DOM so it can be unit tested.

export interface SelOption {
  value: string | number;
  label: string;
  disabled?: boolean;
  title?: string;
  color?: string;
  /** Extra text the search box also matches (e.g. the English name). */
  search?: string;
}

/** Strip POB colour codes (`^xRRGGBB`, `^0`-`^9`) from a label. */
export function plainLabel(s: string): string {
  return s.replace(/\^x[0-9a-fA-F]{6}|\^[0-9]/g, "");
}

/**
 * Which side of the trigger the list opens on and how tall it may be.
 * `below` / `above` are the free space (px) between the trigger and the window
 * edges, `listH` the list's natural height. Below is preferred; above when only
 * that side fits; otherwise the larger side, capped to the space it has.
 */
export function placeList(p: { below: number; above: number; listH: number; margin?: number }): {
  dir: "down" | "up";
  maxH: number;
} {
  const m = p.margin ?? 8;
  const below = Math.max(0, p.below - m);
  const above = Math.max(0, p.above - m);
  if (p.listH <= below) return { dir: "down", maxH: p.listH };
  if (p.listH <= above) return { dir: "up", maxH: p.listH };
  return below >= above ? { dir: "down", maxH: below } : { dir: "up", maxH: above };
}

/**
 * Move the active index by `d` steps, skipping disabled options. Stops at the
 * ends (no wrapping); if there is no enabled option that way, stays put.
 */
export function moveIndex(o: SelOption[], from: number, d: number): number {
  if (!o.length || d === 0) return from;
  const step = d > 0 ? 1 : -1;
  let left = Math.abs(d);
  let at = from;
  let i = from + step;
  while (i >= 0 && i < o.length && left > 0) {
    if (!o[i].disabled) {
      at = i;
      left--;
    }
    i += step;
  }
  return at;
}

/**
 * The next enabled option after `from` (wrapping) whose label starts with `q`,
 * ignoring case and colour codes; `from` itself is checked last. Returns
 * `from` when nothing matches.
 */
export function typeahead(o: SelOption[], from: number, q: string): number {
  const n = o.length;
  const needle = q.toLowerCase();
  if (!n || !needle) return from;
  for (let k = 1; k <= n; k++) {
    const i = (((from + k) % n) + n) % n;
    if (o[i].disabled) continue;
    if (plainLabel(o[i].label).trimStart().toLowerCase().startsWith(needle)) return i;
  }
  return from;
}

/** Options whose label or `search` alias (colour codes removed) contains `q`, ignoring case. */
export function filterOptions(o: SelOption[], q: string): SelOption[] {
  const needle = q.trim().toLowerCase();
  if (!needle) return o.slice();
  return o.filter((x) => plainLabel(x.label).toLowerCase().includes(needle) || (x.search !== undefined && plainLabel(x.search).toLowerCase().includes(needle)));
}
