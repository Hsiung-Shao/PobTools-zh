<!-- 天賦樹清單:POB TreeTab 的樹選擇、「管理天賦樹」、版本轉換、重置與網址匯入/匯出。
     compact = 物品頁用,只留樹選擇(ItemsTab 的 specSelect)。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type SpecList } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  let { compact = false }: { compact?: boolean } = $props();

  let specs = $state<SpecList | null>(null);
  let loadedRev = -1;
  let manage = $state(false);
  let manageSel = $state(0);
  let nameDialog = $state<{ op: "new" | "copy" | "rename"; index?: number; title: string } | null>(null);
  let convert = $state<string | null>(null);
  let reset = $state(false);
  let linkIn = $state<{ url: string; title: string; error: string } | null>(null);
  let linkOut = $state<string | null>(null);

  async function reload() {
    try {
      specs = await api.listSpecs();
      loadedRev = app.rev;
    } catch {
      specs = null;
    }
  }
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  async function done(r: SpecList | null | undefined | false) {
    if (!r) return;
    specs = r;
    await app.afterTreeChange();
    loadedRev = app.rev;
  }
  const specLabel = (s: SpecList["specs"][number]) =>
    `${s.latest ? "" : `[${s.versionLabel}] `}${s.title || t("tree.specDefault")} (${s.ascendClassNameZh || s.ascendClassName || s.classNameZh || s.className}, ${s.points})`;

  async function pick(v: string) {
    const i = Number(v);
    if (!specs || i === specs.activeSpec) return;
    await done(await app.run(() => api.setActiveSpec(i)));
  }
  async function nameOk() {
    if (!nameDialog || !nameDialog.title.trim()) return;
    const d = nameDialog;
    nameDialog = null;
    await done(await app.run(() => api.specOp({ op: d.op, index: d.index, title: d.title.trim() })));
  }
  async function del(i: number) {
    const s = specs?.specs[i - 1];
    if (!s || !confirm(t("tree.specDeleteConfirm", { name: s.title || t("tree.specDefault") }))) return;
    await done(await app.run(() => api.specOp({ op: "delete", index: i })));
    manageSel = 0;
  }
  async function move(i: number, d: -1 | 1) {
    if (!specs || i + d < 1 || i + d > specs.specs.length) return;
    await done(await app.run(() => api.specOp({ op: "move", index: i, to: i + d })));
    manageSel = i + d;
  }
  async function doConvert(mode: "replace" | "copy" | "all") {
    const v = convert;
    convert = null;
    if (!v) return;
    await done(await app.run(() => api.convertTree(v, { copy: mode === "copy", all: mode === "all" })));
  }
  async function doReset(tattoos: boolean) {
    reset = false;
    const r = await app.run(() => api.resetTree(tattoos));
    if (r) {
      await app.afterTreeChange();
      await reload();
    }
  }
  async function importLink() {
    if (!linkIn) return;
    const d = linkIn;
    try {
      const r = await api.importTreeUrl(d.url, d.title);
      linkIn = null;
      await done(r);
    } catch (e: any) {
      d.error = String(e?.message ?? e);
    }
  }
  // TreeTab's Compare tick + its tree drop-down in one control. What is being
  // compared comes back from POB with the spec list, so leaving the tab and coming
  // back (or switching trees) still shows it.
  const compare = $derived(specs?.compareSpec ?? null);
  async function setCompare(v: string) {
    const i = v ? Number(v) : null;
    const r = await app.run(() => api.setCompareSpec(i ?? undefined));
    if (!r) return;
    await app.afterTreeChange();
    await reload();
    app.treeNonce++; // no recalculation happened, so the revision alone would not redraw the rings
  }

  async function exportLink() {
    const r = await app.run(() => api.exportTreeUrl());
    if (r) linkOut = r.url;
  }
</script>

{#if specs}
  <select class="select sm spec" value={String(specs.activeSpec)} title={t("tree.specSelect")} onchange={(e) => pick(e.currentTarget.value)} disabled={app.busy > 0}>
    {#each specs.specs as s (s.index)}<option value={String(s.index)}>{specLabel(s)}</option>{/each}
  </select>
  {#if !compact}
    <button class="btn ghost sm" onclick={() => { manage = true; manageSel = specs!.activeSpec; }}>{t("tree.specManage")}</button>
    <select class="select sm ver" value={specs.treeVersion} title={t("tree.version")} onchange={(e) => { const v = e.currentTarget.value; e.currentTarget.value = specs!.treeVersion; if (v !== specs!.treeVersion) convert = v; }} disabled={app.busy > 0}>
      {#each specs.versions as v (v.value)}<option value={v.value}>{v.label}</option>{/each}
    </select>
    <select class="select sm cmp" value={String(compare ?? "")} title={t(specs.specs.length < 2 ? "tree.compareNeedTwo" : "tree.compareHint")}
      onchange={(e) => setCompare(e.currentTarget.value)} disabled={app.busy > 0 || (specs.specs.length < 2 && compare == null)}>
      <option value="">{t("tree.compare")}: {t("tree.compareOff")}</option>
      {#each specs.specs as s (s.index)}
        {#if s.index !== specs.activeSpec}<option value={String(s.index)}>{t("tree.compare")}: {s.title || t("tree.specDefault")}</option>{/if}
      {/each}
    </select>
    <button class="btn ghost sm" onclick={() => (reset = true)} disabled={app.busy > 0}>{t(specs.tattoos ? "tree.resetTattoos" : "tree.reset")}</button>
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
  .spec {
    max-width: 280px;
  }
  .ver {
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
    border-radius: 8px;
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
    background: color-mix(in srgb, var(--gold) 18%, transparent);
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
  .cmp {
    max-width: 200px;
  }
</style>
