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

  const colours: [string, string][] = [
    ["^xE05030", "life"],
    ["^x7070FF", "mana"],
    ["^x88FFFF", "es"],
    ["^xB97123", "fire"],
    ["^x3F6DB3", "cold"],
    ["^xADAA47", "lightning"],
    ["^xD02090", "chaos"],
    ["^7", "default"],
  ];

  async function reload() {
    const r = await app.run(() => api.getNotes());
    if (r) {
      text = r.text;
      saved = r.text;
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
    <span class="dim small">{t("notes.hint")}</span>
    <span class="grow"></span>
    <span class="k">{t("notes.colors")}</span>
    {#each colours as [code, name]}
      <button class="sw" title={code} style:color={`var(--c-${name}, var(--ink-0))`} onclick={() => insertColour(code)}>■</button>
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
    gap: 6px;
    height: 36px;
    padding: 0 12px;
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
    border: 0;
    background: none;
    font-size: 14px;
    padding: 0 2px;
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
