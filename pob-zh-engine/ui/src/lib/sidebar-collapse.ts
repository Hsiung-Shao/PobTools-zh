// Folding the sidebar's stat groups: which groups the viewer folded (kept per
// viewer in localStorage) and the one-line summary a folded group shows in its
// heading, so the important numbers stay visible.
import { pobPlain } from "./pobtext";
import type { SidebarItem } from "./sidebar-groups";

const KEY = "pobtools.sidebar.collapsedGroups";

/** The first `max` values of the group's "label: value" rows, joined. */
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
    /* storage off: folding just isn't remembered */
  }
}
