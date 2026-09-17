<!-- 備註頁:純文字編輯區 + 色碼預覽;失焦或 800 ms 靜止後送 set_notes。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import PobText from "../components/PobText.svelte";

  let text = $state("");
  let saved = $state("");
  let loadedRev = -1;
  let timer = 0;
  let ta = $state<HTMLTextAreaElement | null>(null);

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
      text = r.text;
      saved = r.text;
      if (r.colours?.length) colours = r.colours;
      loadedRev = r.rev;
    }
  }
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  async function flush() {
    clearTimeout(timer);
    if (text === saved) return;
    const r = await app.run(() => api.setNotes(text));
    if (r) {
      saved = text;
      loadedRev = r.rev;
      await app.refresh();
    }
  }
  function onInput() {
    clearTimeout(timer);
    timer = window.setTimeout(() => void flush(), 800);
  }
  function insertColour(code: string) {
    if (!ta) return;
    const s = ta.selectionStart;
    const e = ta.selectionEnd;
    text = text.slice(0, s) + code + text.slice(e);
    queueMicrotask(() => {
      ta!.focus();
      ta!.setSelectionRange(s + code.length, s + code.length);
    });
    onInput();
  }
</script>

<div class="page">
  <div class="bar">
    <span class="dim small hint">{t("notes.hint")}</span>
    <span class="grow"></span>
    <span class="k">{t("notes.colors")}</span>
    {#each colours as c}
      <button class="sw" title={c.code} style:color={swatch(c.code)} onclick={() => insertColour(c.code)}>{colourLabel(c.name)}</button>
    {/each}
  </div>
  <div class="two">
    <textarea class="input area" bind:this={ta} bind:value={text} oninput={onInput} onblur={flush} spellcheck="false"></textarea>
    <div class="preview">
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
  .bar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 6px;
    min-height: 36px;
    padding: 4px 12px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
  }
  .grow {
    flex: 1;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .sw {
    appearance: none;
    border: 1px solid var(--edge-0);
    border-radius: 3px;
    background: none;
    font-size: var(--fs-xs);
    padding: 1px 6px;
    white-space: nowrap;
  }
  .sw:hover {
    border-color: var(--edge-1);
  }
  .two {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1px;
    background: var(--edge-0);
  }
  .area {
    height: 100%;
    border: 0;
    border-radius: 0;
    padding: 12px 14px;
    font-family: var(--font-mono);
    font-size: var(--fs-sm);
    line-height: 1.5;
    resize: none;
    background: var(--surface-0);
  }
  .preview {
    background: var(--surface-1);
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
