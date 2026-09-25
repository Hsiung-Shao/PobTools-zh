// Undo / redo for a text box whose value the page also sets itself (the Notes
// tab: a colour button rewrites the text, and a reload after saving assigns it
// again). Any programmatic assignment wipes the browser's own undo stack, which
// is why Ctrl+Z there did nothing; this keeps its own.

export interface TextSnap {
  text: string;
  start: number;
  end: number;
}

export class TextHistory {
  private undos: TextSnap[] = [];
  private redos: TextSnap[] = [];
  private lastAt = -Infinity;

  constructor(
    private readonly limit = 200,
    /** Typing within this many ms of the last recorded change is one undo step. */
    private readonly groupMs = 1000,
  ) {}

  reset() {
    this.undos = [];
    this.redos = [];
    this.lastAt = -Infinity;
  }

  get canUndo() {
    return this.undos.length > 0;
  }
  get canRedo() {
    return this.redos.length > 0;
  }

  /**
   * Call before a change with the state it replaces. `step` forces a step of its
   * own (a colour button, a paste) instead of joining the typing before it.
   */
  record(before: TextSnap, now = Date.now(), step = false) {
    this.redos = [];
    const joins = !step && this.undos.length > 0 && now - this.lastAt < this.groupMs;
    this.lastAt = step ? -Infinity : now;
    if (joins) return;
    if (this.undos.length && this.undos[this.undos.length - 1].text === before.text) return;
    this.undos.push(before);
    if (this.undos.length > this.limit) this.undos.shift();
  }

  /** The state to go back to, given what is there now; null when there is none. */
  undo(current: TextSnap): TextSnap | null {
    const s = this.undos.pop();
    if (!s) return null;
    this.redos.push(current);
    this.lastAt = -Infinity;
    return s;
  }

  redo(current: TextSnap): TextSnap | null {
    const s = this.redos.pop();
    if (!s) return null;
    this.undos.push(current);
    this.lastAt = -Infinity;
    return s;
  }
}
