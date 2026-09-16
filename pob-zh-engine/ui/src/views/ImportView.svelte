<!-- 匯入 / 匯出:分享碼(貼上 → 預覽 → 匯入)、產生分享碼、角色 JSON 匯入。
     不打網路:帳號匯入請使用者自己把 get-items / get-passive-skills 的回應貼進來。 -->
<script lang="ts">
  import { api, type CodeInfo } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  let code = $state("");
  let preview = $state<CodeInfo | null>(null);
  let previewErr = $state<string | null>(null);
  let decodeTimer = 0;

  let exported = $state("");
  let copied = $state(false);

  let itemsJson = $state("");
  let passivesJson = $state("");
  let importTree = $state(true);
  let importItems = $state(true);
  let deleteJewels = $state(false);
  let clearItems = $state(false);
  let clearSkills = $state(false);
  let ignoreWeaponSwap = $state(false);
  let charResult = $state<string | null>(null);

  function onCodeInput() {
    clearTimeout(decodeTimer);
    preview = null;
    previewErr = null;
    const c = code.trim();
    if (!c) return;
    decodeTimer = window.setTimeout(async () => {
      try {
        preview = await api.decodeCode(c);
      } catch (e: any) {
        previewErr = String(e?.message ?? e);
      }
    }, 250);
  }

  async function doImport(mode: "replace" | "new") {
    if (!preview) return;
    if (mode === "replace" && !confirm(t("import.replaceConfirm"))) return;
    const r = await app.run(() => api.importCode(code.trim(), mode));
    if (r) {
      await app.afterReinit();
      app.view = "tree";
    }
  }

  async function doExport() {
    copied = false;
    const r = await app.run(() => api.exportCode());
    if (r) exported = r.code;
  }

  async function copyExport() {
    try {
      await navigator.clipboard.writeText(exported);
      copied = true;
    } catch {
      copied = false;
    }
  }

  async function doCharImport() {
    charResult = null;
    const r = await app.run(() =>
      api.importCharacter({
        items: itemsJson.trim(),
        passives: passivesJson.trim(),
        importTree,
        importItems,
        deleteJewels,
        clearItems,
        clearSkills,
        ignoreWeaponSwap,
      }),
    );
    if (r) {
      charResult = r.imported.join(" + ");
      await app.refresh();
    }
  }
</script>

<div class="page">
  <section class="card">
    <h2>{t("import.codeTitle")}</h2>
    <p class="dim">{t("import.codeHint")}</p>
    <textarea class="input area" rows="4" bind:value={code} oninput={onCodeInput} placeholder={t("import.codePlaceholder")}></textarea>
    {#if previewErr}
      <div class="bad">{previewErr}</div>
    {:else if preview}
      <div class="preview">
        <span class="gold">{preview.ascendClassNameZh || preview.ascendClassName || preview.classNameZh || preview.className || "?"}</span>
        <span class="dim">{t("sidebar.level")} {preview.level ?? "?"}</span>
        <span class="dim">·</span>
        <span>{t("import.items", { n: preview.itemCount ?? 0 })}</span>
        <span>{t("import.skills", { n: preview.skillCount ?? 0 })}</span>
        {#if preview.hasTree}<span>{t("import.tree")}</span>{/if}
        <span class="dim">{preview.targetVersion ?? ""}</span>
      </div>
      <div class="actions">
        <button class="btn primary" disabled={app.busy > 0} onclick={() => doImport("new")}>{t("import.asNew")}</button>
        <button class="btn" disabled={app.busy > 0 || !app.loaded} onclick={() => doImport("replace")}>{t("import.replace")}</button>
      </div>
    {/if}
  </section>

  <section class="card">
    <h2>{t("import.exportTitle")}</h2>
    <p class="dim">{t("import.exportHint")}</p>
    <div class="actions">
      <button class="btn primary" disabled={app.busy > 0 || !app.loaded} onclick={doExport}>{t("import.generate")}</button>
      {#if exported}
        <button class="btn" onclick={copyExport}>{copied ? t("import.copied") : t("import.copy")}</button>
        <span class="dim num">{exported.length}</span>
      {/if}
    </div>
    {#if exported}
      <textarea class="input area selectable" rows="4" readonly value={exported}></textarea>
    {/if}
  </section>

  <section class="card">
    <h2>{t("import.charTitle")}</h2>
    <p class="dim">{t("import.charHint")}</p>
    <div class="two">
      <label class="col">
        <span class="k">{t("import.charItems")}</span>
        <textarea class="input area" rows="5" bind:value={itemsJson} placeholder={'{ "items": [...], "character": {...} }'}></textarea>
      </label>
      <label class="col">
        <span class="k">{t("import.charPassives")}</span>
        <textarea class="input area" rows="5" bind:value={passivesJson} placeholder={'{ "hashes": [...], "items": [...] }'}></textarea>
      </label>
    </div>
    <div class="opts">
      <label><input type="checkbox" bind:checked={importTree} /> {t("import.optTree")}</label>
      <label><input type="checkbox" bind:checked={deleteJewels} disabled={!importTree} /> {t("import.optDeleteJewels")}</label>
      <span class="vsep"></span>
      <label><input type="checkbox" bind:checked={importItems} /> {t("import.optItems")}</label>
      <label><input type="checkbox" bind:checked={clearItems} disabled={!importItems} /> {t("import.optClearItems")}</label>
      <label><input type="checkbox" bind:checked={clearSkills} disabled={!importItems} /> {t("import.optClearSkills")}</label>
      <label><input type="checkbox" bind:checked={ignoreWeaponSwap} disabled={!importItems} /> {t("import.optIgnoreSwap")}</label>
    </div>
    <div class="actions">
      <button class="btn primary" disabled={app.busy > 0 || !app.loaded || (!itemsJson.trim() && !passivesJson.trim())} onclick={doCharImport}>{t("import.charGo")}</button>
      {#if charResult}<span class="ok">{t("import.charDone", { what: charResult })}</span>{/if}
    </div>
  </section>
</div>

<style>
  .page {
    height: 100%;
    overflow-y: auto;
    padding: 16px 18px 24px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    max-width: 980px;
  }
  .card {
    padding: 14px 16px 16px;
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  h2 {
    margin: 0;
    font-size: var(--fs-md);
    font-weight: 600;
    padding-left: 10px;
    position: relative;
  }
  h2::before {
    content: "";
    position: absolute;
    left: 0;
    top: 3px;
    bottom: 3px;
    width: 2px;
    background: var(--gold);
    border-radius: 1px;
  }
  p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .area {
    height: auto;
    padding: 8px 9px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    resize: vertical;
    line-height: 1.4;
  }
  .preview {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    align-items: baseline;
    font-size: var(--fs-sm);
  }
  .gold {
    color: var(--gold);
    font-weight: 600;
  }
  .actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }
  .two {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .opts {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 14px;
    font-size: var(--fs-xs);
    color: var(--ink-1);
  }
  .opts label {
    display: inline-flex;
    align-items: center;
    gap: 5px;
  }
  .vsep {
    width: 1px;
    height: 14px;
    background: var(--edge-1);
    align-self: center;
  }
  .bad {
    color: var(--bad);
    font-size: var(--fs-xs);
  }
  .ok {
    color: var(--ok);
    font-size: var(--fs-xs);
  }
</style>
