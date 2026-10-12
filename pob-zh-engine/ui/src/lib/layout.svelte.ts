// The window-level layout state: whether the left sidebar (build summary and
// POB's calculation sidebar) is shown. Below NARROW_W the window can fold it
// away to give the page its width; the choice is kept per viewer. Wider than
// that it is always shown.

const KEY = "pobtools.sidebarCollapsed";
/** window width (CSS px) below which the sidebar can be folded */
export const NARROW_W = 1100;

function readCollapsed(): boolean {
  try {
    const v = localStorage.getItem(KEY);
    // folded by default: a narrow window needs the room more than the summary
    return v == null ? true : v === "1";
  } catch {
    return true;
  }
}

class Layout {
  winW = $state(typeof window !== "undefined" ? window.innerWidth : 1920);
  collapsed = $state(readCollapsed());
  get narrow(): boolean {
    return this.winW < NARROW_W;
  }
  get sideHidden(): boolean {
    return this.narrow && this.collapsed;
  }
  toggleSide(): void {
    this.collapsed = !this.collapsed;
    try {
      localStorage.setItem(KEY, this.collapsed ? "1" : "0");
    } catch {
      /* private mode */
    }
  }
}

export const layout = new Layout();
