import type { TooltipLine } from "./bridge";

/** A plain tooltip string (POB's tooltipText) as card lines: one per "\n", leading tabs become `indent`. */
export function textToTooltipLines(text: string): Extract<TooltipLine, { text: string }>[] {
  return text.split("\n").map((s) => {
    const tabs = /^\t*/.exec(s)![0].length;
    const body = s.slice(tabs);
    return { size: 14, text: body, raw: body, center: false, indent: tabs };
  });
}
