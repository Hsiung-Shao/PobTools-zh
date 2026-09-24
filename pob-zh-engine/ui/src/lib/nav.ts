// The "previous screen" the mouse's back / forward buttons (and Alt+Left /
// Alt+Right) walk: a browser-style history of views, kept apart from the rune
// state so it can be tested on its own.

export const NAV_LIMIT = 30;

export class ViewHistory<V> {
  back: V[] = [];
  fwd: V[] = [];

  /** A normal move from `from` to `to`: `from` becomes "back", the forward trail is dropped. */
  visit(from: V, to: V) {
    if (from === to) return;
    this.back.push(from);
    if (this.back.length > NAV_LIMIT) this.back.shift();
    this.fwd = [];
  }

  /**
   * The screen before `current` that can still be shown (`ok` false for, say, a
   * build tab once no build is loaded -- those entries are dropped on the way).
   * Undefined when there is none; nothing changes then.
   */
  goBack(current: V, ok: (v: V) => boolean): V | undefined {
    return this.step(this.back, this.fwd, current, ok);
  }

  goForward(current: V, ok: (v: V) => boolean): V | undefined {
    return this.step(this.fwd, this.back, current, ok);
  }

  clear() {
    this.back = [];
    this.fwd = [];
  }

  private step(from: V[], to: V[], current: V, ok: (v: V) => boolean): V | undefined {
    while (from.length) {
      const v = from.pop()!;
      if (v === current || !ok(v)) continue;
      to.push(current);
      if (to.length > NAV_LIMIT) to.shift();
      return v;
    }
    return undefined;
  }
}

/** The folder above `subPath` ("a/b/" -> "a/", "a/" -> ""). */
export function parentFolder(subPath: string): string {
  return subPath.replace(/[^/]+\/$/, "");
}

/** "a/b/" -> [{ name: "a", path: "a/" }, { name: "b", path: "a/b/" }]. */
export function folderCrumbs(subPath: string): { name: string; path: string }[] {
  const out: { name: string; path: string }[] = [];
  let path = "";
  for (const name of subPath.split("/")) {
    if (!name) continue;
    path += `${name}/`;
    out.push({ name, path });
  }
  return out;
}
