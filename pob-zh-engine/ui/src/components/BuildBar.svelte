<!-- 建置列(頂列第二列):等級、主技能選擇、儲存。全部走 POB 自己的
     頂列控制項回呼(get_build_header / set_build_field / save_build*)。 -->
<script lang="ts">
  import { api, type BuildHeader } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import ClassChangeDialog from "./ClassChangeDialog.svelte";

  const h = $derived(app.header);
  let levelDraft = $state<string>("");
  let saveAsOpen = $state(false);
  let saveAsName = $state("");

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
      saveAsName = h.buildName ?? "";
      saveAsOpen = true;
      return;
    }
    await app.save();
  }

  async function saveAs() {
    const name = saveAsName.trim();
    if (!name) return;
    saveAsOpen = false;
    await app.saveAs(name);
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
      <button class="btn ghost sm" disabled={app.busy > 0} onclick={() => { saveAsName = h.buildName ?? ""; saveAsOpen = true; }}>{t("bar.saveAs")}</button>
    </span>
  </div>

  {#if classConfirm}
    <ClassChangeDialog className={classConfirm.className} connectFailed={classConfirm.connectFailed} onanswer={answerClass} oncancel={cancelClass} />
  {/if}

  {#if saveAsOpen}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("bar.saveAsTitle")}</div>
        <p class="dim">{t("bar.saveAsBody")}</p>
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
</style>
