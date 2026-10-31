// 計算頁收合的子區在 POB 沒給 `extra` 時,標題旁顯示的摘要:子區裡第一個純數值格。
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
