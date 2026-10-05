// The DPS difference a gem makes, as the skills page shows it: POB's three
// colours (GemSelectControl:DPSBuilder -- green higher, red lower, yellow the
// same) and signed numbers with a real minus sign.

export const DPS_UP = "#228866";
export const DPS_DOWN = "#FF4422";
export const DPS_SAME = "#FFFF66";

const EPS = 1e-9;

/** POB's colour for a DPS change; undefined when there is no number. */
export function dpsDeltaColor(delta: number | null | undefined): string | undefined {
  if (typeof delta !== "number" || !Number.isFinite(delta)) return undefined;
  if (delta > EPS) return DPS_UP;
  if (delta < -EPS) return DPS_DOWN;
  return DPS_SAME;
}

function sign(n: number): string {
  return n > 0 ? "+" : n < 0 ? "−" : "±";
}

/** "+45,678" / "−45,678" / "±0" (whole numbers, grouped). */
export function formatSignedNumber(n: number): string {
  const r = Math.round(n);
  return `${sign(r)}${Math.abs(r).toLocaleString("en-US")}`;
}

/** "+12.3%" / "−0.4%" / "±0%" (one decimal, a trailing .0 dropped). */
export function formatSignedPct(p: number): string {
  const r = Math.round(p * 10) / 10;
  const abs = Math.abs(r);
  return `${sign(r)}${Number.isInteger(abs) ? abs.toFixed(0) : abs.toFixed(1)}%`;
}

/** The change as text: the percentage when the base is not 0, else the amount. */
export function dpsDeltaText(d: { delta?: number | null; pct?: number | null }): string {
  if (typeof d.pct === "number" && Number.isFinite(d.pct)) return formatSignedPct(d.pct);
  if (typeof d.delta === "number" && Number.isFinite(d.delta)) return formatSignedNumber(d.delta);
  return "";
}
