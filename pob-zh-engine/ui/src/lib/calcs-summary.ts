// What a folded Calcs subsection shows next to its title when POB gives it no
// `extra` of its own: the first plain value in it.
import type { CalcSubsection } from "./bridge";
import { pobPlain } from "./pobtext";

export function subsectionSummary(sub: CalcSubsection): string {
  for (const row of sub.rows) {
    for (const cell of row.cells) {
      if (cell.control || cell.widget) continue;
      const v = pobPlain(cell.text).trim();
      if (v) return v;
    }
  }
  return "";
}
