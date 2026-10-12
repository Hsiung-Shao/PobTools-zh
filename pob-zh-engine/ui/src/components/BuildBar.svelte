<!-- 建置列(頂列第二列):等級、主技能選擇、儲存。全部走 POB 自己的
     頂列控制項回呼(get_build_header / set_build_field / save_build*)。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type BuildHeader, type DdEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import type { SelOption } from "$lib/select";
  import { app } from "$lib/state.svelte";
  import ClassChangeDialog from "./ClassChangeDialog.svelte";
  import Select from "./Select.svelte";
  import MinionLibrary from "./MinionLibrary.svelte";

  const h = $derived(app.header);
  let levelDraft = $state<string>("");
  // Build.lua's loadout drop-down: one pick switches tree, items, skills and config
  let loadouts = $state<import("$lib/bridge").LoadoutList | null>(null);
  let loadoutRev = -1;
  let newLoadout = $state<string | null>(null);
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (rev === loadoutRev) return;
    loadoutRev = rev;
    untrack(async () => {
      try {
        loadouts = await api.listLoadouts();
      } catch {
        loadouts = null;
      }
    });
  });
  async function pickLoadout(v: string) {
    const i = Number(v);
    const e = loadouts?.entries.find((x) => x.index === i);
    if (!e) return;
    if (e.label === "New Loadout") {
      newLoadout = "";
      return;
    }
    const r = await app.run(() => api.selectLoadout(i));
    if (r) {
      loadouts = r;
      await app.refresh();
    }
  }
  async function makeLoadout() {
    const title = (newLoadout ?? "").trim();
    const i = loadouts?.entries.find((x) => x.label === "New Loadout")?.index;
    newLoadout = null;
    if (!title || !i) return;
    const r = await app.run(() => api.selectLoadout(i, title));
    if (r) {
      loadouts = r;
      await app.refresh();
    }
  }

  // POB's spectre / beast library (Build.lua's Manage Spectres...)
  let library = $state<"spectre" | "beast" | null>(null);

  let saveAsOpen = $state(false);
  let saveAsName = $state("");
  // the folder the build goes into, relative to POB's build folder ("" = top)
  let saveAsDir = $state("");
  let saveAsFolders = $state<string[]>([]);
  let saveAsNewFolder = $state<string | null>(null);
  async function openSaveAs() {
    saveAsName = h?.buildName ?? "";
    const root = app.buildPath.split(String.fromCharCode(92)).join("/").replace(/[/]+$/, "").toLowerCase();
    const file = (h?.dbFileName ?? "").split(String.fromCharCode(92)).join("/");
    const dir = file.slice(0, file.lastIndexOf("/") + 1);
    saveAsDir = file && dir.toLowerCase().startsWith(root + "/") ? dir.slice(root.length + 1) : "";
    saveAsOpen = true;
    await loadFolders(saveAsDir);
  }
  async function loadFolders(dir: string) {
    const r = await app.run(() => api.listBuilds(dir, { filter: "" }));
    if (!r) return;
    saveAsDir = r.subPath;
    saveAsFolders = r.entries.filter((e) => e.isFolder).map((e) => e.folderName ?? "");
  }
  async function makeFolder() {
    const name = (saveAsNewFolder ?? "").trim();
    saveAsNewFolder = null;
    if (!name) return;
    const r = await app.run(() => api.newFolder(saveAsDir, name));
    if (r) await loadFolders(saveAsDir + name + "/");
  }

  // class / ascendancy (Build.lua's classDrop / ascendDrop / secondaryAscendDrop)
  const pick = $derived(h?.classPick ?? null);
  const currentClass = $derived(h?.classes?.find((c) => c.id === pick?.classId) ?? null);
  let classConfirm = $state<{ classId: number; className: string; connectFailed: boolean } | null>(null);

  $effect(() => {
    if (h) levelDraft = String(h.level);
  });
  // the drop-downs are controlled: they show POB's answer (pick.*), not what was clicked
  const dd = (l: DdEntry[], byIndex = false): SelOption[] => l.map((o, i) => ({ value: byIndex ? i + 1 : (o.val ?? i + 1), label: o.labelZh || o.label }));

  async function setField(field: string, value: unknown) {
    const r = await app.run(() => api.setBuildField(field, value));
    if (r) await app.refresh();
  }

  async function changeClass(classId: number, confirm?: "reset" | "connect") {
    const r = await app.run(() => api.setClass(classId, confirm));
    if (!r) return app.refresh();
    if (r.needsConfirm) {
      classConfirm = { classId, className: r.classNameZh || r.className, connectFailed: !!r.connectFailed };
      if (!r.connectFailed) return;
    }
    await app.afterTreeChange();
  }
  async function answerClass(mode: "reset" | "connect") {
    if (!classConfirm) return;
    const id = classConfirm.classId;
    classConfirm = null;
    await changeClass(id, mode);
  }
  async function cancelClass() {
    classConfirm = null;
    await app.refresh();
  }
  async function setAscendancy(id: number) {
    const r = await app.run(() => api.setAscendancy(id));
    await (r ? app.afterTreeChange() : app.refresh());
  }
  async function setSecondary(id: number) {
    const r = await app.run(() => api.setSecondaryAscendancy(id));
    await (r ? app.afterTreeChange() : app.refresh());
  }

  function commitLevel() {
    const n = Math.max(1, Math.min(100, Number(levelDraft) || 1));
    if (h && n !== h.level) void setField("level", n);
    else if (h) levelDraft = String(h.level);
  }

  async function save() {
    if (!h) return;
    await app.saveOrAsk();
  }
  // app.saveOrAsk (Ctrl+S, a "save first?" answer) asks for this dialog
  let seenSaveAsRequest = app.saveAsRequest;
  $effect(() => {
    const n = app.saveAsRequest;
    if (n !== seenSaveAsRequest) {
      seenSaveAsRequest = n;
      void openSaveAs();
    }
  });

  async function saveAs() {
    const name = saveAsName.trim();
    if (!name) return;
    saveAsOpen = false;
    const path = saveAsDir + name;
    // another build already has this name: POB greys Save out, here we ask
    let r = await app.saveAs(path);
    if (r === "exists" && confirm(t("bar.overwriteConfirm", { name }))) r = await app.saveAs(path, true);
    // not saved after all: a pending "then" (close, import a link) is dropped
    if (r !== "saved") app.cancelSaveAs();
  }

  export function onKey(e: KeyboardEvent): boolean {
    if (e.ctrlKey && !e.shiftKey && e.key.toLowerCase() === "s") {
      e.preventDefault();
      void save();
      return true;
    }
    return false;
  }

  const selectClass = "select sm";
