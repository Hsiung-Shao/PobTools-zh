<!-- 建置列(頂列第二列):等級、主技能選擇、儲存。全部走 POB 自己的
     頂列控制項回呼(get_build_header / set_build_field / save_build*)。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type BuildHeader } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import ClassChangeDialog from "./ClassChangeDialog.svelte";
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
  let classSel = $state<HTMLSelectElement | null>(null);
  let ascSel = $state<HTMLSelectElement | null>(null);
  let asc2Sel = $state<HTMLSelectElement | null>(null);
  let classConfirm = $state<{ classId: number; className: string; connectFailed: boolean } | null>(null);

  $effect(() => {
    if (h) levelDraft = String(h.level);
  });
  // native selects keep whatever the user picked; put POB's answer back after every header refresh
  $effect(() => {
    if (!pick) return;
    if (classSel) classSel.value = String(pick.classId);
    if (ascSel) ascSel.value = String(pick.ascendClassId);
    if (asc2Sel) asc2Sel.value = String(pick.secondaryAscendClassId);
  });

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
    if (!h.dbFileName) {
      void openSaveAs();
      return;
    }
    await app.save();
  }

  async function saveAs() {
    const name = saveAsName.trim();
    if (!name) return;
    saveAsOpen = false;
    await app.saveAs(saveAsDir + name);
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
        <select class="{selectClass} cls" bind:this={classSel} disabled={app.busy > 0} onchange={(e) => void changeClass(Number(e.currentTarget.value))}>
          {#each h.classes as c (c.id)}
            <option value={c.id} selected={c.id === pick.classId}>{c.nameZh || c.name}</option>
          {/each}
        </select>
        {#if currentClass}
          <select class="{selectClass} cls" bind:this={ascSel} disabled={app.busy > 0} onchange={(e) => void setAscendancy(Number(e.currentTarget.value))}>
            {#each currentClass.ascendancies as a (a.id)}
              <option value={a.id} selected={a.id === pick.ascendClassId}>{a.nameZh || a.name}</option>
            {/each}
          </select>
        {/if}
        {#if h.secondaryAscendancies && h.secondaryAscendancies.length > 1}
          <select class="{selectClass} cls" bind:this={asc2Sel} disabled={app.busy > 0} title={t("bar.ascend2")} onchange={(e) => void setSecondary(Number(e.currentTarget.value))}>
            {#each h.secondaryAscendancies as a (a.id)}
              <option value={a.id} selected={a.id === pick.secondaryAscendClassId}>{a.nameZh || a.name}</option>
            {/each}
          </select>
        {/if}
      </label>
    {/if}

    <label class="field grow">
      <span class="k">{t("bar.mainSkill")}</span>
      <select class={selectClass} value={h.mainSocketGroup.index} onchange={(e) => setField("mainSocketGroup", Number(e.currentTarget.value))}>
        {#each h.mainSocketGroup.list as o}
          <option value={o.val}>{o.labelZh || o.label}</option>
        {/each}
      </select>
      {#if loadouts && loadouts.entries.length > 1}
        <select class={selectClass} value={String(loadouts.selIndex)} title={t("bar.loadout")} onchange={(e) => pickLoadout(e.currentTarget.value)}>
          {#each loadouts.entries as e (e.index)}
            <option value={String(e.index)} disabled={e.kind === "header"}>{e.kind === "loadout" ? e.label : e.labelZh || e.label}</option>
          {/each}
        </select>
      {/if}
      {#if h.mainSkill}
        <select class={selectClass} value={h.mainSkill.index} disabled={h.mainSkill.enabled === false} onchange={(e) => setField("mainSkill", Number(e.currentTarget.value))}>
          {#each h.mainSkill.list as o}
            <option value={o.val}>{o.labelZh || o.label}</option>
          {/each}
        </select>
      {/if}
      {#if h.mainSkillPart}
        <select class={selectClass} value={h.mainSkillPart.index} onchange={(e) => setField("mainSkillPart", Number(e.currentTarget.value))}>
          {#each h.mainSkillPart.list as o}
            <option value={o.val}>{o.labelZh || o.label}</option>
          {/each}
        </select>
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
        <select class={selectClass} value={h.mainSkillMinion.index} disabled={h.mainSkillMinion.enabled === false} onchange={(e) => setField("mainSkillMinion", Number(e.currentTarget.value))}>
          {#each h.mainSkillMinion.list as o, i}
            <option value={i + 1}>{o.labelZh || o.label}</option>
          {/each}
        </select>
      {/if}
      {#if h.statSet}
        <select class={selectClass} value={h.statSet.index} disabled={h.statSet.enabled === false} onchange={(e) => setField("statSet", Number(e.currentTarget.value))} title={t("bar.statSet")}>
          {#each h.statSet.list as o}
            <option value={o.val}>{o.labelZh || o.label}</option>
          {/each}
        </select>
      {/if}
      {#if h.minionLibrary}
        <button class="btn ghost sm" onclick={() => (library = "spectre")}>{t("bar.spectres")}</button>
      {/if}
      {#if h.beastLibrary}
        <button class="btn ghost sm" onclick={() => (library = "beast")}>{t("bar.beasts")}</button>
      {/if}
      {#if h.minionStatSet}
        <select class={selectClass} value={h.minionStatSet.index} onchange={(e) => setField("minionStatSet", Number(e.currentTarget.value))} title={t("bar.statSet")}>
          {#each h.minionStatSet.list as o}
            <option value={o.val}>{o.labelZh || o.label}</option>
          {/each}
        </select>
      {/if}
      {#if h.mainSkillMinionSkill}
        <select class={selectClass} value={h.mainSkillMinionSkill.index} onchange={(e) => setField("mainSkillMinionSkill", Number(e.currentTarget.value))}>
          {#each h.mainSkillMinionSkill.list as o}
            <option value={o.val}>{o.labelZh || o.label}</option>
          {/each}
        </select>
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
          <button class="btn ghost" onclick={() => (saveAsOpen = false)}>{t("tree.cancel")}</button>
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
  .select.sm {
    height: 24px;
    max-width: 260px;
    text-overflow: ellipsis;
  }
  .select.cls {
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
