// 小地圖的座標換算:把一組樹座標等比例縮進 w×h 的畫布並置中。

export interface Fit {
  s: number;
  ox: number;
  oy: number;
}

export function fitTransform(points: Iterable<{ x: number; y: number }>, w: number, h: number, pad = 6): Fit {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const p of points) {
    if (p.x < minX) minX = p.x;
    if (p.x > maxX) maxX = p.x;
    if (p.y < minY) minY = p.y;
    if (p.y > maxY) maxY = p.y;
  }
  if (minX === Infinity) return { s: 1, ox: w / 2, oy: h / 2 };
  const s = Math.min((w - pad * 2) / Math.max(1, maxX - minX), (h - pad * 2) / Math.max(1, maxY - minY));
  return { s, ox: w / 2 - ((minX + maxX) / 2) * s, oy: h / 2 - ((minY + maxY) / 2) * s };
}
