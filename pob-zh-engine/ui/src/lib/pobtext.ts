// Path of Building colours its strings inline: `^N` picks one of ten palette
// slots, `^xRRGGBB` sets a literal colour, and either stays in force until
// the next code. These two helpers turn such a string into runs for the DOM.
//
// Palette slots follow POB's colorCodes order; the literal colours POB uses
// for the game's own semantics (life red, mana blue, unique orange...) map to
// our tokens so the page can restyle them, anything else passes through.

export interface TextRun {
  text: string;
  color: string | null;
}

const SLOT: readonly string[] = [
  "var(--fg-4)", // ^0 black-ish
  "var(--bad)", // ^1 red
  "var(--ok)", // ^2 green
  "var(--c-mana)", // ^3 blue
  "var(--c-rare)", // ^4 yellow
  "var(--c-chaos)", // ^5 purple
  "var(--c-es)", // ^6 cyan
  "var(--fg-0)", // ^7 white
  "var(--fg-2)", // ^8 grey
  "var(--fg-3)", // ^9 dark grey
];

const KNOWN: Record<string, string> = {
  ff7070: "var(--c-life)",
  fdb8b8: "var(--c-life)",
  "7070ff": "var(--c-mana)",
  "88ffff": "var(--c-es)",
  b97123: "var(--c-unique)",
  ffff77: "var(--c-rare)",
  "8888ff": "var(--c-magic)",
  c8c8c8: "var(--c-normal)",
  ffffff: "var(--fg-0)",
  "1aa29b": "var(--c-gem)",
  "74cabf": "var(--c-gem)",
  aa9e82: "var(--c-currency)",
  "808080": "var(--fg-2)",
  e05030: "var(--bad)",
  ff9922: "var(--warn)",
  "33ff77": "var(--ok)",
  "70ff70": "var(--ok)",
};

const HEX = /^[0-9a-fA-F]{6}$/;

export function pobRuns(s: string | null | undefined): TextRun[] {
  if (!s) return [];
  const runs: TextRun[] = [];
  let color: string | null = null;
  let start = 0;
  let i = 0;
  const cut = (end: number) => {
    if (end > start) runs.push({ text: s.slice(start, end), color });
  };
  while (i < s.length) {
    if (s.charCodeAt(i) === 94 /* ^ */ && i + 1 < s.length) {
      const next = s[i + 1];
      if ((next === "x" || next === "X") && i + 8 <= s.length && HEX.test(s.slice(i + 2, i + 8))) {
        cut(i);
        const hex = s.slice(i + 2, i + 8).toLowerCase();
        color = KNOWN[hex] ?? `#${hex}`;
        i += 8;
        start = i;
        continue;
      }
      if (next >= "0" && next <= "9") {
        cut(i);
        color = SLOT[next.charCodeAt(0) - 48] ?? null;
        i += 2;
        start = i;
        continue;
      }
    }
    i++;
  }
  cut(s.length);
  return runs;
}

/** The text with every colour code removed. */
export function pobPlain(s: string | null | undefined): string {
  return pobRuns(s)
    .map((r) => r.text)
    .join("");
}
