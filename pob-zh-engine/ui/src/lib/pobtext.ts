/**
 * Path of Building colours strings with inline escapes: `^7` (palette index)
 * and `^xRRGGBB` (literal). These helpers split such strings into spans.
 *
 * Borrowed verbatim from pob-redux (https://github.com/juddisjudd/pob-redux),
 * MIT License, (c) 2026 Judd. See NOTICE.md.
 */

export interface Span {
  text: string;
  color: string | null;
}

const PALETTE: Record<string, string> = {
  "0": "var(--fg-4)",
  "1": "var(--bad)",
  "2": "var(--ok)",
  "3": "var(--c-mana)",
  "4": "var(--c-rare)",
  "5": "var(--c-chaos)",
  "6": "var(--c-es)",
  "7": "var(--fg-0)",
  "8": "var(--fg-2)",
  "9": "var(--fg-3)",
};

// PoB's named colour codes, mapped onto the UI palette where they carry meaning.
const HEX_ALIASES: Record<string, string> = {
  "ff7070": "var(--c-life)",
  "7070ff": "var(--c-mana)",
  "88ffff": "var(--c-es)",
  "b97123": "var(--c-unique)",
  "ffff77": "var(--c-rare)",
  "8888ff": "var(--c-magic)",
  "c8c8c8": "var(--c-normal)",
  ffffff: "var(--fg-0)",
  "1aa29b": "var(--c-gem)",
  "74cabf": "var(--c-gem)",
  "f5d076": "var(--c-spirit)",
  "aa9e82": "var(--c-currency)",
  "808080": "var(--fg-2)",
  "e05030": "var(--bad)",
  "ff9922": "var(--warn)",
  "33ff77": "var(--ok)",
  "fdb8b8": "var(--c-life)",
  "70ff70": "var(--ok)",
};

export function parsePobText(s: string | null | undefined): Span[] {
  if (!s) return [];
  const spans: Span[] = [];
  let color: string | null = null;
  let buf = "";
  let i = 0;
  const flush = () => {
    if (buf) spans.push({ text: buf, color });
    buf = "";
  };
  while (i < s.length) {
    const ch = s[i];
    if (ch === "^" && i + 1 < s.length) {
      const nx = s[i + 1];
      if (nx === "x" && i + 7 < s.length && /^[0-9a-fA-F]{6}$/.test(s.slice(i + 2, i + 8))) {
        flush();
        const hex = s.slice(i + 2, i + 8).toLowerCase();
        color = HEX_ALIASES[hex] ?? `color-mix(in srgb, #${hex} var(--pob-mix, 100%), var(--fg-0))`;
        i += 8;
        continue;
      }
      if (/[0-9]/.test(nx)) {
        flush();
        color = PALETTE[nx] ?? null;
        i += 2;
        continue;
      }
    }
    buf += ch;
    i++;
  }
  flush();
  return spans;
}

export function stripPobText(s: string | null | undefined): string {
  return parsePobText(s)
    .map((p) => p.text)
    .join("");
}
