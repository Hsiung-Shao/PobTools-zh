/**
 * Height actually applied to the item editor's preview pane: the user's stored
 * height `h`, kept within [min, max] and also within the container height minus
 * `reserve` (the room left for the affix editor below), but never below `min`.
 */
export function clampPreview(
  h: number,
  containerH: number,
  o: { min?: number; reserve?: number; max?: number } = {},
): number {
  const min = o.min ?? 120;
  const reserve = o.reserve ?? 140;
  const max = o.max ?? 900;
  let v = Number.isFinite(h) ? h : min;
  v = Math.min(v, max);
  if (Number.isFinite(containerH) && containerH > 0) v = Math.min(v, containerH - reserve);
  return Math.round(Math.max(min, v));
}
