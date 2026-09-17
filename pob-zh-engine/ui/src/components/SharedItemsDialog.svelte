<!-- POB 的共用物品與共用裝備組(main.sharedItemList / sharedItemSetList):
     所有建置共用,原版是用拖曳加入,這裡用按鈕。 -->
<script lang="ts">
  import { onMount } from "svelte";
  import { api, type SharedItems } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  let { onclose, onedit }: { onclose: () => void; onedit?: (raw: string) => void } = $props();

  let data = $state<SharedItems | null>(null);
  let err = $state<string | null>(null);

  async function reload() {
    try {
      data = await api.sharedItems();
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  }
  onMount(reload);

  async function drop(kind: "item" | "set", index: number) {
    const r = await app.run(() => api.unshare(kind, index));
    if (r) data = r;
  }
  async function useSet(index: number) {
    const r = await app.run(() => api.useSharedSet(index));
    if (r) {
      await app.refresh();
      onclose();
    }
  }
  const slotNames = (s: SharedItems["sets"][number]) =>
    Object.values(s.slots)
      .map((it) => it.nameZh || it.name)
      .join("、");
</script>

<div class="modal">
  <div class="dialog">
    <div class="label">{t("items.sharedTitle")}</div>
    {#if err}<p class="bad">{err}</p>{/if}
    <p class="dim small">{t("items.sharedHint")}</p>
    {#if data}
      <div class="cols">
        <div class="col">
          <div class="chead">{t("items.sharedItems")} <span class="dim num">{data.items.length}</span></div>
          <div class="list">
            {#each data.items as it (it.index)}
              <div class="row">
                <span class="nm" class:unique={it.rarity === "UNIQUE"} class:rare={it.rarity === "RARE"}>{it.nameZh || it.name}</span>
                {#if onedit}<button class="btn ghost sm" onclick={() => { onedit?.(it.raw); onclose(); }}>{t("items.edit")}</button>{/if}
                <button class="btn ghost sm danger" onclick={() => drop("item", it.index)}>×</button>
              </div>
            {/each}
          </div>
        </div>
        <div class="col">
          <div class="chead">{t("items.sharedSets")} <span class="dim num">{data.sets.length}</span></div>
          <div class="list">
            {#each data.sets as s (s.index)}
              <div class="row">
                <span class="nm" title={slotNames(s)}>{s.title || t("tree.specDefault")}</span>
                <button class="btn ghost sm" onclick={() => useSet(s.index)}>{t("items.useSet")}</button>
                <button class="btn ghost sm danger" onclick={() => drop("set", s.index)}>×</button>
              </div>
            {/each}
          </div>
        </div>
      </div>
    {/if}
    <div class="dlg-actions">
      <button class="btn primary" onclick={onclose}>{t("tree.done")}</button>
    </div>
  </div>
</div>

<style>
  .modal {
    position: fixed;
    inset: 0;
    z-index: 220;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 70%, transparent);
  }
  .dialog {
    width: min(820px, calc(100vw - 32px));
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: 8px;
  }
  .cols {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
  }
  .chead {
    margin-bottom: 6px;
  }
  .list {
    height: 300px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 2px 8px;
  }
  .row:hover {
    background: var(--surface-2);
  }
  .nm {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .nm.unique {
    color: var(--c-unique, var(--gold));
  }
  .nm.rare {
    color: var(--c-rare, var(--warn));
  }
</style>
