<!-- 天賦樹清單:POB TreeTab 的樹選擇、「管理天賦樹」、版本轉換、重置與網址匯入/匯出。
     compact = 物品頁用,只留樹選擇(ItemsTab 的 specSelect)。 -->
<script lang="ts" src="$lib/views/TreeSpecBar.ts"></script>

{#if specs}
  <Select class="select sm tsb-spec" value={String(specs.activeSpec)} title={t("tree.specSelect")} options={specs.specs.map((s) => ({ value: String(s.index), label: specLabel(s) }))} onchange={(v) => pick(String(v))} disabled={app.busy > 0} />
  {#if !compact}
    <!-- controlled: picking a version only asks (the convert dialog); the drop-down keeps showing the tree's version -->
    <Select class="select sm tsb-ver" value={specs.treeVersion} title={t("tree.version")} options={specs.versions.map((v) => ({ value: v.value, label: v.label }))} onchange={(v) => { if (String(v) !== specs!.treeVersion) convert = String(v); }} disabled={app.busy > 0} />
    <button class="btn ghost sm" onclick={() => { manage = true; manageSel = specs!.activeSpec; }}>{t("tree.specManage")}</button>
    <Select class="select sm tsb-cmp" value={String(compare ?? "")} title={t(specs.specs.length < 2 ? "tree.compareNeedTwo" : "tree.compareHint")}
      options={[{ value: "", label: `${t("tree.compare")}: ${t("tree.compareOff")}` }, ...specs.specs.filter((s) => s.index !== specs!.activeSpec).map((s) => ({ value: String(s.index), label: `${t("tree.compare")}: ${s.title || t("tree.specDefault")}` }))]}
      onchange={(v) => setCompare(String(v))} disabled={app.busy > 0 || (specs.specs.length < 2 && compare == null)} />
    <button class="btn sm danger tsb-reset" onclick={() => (reset = true)} disabled={app.busy > 0}>{t(specs.tattoos ? "tree.resetTattoos" : "tree.reset")}</button>
  {/if}
{/if}

{#if manage && specs}
  <div class="modal">
    <div class="dialog wide">
      <div class="label">{t("tree.specManage")}</div>
      <div class="rows">
        {#each specs.specs as s (s.index)}
          <!-- svelte-ignore a11y_click_events_have_key_events -->
          <div class="row" class:sel={manageSel === s.index} role="option" aria-selected={manageSel === s.index} tabindex="0"
            onclick={() => (manageSel = s.index)} ondblclick={() => pick(String(s.index))}>
            <span>{specLabel(s)}</span>
            {#if s.active}<span class="badge gold">{t("tree.specCurrent")}</span>{/if}
          </div>
        {/each}
      </div>
      <div class="btns">
        <button class="btn sm" onclick={() => (nameDialog = { op: "new", title: t("tree.specNewName") })}>{t("tree.specNew")}</button>
        <button class="btn sm" disabled={!manageSel} onclick={() => (nameDialog = { op: "copy", index: manageSel, title: specs!.specs[manageSel - 1]?.title ?? "" })}>{t("tree.specCopy")}</button>
        <button class="btn sm" disabled={!manageSel} onclick={() => (nameDialog = { op: "rename", index: manageSel, title: specs!.specs[manageSel - 1]?.title ?? "" })}>{t("tree.specRename")}</button>
        <button class="btn ghost sm" disabled={manageSel <= 1} onclick={() => move(manageSel, -1)}>↑</button>
        <button class="btn ghost sm" disabled={!manageSel || manageSel >= specs.specs.length} onclick={() => move(manageSel, 1)}>↓</button>
        <button class="btn sm danger" disabled={!manageSel || specs.specs.length <= 1} onclick={() => del(manageSel)}>{t("tree.specDelete")}</button>
        <span class="grow"></span>
        {#if specs.treeLinks}
          <button class="btn sm" onclick={() => (linkIn = { url: "", title: "", error: "" })}>{t("tree.linkImport")}</button>
          <button class="btn sm" onclick={exportLink}>{t("tree.linkExport")}</button>
        {/if}
        <button class="btn primary sm" onclick={() => (manage = false)}>{t("tree.done")}</button>
      </div>
    </div>
  </div>
{/if}

{#if nameDialog}
  <div class="modal top">
    <div class="dialog">
      <div class="label">{t(nameDialog.op === "rename" ? "tree.specRename" : nameDialog.op === "copy" ? "tree.specCopy" : "tree.specNew")}</div>
      <p class="dim">{t("tree.specNameHint")}</p>
      <!-- svelte-ignore a11y_autofocus -->
      <input class="input" autofocus bind:value={nameDialog.title} onkeydown={(e) => e.key === "Enter" && nameOk()} />
      <div class="btns"><button class="btn ghost" onclick={() => (nameDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" disabled={!nameDialog.title.trim()} onclick={nameOk}>OK</button></div>
    </div>
  </div>
{/if}

{#if convert && specs}
  <div class="modal top">
    <div class="dialog">
      <div class="label">{t("tree.convertTitle", { version: specs.versions.find((v) => v.value === convert)?.label ?? convert })}</div>
      <p class="dim">{t("tree.convertWarn")}</p>
      <div class="btns">
        <button class="btn ghost" onclick={() => (convert = null)}>{t("tree.cancel")}</button>
        <button class="btn" onclick={() => doConvert("all")} title={t("tree.convertAllHint")}>{t("tree.convertAll")}</button>
        <button class="btn" onclick={() => doConvert("copy")}>{t("tree.convertCopy")}</button>
        <button class="btn primary" onclick={() => doConvert("replace")}>{t("tree.convert")}</button>
      </div>
    </div>
  </div>
{/if}

{#if reset && specs}
  <div class="modal top">
    <div class="dialog">
      <div class="label">{t(specs.tattoos ? "tree.resetTattoos" : "tree.reset")}</div>
      <p class="dim">{t(specs.tattoos ? "tree.resetWarnTattoos" : "tree.resetWarn")}</p>
      <div class="btns">
        <button class="btn ghost" onclick={() => (reset = false)}>{t("tree.cancel")}</button>
        {#if specs.tattoos}<button class="btn" onclick={() => doReset(true)}>{t("tree.removeTattoos")}</button>{/if}
        <button class="btn danger" onclick={() => doReset(false)}>{t("tree.resetTree")}</button>
      </div>
    </div>
  </div>
{/if}

{#if linkIn}
  <div class="modal top">
    <div class="dialog">
      <div class="label">{t("tree.linkImport")}</div>
      <input class="input" placeholder={t("tree.specNameHint")} bind:value={linkIn.title} />
      <input class="input" placeholder={t("tree.linkHint")} bind:value={linkIn.url} onkeydown={(e) => e.key === "Enter" && importLink()} />
      {#if linkIn.error}<p class="bad small">{linkIn.error}</p>{/if}
      <div class="btns"><button class="btn ghost" onclick={() => (linkIn = null)}>{t("tree.cancel")}</button><button class="btn primary" disabled={!linkIn.url.trim() || !linkIn.title.trim()} onclick={importLink}>{t("tree.linkImportGo")}</button></div>
    </div>
  </div>
{/if}

{#if linkOut !== null}
  <div class="modal top">
    <div class="dialog">
      <div class="label">{t("tree.linkExport")}</div>
      <input class="input selectable" readonly value={linkOut} onfocus={(e) => e.currentTarget.select()} />
      <div class="btns">
        <button class="btn" onclick={async () => { if (await copyText(linkOut ?? "")) app.notice = t("tree.linkCopied"); }}>{t("tree.linkCopy")}</button>
        <button class="btn primary" onclick={() => (linkOut = null)}>{t("tree.done")}</button>
      </div>
    </div>
  </div>
{/if}

<style>
  :global(.select.tsb-spec) {
    max-width: 280px;
  }
  :global(.select.tsb-ver) {
    width: auto;
  }
  .modal {
    position: fixed;
    inset: 0;
    z-index: 200;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 70%, transparent);
  }
  .modal.top {
    z-index: 210;
  }
  .dialog {
    min-width: 380px;
    max-width: 640px;
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 18px 20px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
  }
  .dialog.wide {
    min-width: 560px;
  }
  .rows {
    max-height: 320px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 5px 10px;
    cursor: default;
  }
  .row:hover {
    background: var(--surface-2);
  }
  .row.sel {
    background: var(--gold-soft);
  }
  .badge.gold {
    padding: 0 7px;
    border: 1px solid var(--hl-dim);
    border-radius: 999px;
    color: var(--hl);
    font-size: var(--fs-2xs);
    white-space: nowrap;
  }
  .btns {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: flex-end;
    gap: 6px;
  }
  .grow {
    flex: 1;
  }
  :global(.select.tsb-cmp) {
    max-width: 200px;
  }
</style>
