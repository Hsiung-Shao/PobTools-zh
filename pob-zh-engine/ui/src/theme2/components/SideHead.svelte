<!-- 側欄頂端:職業/昇華、等級與點數、主要技能選擇。全部走 POB 自己的
     頂列控制項回呼(get_build_header / set_build_field / set_class*)。 -->
<script lang="ts">
  import { api, type DdEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import type { SelOption } from "$lib/select";
  import { app } from "$lib/state.svelte";
  import ClassChangeDialog from "./ClassChangeDialog.svelte";
  import Select from "$lib/ui/Select.svelte";
  import MinionLibrary from "./MinionLibrary.svelte";

  const h = $derived(app.header);
  const info = $derived(app.info);
  let levelDraft = $state<string>("");

  // POB's spectre / beast library (Build.lua's Manage Spectres...)
  let library = $state<"spectre" | "beast" | null>(null);

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

  const overCap = (used: number, max?: number | null) => max != null && used > max;
  const selectClass = "select sm full";
</script>

{#if h && info}
  {#if pick && h.classes}
    <div class="blk cls">
      <Select class={selectClass} ariaLabel={t("bar.class")} title={t("bar.class")} disabled={app.busy > 0} value={pick.classId} options={h.classes.map((c) => ({ value: c.id, label: c.nameZh || c.name }))} onchange={(v) => void changeClass(Number(v))} />
      {#if currentClass}
        <Select class={selectClass} color="var(--hl)" ariaLabel={t("bar.ascend")} title={t("bar.ascend")} disabled={app.busy > 0} value={pick.ascendClassId} options={currentClass.ascendancies.map((a) => ({ value: a.id, label: a.nameZh || a.name }))} onchange={(v) => void setAscendancy(Number(v))} />
      {/if}
      {#if h.secondaryAscendancies && h.secondaryAscendancies.length > 1}
        <span class="wide">
          <Select class={selectClass} ariaLabel={t("bar.ascend2")} disabled={app.busy > 0} title={t("bar.ascend2")} value={pick.secondaryAscendClassId} options={h.secondaryAscendancies.map((a) => ({ value: a.id, label: a.nameZh || a.name }))} onchange={(v) => void setSecondary(Number(v))} />
        </span>
      {/if}
    </div>
  {/if}

  <dl class="blk facts">
    <div>
      <dt>{t("bar.level")}</dt>
      <dd class="lvl-row">
        <input
          class="input num lvl"
          type="number"
          min="1"
          max="100"
          aria-label={t("bar.level")}
          bind:value={levelDraft}
          onblur={commitLevel}
          onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()}
        />
        <button class="auto" class:on={h.levelAuto} title={t("bar.levelAutoTip")} onclick={() => setField("levelAuto", !h.levelAuto)}>
          {h.levelAuto ? t("bar.levelAuto") : t("bar.levelManual")}
        </button>
      </dd>
    </div>
    <div title={info.points.req ?? ""}>
      <dt>{t("sidebar.points")}</dt>
      <dd class="num">
        <b class:over={overCap(info.points.used, info.points.usedMax)}>{info.points.used}</b><span class="dim">/{info.points.usedMax ?? "?"}</span>
      </dd>
    </div>
    <div>
      <dt>{t("sidebar.asc")}</dt>
      <dd class="num">
        <b class:over={overCap(info.points.ascUsed, info.points.ascMax)}>{info.points.ascUsed}</b><span class="dim">/{info.points.ascMax ?? "?"}</span>
      </dd>
    </div>
    {#if info.points.weaponSets?.length}
      <!-- PoE2: POB's point display has the two weapon sets between them -->
      <div class="ws" title={t("sidebar.weaponSetsHint")}>
        <dt>{t("sidebar.weaponSets")}</dt>
        <dd class="num">
          {#each info.points.weaponSets as w, i}
            {#if i}<span class="dim"> · </span>{/if}<b class:over={overCap(w.used, w.max)}>{w.used}</b><span class="dim">/{w.max}</span>
          {/each}
        </dd>
      </div>
    {/if}
  </dl>

  <div class="blk skill">
    <div class="label">{t("bar.mainSkill")}</div>
    <Select class={selectClass} ariaLabel={t("bar.mainSkill")} value={h.mainSocketGroup.index} options={dd(h.mainSocketGroup.list)} onchange={(v) => setField("mainSocketGroup", Number(v))} />
    <div class="more">
      {#if h.mainSkill}
        <Select class={selectClass} value={h.mainSkill.index} disabled={h.mainSkill.enabled === false} options={dd(h.mainSkill.list)} onchange={(v) => setField("mainSkill", Number(v))} />
      {/if}
      {#if h.mainSkillPart}
        <Select class={selectClass} value={h.mainSkillPart.index} options={dd(h.mainSkillPart.list)} onchange={(v) => setField("mainSkillPart", Number(v))} />
      {/if}
      {#if h.mainSkillStageCount != null}
        <span class="numf">
          <span class="k">{t("bar.stages")}</span>
          <input class="input num cnt" type="number" min="1" aria-label={t("bar.stages")} value={h.mainSkillStageCount} onchange={(e) => setField("mainSkillStageCount", Number(e.currentTarget.value))} />
        </span>
      {/if}
      {#if h.mainSkillMineCount != null}
        <span class="numf">
          <span class="k">{t("bar.mines")}</span>
          <input class="input num cnt" type="number" min="0" aria-label={t("bar.mines")} value={h.mainSkillMineCount} onchange={(e) => setField("mainSkillMineCount", Number(e.currentTarget.value))} />
        </span>
      {/if}
      {#if h.mainSkillMinion}
        <Select class={selectClass} value={h.mainSkillMinion.index} disabled={h.mainSkillMinion.enabled === false} options={dd(h.mainSkillMinion.list, true)} onchange={(v) => setField("mainSkillMinion", Number(v))} />
      {/if}
      {#if h.statSet}
        <Select class={selectClass} value={h.statSet.index} disabled={h.statSet.enabled === false} options={dd(h.statSet.list)} onchange={(v) => setField("statSet", Number(v))} title={t("bar.statSet")} />
      {/if}
      {#if h.minionLibrary}
        <button class="btn sm" onclick={() => (library = "spectre")}>{t("bar.spectres")}</button>
      {/if}
      {#if h.beastLibrary}
        <button class="btn sm" onclick={() => (library = "beast")}>{t("bar.beasts")}</button>
      {/if}
      {#if h.minionStatSet}
        <Select class={selectClass} value={h.minionStatSet.index} options={dd(h.minionStatSet.list)} onchange={(v) => setField("minionStatSet", Number(v))} title={t("bar.statSet")} />
      {/if}
      {#if h.mainSkillMinionSkill}
        <Select class={selectClass} value={h.mainSkillMinionSkill.index} options={dd(h.mainSkillMinionSkill.list)} onchange={(v) => setField("mainSkillMinionSkill", Number(v))} />
      {/if}
    </div>
  </div>

  {#if classConfirm}
    <ClassChangeDialog className={classConfirm.className} connectFailed={classConfirm.connectFailed} onanswer={answerClass} oncancel={cancelClass} />
  {/if}

  {#if library}
    <MinionLibrary kind={library} onclose={() => (library = null)} />
  {/if}
{/if}

<style>
  .blk {
    padding: 10px 16px;
    border-bottom: 1px solid var(--edge-0);
  }
  .blk :global(.select.full) {
    width: 100%;
    max-width: 100%;
    height: 28px;
  }
  .cls {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 6px;
  }
  .cls .wide {
    grid-column: 1 / -1;
    min-width: 0;
  }
  .facts {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 6px;
    margin: 0;
  }
  .facts .ws {
    grid-column: 1 / -1;
  }
  .facts div {
    min-width: 0;
    padding: 6px 8px;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    background: var(--surface-2);
  }
  .facts dt {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .facts dd {
    margin: 2px 0 0;
    font-size: var(--fs-md);
    color: var(--ink-0);
  }
  .facts b {
    font-weight: 600;
  }
  .over {
    color: var(--bad);
  }
  .lvl-row {
    display: flex;
    align-items: center;
    gap: 4px;
  }
  /* the level reads like the two numbers beside it: no box until it is focused */
  .lvl {
    width: 100%;
    min-width: 0;
    height: 20px;
    padding: 0 2px;
    border-color: transparent;
    background: transparent;
    font-size: var(--fs-md);
    font-weight: 600;
    -moz-appearance: textfield;
    appearance: textfield;
  }
  .lvl::-webkit-inner-spin-button,
  .lvl::-webkit-outer-spin-button {
    -webkit-appearance: none;
    margin: 0;
  }
  .lvl:hover {
    border-color: var(--edge-1);
  }
  .auto {
    flex: none;
    appearance: none;
    height: 18px;
    padding: 0 4px;
    border: 1px solid var(--edge-1);
    border-radius: 3px;
    background: none;
    color: var(--ink-2);
    font-size: var(--fs-2xs);
    cursor: pointer;
  }
  .auto.on {
    color: var(--gold-text);
    border-color: var(--gold);
  }
  .skill {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  /* two to a row; what does not fit wraps */
  .more {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }
  .more:empty {
    display: none;
  }
  .more > :global(*) {
    flex: 1 1 calc(50% - 3px);
    min-width: 0;
  }
  .numf {
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
    white-space: nowrap;
  }
  .cnt {
    flex: 1;
    width: 0;
    min-width: 40px;
    height: 28px;
  }
</style>
