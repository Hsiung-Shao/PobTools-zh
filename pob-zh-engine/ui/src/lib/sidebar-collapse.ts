// 側欄分組收合:記住使用者收了哪些組(存在本機 localStorage),
// 以及收合後標題列顯示的一行摘要,讓重要數字仍看得到。
import { pobPlain } from "./pobtext";
import type { SidebarItem } from "./sidebar-groups";

const KEY = "pobtools.sidebar.collapsedGroups";

/** 取該組「標籤:值」列的前 `max` 個值,以「 · 」連接。 */
export function sidebarSummary(items: SidebarItem[], max = 3): string {
  const out: string[] = [];
  for (const { row } of items) {
    if (!row.lhs || !row.rhs || row.align === "CENTER_X") continue;
    const v = pobPlain(row.rhs).trim();
    if (v) out.push(v);
    if (out.length >= max) break;
  }
  return out.join(" · ");
}

export function loadCollapsed(): Set<string> {
  try {
    const v = JSON.parse(localStorage.getItem(KEY) ?? "[]");
    return new Set(Array.isArray(v) ? v.filter((k) => typeof k === "string") : []);
  } catch {
    return new Set();
  }
}

export function saveCollapsed(keys: Set<string>): void {
  try {
    localStorage.setItem(KEY, JSON.stringify([...keys]));
  } catch {
    /* 存不了就不記,不影響收合本身 */
  }
}
