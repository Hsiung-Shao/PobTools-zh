<!-- 備註頁:純文字編輯區 + 色碼預覽;失焦或 800 ms 靜止後送 set_notes。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import { dataKey } from "$lib/lang.svelte";
  import PobText from "$lib/ui/PobText.svelte";
  import { TextHistory } from "$lib/textHistory";

  let text = $state("");
  let saved = $state("");
  let loadedRev = "";
  let timer = 0;
  let ta = $state<HTMLTextAreaElement | null>(null);
  let gutterEl = $state<HTMLElement | null>(null);
  const lineCount = $derived(text.split("\n").length);
  const gutter = $derived(Array.from({ length: lineCount }, (_, i) => i + 1).join("\n"));
  // Our own Ctrl+Z / Ctrl+Y: the colour buttons and the reload after a save set
  // the text from script, which empties the text box's own undo stack.
  const history = new TextHistory();
  const snap = () => {
    const v = ta?.value ?? text;
    return { text: v, start: ta?.selectionStart ?? v.length, end: ta?.selectionEnd ?? v.length };
  };
  // what the box held after the last change: an input records it as the step
  // back (input, not beforeinput -- script-driven edits send no beforeinput)
  let last = { text: "", start: 0, end: 0 };

  // POB's own colour buttons (NotesTab: NORMAL ... INTELLIGENCE, DEFAULT)
  let colours = $state<{ code: string; name: string }[]>([]);
  const swatch = (code: string) => (code.startsWith("^x") ? `#${code.slice(2)}` : "var(--ink-0)");
  const colourLabel = (name: string) => {
    const k = `notes.col.${name}`;
    const v = t(k);
    return v === k ? name : v;
  };

  async function reload() {
    const r = await app.run(() => api.getNotes());
    if (r) {
      // typed but not flushed yet (a re-read after F2 within the save delay):
      // keep it, the pending flush still sends it
      if (text !== saved && r.text === saved) {
        if (r.colours?.length) colours = r.colours;
        loadedRev = dataKey(r.rev, app.langRev);
        return;
      }
      // other notes (another build, a re-import): what came before is not theirs
      if (r.text !== text) history.reset();
      text = r.text;
      saved = r.text;
      last = { text, start: text.length, end: text.length };
      if (r.colours?.length) colours = r.colours;
      loadedRev = dataKey(r.rev, app.langRev);
    }
  }
  $effect(() => {
    const rev = app.dataRev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  async function flush() {
    clearTimeout(timer);
    if (text === saved) return;
    const r = await app.run(() => api.setNotes(text));
    if (r) {
      saved = text;
      loadedRev = dataKey(r.rev, app.langRev);
      await app.refresh();
    }
  }
  // the context menu's Undo / Redo come here, not through the keys
  function onBeforeInput(e: InputEvent) {
    if (e.inputType === "historyUndo" || e.inputType === "historyRedo") {
      e.preventDefault();
      step(e.inputType === "historyRedo");
    }
  }
  function onTyped(e: Event) {
    const kind = (e as InputEvent).inputType ?? "";
    if (last.text !== (ta?.value ?? text)) history.record(last, Date.now(), kind.startsWith("insertFromPaste") || kind.startsWith("deleteBy"));
    last = snap();
    onInput();
  }
  function onKey(e: KeyboardEvent) {
    if (!e.ctrlKey || e.altKey) return;
    const k = e.key.toLowerCase();
    if (k === "z" || k === "y") {
      e.preventDefault();
      e.stopPropagation();
      step(k === "y" || e.shiftKey);
    }
  }
  function step(redo: boolean) {
    const s = redo ? history.redo(snap()) : history.undo(snap());
    if (!s) return;
    text = s.text;
    last = s;
    queueMicrotask(() => ta?.setSelectionRange(s.start, s.end));
    onInput();
  }
  function onInput() {
    clearTimeout(timer);
    timer = window.setTimeout(() => void flush(), 800);
  }
  // NotesTab:SetColor. No selection: insert the code. A selection: colour it --
  // its own ^xRRGGBB codes are dropped, the new code goes in front and the colour
  // that was in effect at its end goes after it, so the text that follows keeps
  // its colour. (Replacing the selection with the code deleted what was selected.)
  function insertColour(code: string) {
    if (!ta) return;
    const s = ta.selectionStart;
    const e = ta.selectionEnd;
    let piece = code;
    if (e > s) {
      const sel = text.slice(s, e);
      const last = sel.match(/.*(\^x[0-9a-fA-F]{6})/s)?.[1] ?? "^7";
      piece = code + sel.replace(/\^x[0-9a-fA-F]{6}/g, "") + last;
    }
    history.record(snap(), Date.now(), true);
    last = { text: text.slice(0, s) + piece + text.slice(e), start: s + piece.length, end: s + piece.length };
    text = text.slice(0, s) + piece + text.slice(e);
    queueMicrotask(() => {
      ta!.focus();
      ta!.setSelectionRange(s + piece.length, s + piece.length);
    });
    onInput();
  }
</script>

<div class="page">
  <div class="toolbar">
    <span class="k">{t("notes.colors")}</span>
    {#each colours as c}
      <button class="swatch" title={colourLabel(c.name)} aria-label={colourLabel(c.name)} onclick={() => insertColour(c.code)}><i style:background={swatch(c.code)}></i></button>
    {/each}
    <span class="grow"></span>
    <span class="small" class:dim={text === saved} class:pending={text !== saved}>{t(text === saved ? "notes.saved" : "notes.saving")}</span>
    <span class="small num dim">{t("notes.stats", { chars: text.length, lines: lineCount })}</span>
  </div>
  <div class="two">
    <div class="card editor">
      <pre class="gutter" bind:this={gutterEl} aria-hidden="true">{gutter}</pre>
      <textarea class="input area" bind:this={ta} bind:value={text} placeholder={t("notes.hint")} onbeforeinput={onBeforeInput} onkeydown={onKey} oninput={onTyped} onblur={flush}
        onscroll={(e) => { if (gutterEl) gutterEl.scrollTop = e.currentTarget.scrollTop; }} spellcheck="false"></textarea>
    </div>
    <div class="card preview">
      <div class="label">{t("notes.preview")}</div>
      <div class="pv">
        {#each text.split("\n") as line}
          <div class="line"><PobText text={line} /></div>
        {/each}
      </div>
    </div>
  </div>
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .pending {
    color: var(--warn);
  }
  .grow {
    flex: 1;
  }
  .k {
    margin-right: 2px;
    font-size: var(--fs-xs);
    color: var(--ink-2);
  }
  .small {
    font-size: var(--fs-xs);
  }
  .swatch {
    appearance: none;
    display: grid;
    place-items: center;
    width: 26px;
    height: 26px;
    padding: 0;
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
    background: var(--surface-2);
  }
  .swatch:hover {
    border-color: var(--edge-2);
    background: var(--surface-hover);
  }
  .swatch i {
    width: 12px;
    height: 12px;
    border-radius: 50%;
  }
  .two {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    gap: 12px;
    padding: 16px;
  }
  /* a narrow main area: editor over preview */
  @container view (max-width: 759px) {
    .two {
      grid-template-columns: minmax(0, 1fr);
      grid-template-rows: minmax(160px, 1fr) minmax(160px, 1fr);
    }
  }
  .editor {
    display: flex;
    min-height: 0;
    overflow: hidden;
  }
  /* line numbers: same font, size, line height and top padding as the text box,
     which does not wrap, so row N is always line N */
  .gutter,
  .area {
    margin: 0;
    padding-top: 12px;
    font-family: var(--font-mono);
    font-size: var(--fs-sm);
    line-height: 1.5;
  }
  .gutter {
    flex: none;
    width: 44px;
    padding-right: 8px;
    /* + the text box's 9px horizontal scrollbar, so both reach the same last line */
    padding-bottom: calc(12px + 9px);
    overflow: hidden;
    border-right: 1px solid var(--edge-0);
    color: var(--ink-3);
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
  .area {
    flex: 1;
    min-width: 0;
    height: 100%;
    border: 0;
    border-radius: 0;
    padding-right: 14px;
    padding-bottom: 12px;
    padding-left: 12px;
    resize: none;
    background: transparent;
    white-space: pre;
    overflow: auto;
  }
  .area:focus {
    box-shadow: none;
  }
  .editor:focus-within {
    border-color: var(--gold);
  }
  .preview {
    padding: 12px 14px;
    overflow-y: auto;
    min-height: 0;
  }
  .pv {
    margin-top: 6px;
    font-size: var(--fs-sm);
    line-height: 1.5;
    white-space: pre-wrap;
    user-select: text;
  }
  .line {
    min-height: 1.5em;
  }
</style>