</script>

{#if h}
  <div class="bar">
    <label class="field">
      <span class="k">{t("bar.level")}</span>
      <input
        class="input num lvl"
        type="number"
        min="1"
        max="100"
        bind:value={levelDraft}
        onblur={commitLevel}
        onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()}
      />
      <button class="btn ghost sm" class:on={h.levelAuto} title={t("bar.levelAutoTip")} onclick={() => setField("levelAuto", !h.levelAuto)}>
        {h.levelAuto ? t("bar.levelAuto") : t("bar.levelManual")}
      </button>
    </label>

    {#if pick && h.classes}
      <label class="field">
        <span class="k">{t("bar.class")}</span>
        <Select class="{selectClass} cls" ariaLabel={t("bar.class")} disabled={app.busy > 0} value={pick.classId} options={h.classes.map((c) => ({ value: c.id, label: c.nameZh || c.name }))} onchange={(v) => void changeClass(Number(v))} />
        {#if currentClass}
          <Select class="{selectClass} cls" disabled={app.busy > 0} value={pick.ascendClassId} options={currentClass.ascendancies.map((a) => ({ value: a.id, label: a.nameZh || a.name }))} onchange={(v) => void setAscendancy(Number(v))} />
        {/if}
        {#if h.secondaryAscendancies && h.secondaryAscendancies.length > 1}
          <Select class="{selectClass} cls" disabled={app.busy > 0} title={t("bar.ascend2")} value={pick.secondaryAscendClassId} options={h.secondaryAscendancies.map((a) => ({ value: a.id, label: a.nameZh || a.name }))} onchange={(v) => void setSecondary(Number(v))} />
        {/if}
      </label>
    {/if}

    <label class="field grow">
      <span class="k">{t("bar.mainSkill")}</span>
      <Select class={selectClass} ariaLabel={t("bar.mainSkill")} value={h.mainSocketGroup.index} options={dd(h.mainSocketGroup.list)} onchange={(v) => setField("mainSocketGroup", Number(v))} />
      {#if loadouts && loadouts.entries.length > 1}
        <Select class={selectClass} value={String(loadouts.selIndex)} title={t("bar.loadout")} options={loadouts.entries.map((e) => ({ value: String(e.index), label: e.kind === "loadout" ? e.label : e.labelZh || e.label, disabled: e.kind === "header" }))} onchange={(v) => pickLoadout(String(v))} />
      {/if}
      {#if h.mainSkill}
        <Select class={selectClass} value={h.mainSkill.index} disabled={h.mainSkill.enabled === false} options={dd(h.mainSkill.list)} onchange={(v) => setField("mainSkill", Number(v))} />
      {/if}
      {#if h.mainSkillPart}
        <Select class={selectClass} value={h.mainSkillPart.index} options={dd(h.mainSkillPart.list)} onchange={(v) => setField("mainSkillPart", Number(v))} />
      {/if}
      {#if h.mainSkillStageCount != null}
        <span class="k">{t("bar.stages")}</span>
        <input class="input num cnt" type="number" min="1" value={h.mainSkillStageCount} onchange={(e) => setField("mainSkillStageCount", Number(e.currentTarget.value))} />
      {/if}
      {#if h.mainSkillMineCount != null}
        <span class="k">{t("bar.mines")}</span>
        <input class="input num cnt" type="number" min="0" value={h.mainSkillMineCount} onchange={(e) => setField("mainSkillMineCount", Number(e.currentTarget.value))} />
      {/if}
      {#if h.mainSkillMinion}
        <Select class={selectClass} value={h.mainSkillMinion.index} disabled={h.mainSkillMinion.enabled === false} options={dd(h.mainSkillMinion.list, true)} onchange={(v) => setField("mainSkillMinion", Number(v))} />
      {/if}
      {#if h.statSet}
        <Select class={selectClass} value={h.statSet.index} disabled={h.statSet.enabled === false} options={dd(h.statSet.list)} onchange={(v) => setField("statSet", Number(v))} title={t("bar.statSet")} />
      {/if}
      {#if h.minionLibrary}
        <button class="btn ghost sm" onclick={() => (library = "spectre")}>{t("bar.spectres")}</button>
      {/if}
      {#if h.beastLibrary}
        <button class="btn ghost sm" onclick={() => (library = "beast")}>{t("bar.beasts")}</button>
      {/if}
      {#if h.minionStatSet}
        <Select class={selectClass} value={h.minionStatSet.index} options={dd(h.minionStatSet.list)} onchange={(v) => setField("minionStatSet", Number(v))} title={t("bar.statSet")} />
      {/if}
      {#if h.mainSkillMinionSkill}
        <Select class={selectClass} value={h.mainSkillMinionSkill.index} options={dd(h.mainSkillMinionSkill.list)} onchange={(v) => setField("mainSkillMinionSkill", Number(v))} />
      {/if}
    </label>

    <span class="actions">
      {#if app.savedAt}<span class="dim savedat">{t("bar.savedAt", { time: app.savedAt })}</span>{/if}
      <button class="btn sm" class:primary={h.unsaved} disabled={app.busy > 0} title="Ctrl+S" onclick={save}>{t("bar.save")}</button>
      <button class="btn ghost sm" disabled={app.busy > 0} onclick={openSaveAs}>{t("bar.saveAs")}</button>
    </span>
  </div>

  {#if classConfirm}
    <ClassChangeDialog className={classConfirm.className} connectFailed={classConfirm.connectFailed} onanswer={answerClass} oncancel={cancelClass} />
  {/if}

  {#if library}
    <MinionLibrary kind={library} onclose={() => (library = null)} />
  {/if}

  {#if newLoadout !== null}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("bar.newLoadout")}</div>
        <p class="dim">{t("bar.newLoadoutHint")}</p>
        <!-- svelte-ignore a11y_autofocus -->
        <input class="input" autofocus bind:value={newLoadout} onkeydown={(e) => e.key === "Enter" && makeLoadout()} />
        <div class="dlg-actions">
          <button class="btn ghost" onclick={() => (newLoadout = null)}>{t("tree.cancel")}</button>
          <button class="btn primary" disabled={!newLoadout.trim()} onclick={makeLoadout}>OK</button>
        </div>
      </div>
    </div>
  {/if}

  {#if saveAsOpen}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("bar.saveAsTitle")}</div>
        <p class="dim">{t("bar.saveAsBody")}</p>
        <div class="folders">
          <div class="fpath dim">{app.buildPath}{saveAsDir}</div>
          <div class="flist">
            {#if saveAsDir}
              <button class="frow" onclick={() => loadFolders(saveAsDir.replace(/[^/]+[/]$/, ""))}>↑ ..</button>
            {/if}
            {#each saveAsFolders as f}
              <button class="frow" onclick={() => loadFolders(saveAsDir + f + "/")}>📁 {f}</button>
            {/each}
          </div>
          {#if saveAsNewFolder !== null}
            <span class="fnew">
              <input class="input" placeholder={t("builds.newFolder")} bind:value={saveAsNewFolder} onkeydown={(e) => e.key === "Enter" && makeFolder()} />
              <button class="btn sm" onclick={makeFolder} disabled={!saveAsNewFolder.trim()}>OK</button>
            </span>
          {:else}
            <button class="btn ghost sm" onclick={() => (saveAsNewFolder = "")}>{t("builds.newFolder")}</button>
          {/if}
        </div>
        <input class="input" bind:value={saveAsName} onkeydown={(e) => e.key === "Enter" && saveAs()} />
        <div class="dlg-actions">
          <button class="btn ghost" onclick={() => { saveAsOpen = false; app.cancelSaveAs(); }}>{t("tree.cancel")}</button>
          <button class="btn primary" onclick={saveAs} disabled={!saveAsName.trim()}>{t("bar.save")}</button>
        </div>
      </div>
    </div>
  {/if}
{/if}

<style>
  .bar {
    display: flex;
    align-items: center;
    gap: 14px;
    height: 34px;
    padding: 0 16px;
    background: var(--surface-0);
    border-bottom: 1px solid var(--edge-0);
    font-size: var(--fs-sm);
  }
  .field {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .field.grow {
    flex: 1;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
    white-space: nowrap;
  }
  .lvl {
    width: 58px;
    height: 24px;
  }
  .cnt {
    width: 56px;
    height: 24px;
  }
  .bar :global(.select.sm) {
    height: 24px;
    max-width: 260px;
  }
  .bar :global(.select.cls) {
    max-width: 130px;
  }
  .btn.on {
    color: var(--gold);
  }
  .actions {
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .savedat {
    font-size: var(--fs-2xs);
    margin-right: 6px;
  }
  .modal {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: var(--backdrop);
    z-index: 30;
  }
  .dialog {
    width: 420px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .dialog p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .dlg-actions {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
  }
  .folders {
    display: flex;
    flex-direction: column;
    gap: 6px;
    margin-bottom: 8px;
  }
  .fpath {
    font-size: var(--fs-xs);
    word-break: break-all;
  }
  .flist {
    max-height: 180px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .frow {
    display: block;
    width: 100%;
    text-align: left;
    appearance: none;
    border: 0;
    background: none;
    color: var(--ink-1);
    padding: 3px 8px;
  }
  .frow:hover {
    background: var(--surface-2);
  }
  .fnew {
    display: flex;
    gap: 6px;
  }
</style>
