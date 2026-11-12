<!-- 物品頁:左 = 裝備欄格(POB 的 slot 佈局,含武器組切換、藥劑啟用、珠寶插槽;可接受拖放),
     中 = 所有物品(ItemListControl 的 loadout 篩選 / 排序 / 刪未使用 / 全刪、(未使用) 標記、
     雙擊編輯、Ctrl+點擊裝到主欄位、拖曳),右 = 貼上新增 / 編輯面板 / 傳奇與稀有資料庫。
     每個動作都是 POB 自己的函式或控制項回呼;hover tooltip 預設帶「替換後差異」。 -->
<script lang="ts" src="$lib/views/ItemsView.ts"></script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- any click or scroll dismisses a hover card: the row under it may be about to go away or move -->
<div class="page" onmouseleave={() => craftListActive || hideTip()} onpointerdown={hideTip} onwheel={hideTip}>
  <div class="toolbar">
    {#if data}
      <span class="tlabel">{t("items.itemSet")}</span>
      <Select class="select sm iv" value={String(data.activeItemSetId)} options={data.itemSets.map((s) => ({ value: String(s.id), label: setTitle(s) }))} onchange={(v) => setChange(String(v))} title={t("items.itemSet")} />
      <button class="btn ghost sm ico" title={t("items.newSet")} aria-label={t("items.newSet")} onclick={() => (setDialog = { mode: "new", title: t("items.newSetName"), copy: false })}>+</button>
      <button class="btn ghost sm ico" title={t("builds.rename")} aria-label={t("builds.rename")} onclick={() => (setDialog = { mode: "rename", title: data!.itemSets.find((s) => s.id === data!.activeItemSetId)?.title ?? "", copy: false })}>✎</button>
      <button class="btn ghost sm ico" title={t("items.deleteSet")} aria-label={t("items.deleteSet")} disabled={data.itemSets.length <= 1} onclick={deleteSet}>×</button>
      <button class="btn ghost sm ico" title={t("items.shareSet")} aria-label={t("items.shareSet")} onclick={shareCurrentSet}>⇪</button>
      <span class="vsep"></span>
      <TreeSpecBar compact />
    {/if}
    <span class="grow"></span>
    <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={() => (sharedOpen = true)}>{t("items.shared")}</button>
    {#if app.has("tradeQuery")}
      <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={() => (tradeOpen = true)}>{t("trade.open")}</button>
    {/if}
    <button class="btn sm" class:on={rightTab === "craft"} disabled={!data} onclick={openCraft}>{t("items.craft")}</button>
    <button class="btn sm accent" onclick={() => (rightTab = "paste")}>{t("items.paste")}</button>
  </div>

  <div class="cols">
    <!-- 左:裝備欄 -->
    <section class="col slots">
      <div class="chead"><span class="card-title">{t("items.equipped")}</span></div>
      <div class="scroll">
        {#each slotGroups as g (g.key)}
          <div class="group">
            {#if g.key === "weapons"}
              <div class="gtitle">
                <span class="card-title">{groupTitle(g.key)}</span>
                <span class="seg">
                  <button class:on={!data?.useSecondWeaponSet} onclick={() => weaponSwap(false)}>I</button>
                  <button class:on={data?.useSecondWeaponSet} onclick={() => weaponSwap(true)}>II</button>
                </span>
              </div>
            {:else if groupTitle(g.key)}
              <div class="gtitle"><span class="card-title">{groupTitle(g.key)}</span></div>
            {/if}
            {#each g.slots as s (s.name)}
              {@const it = data ? itemById(data.items, s.selItemId) : undefined}
              <div class="slot" class:sub={!!s.parent} class:drop={canDrop(s)} class:flash={flashSlot === s.name} data-slot={s.name} ondragover={(e) => dragOver(e, s)} ondrop={(e) => drop(e, s)}>
                <span class="sl" title={s.name}>{s.nodeId != null ? t("items.socketN", { n: s.socketIndex ?? "" }) : s.labelZh || s.label}</span>
                <!-- each entry previews against what the slot holds (ItemSlotControl) -->
                <SlotSelect
                  value={s.selItemId}
                  label={it ? it.nameZh || it.name : t("items.none")}
                  color={it ? rarityColor(it.rarity) : "var(--ink-3)"}
                  options={s.valid.flatMap((id) => {
                    const v = data ? itemById(data.items, id) : undefined;
                    return v ? [{ id, label: v.nameZh || v.name, color: rarityColor(v.rarity) }] : [];
                  })}
                  onpick={(id) => slotChange(s, String(id))}
                  ontip={(id, e) => tipItem(id, s.name, e)}
                  onleave={hideTip}
                />
                {#if s.isFlask}
                  <input type="checkbox" class="sw" title={t("items.flaskActive")} aria-label={t("items.flaskActive")} checked={s.active} disabled={s.selItemId === 0} onchange={() => toggleFlask(s)} />
                {/if}
              </div>
            {/each}
          </div>
        {/each}
      </div>
    </section>

    <!-- 中:所有物品 -->
    <section class="col list">
      <div class="chead">
        <span class="card-title">{t("items.list")}</span>
        {#if data}<span class="num dim small">{t("items.count", { n: listItems.length })}</span>{/if}
        <span class="grow"></span>
        <label class="chk" title={t("items.compare")}>{t("items.compare")} <input type="checkbox" class="sw" checked={compare} onchange={(e) => setCompare(e.currentTarget.checked)} /></label>
      </div>
      <div class="tools">
        {#if data}
          <Select
            class="select sm iv"
            value={loadout}
            options={[
              { value: "any", label: t("items.loadoutAny") },
              { value: "current", label: t("items.loadoutCurrent") },
              { value: "unused", label: t("items.loadoutUnused") },
              ...data.itemSets.map((s) => ({ value: String(s.id), label: setTitle(s) })),
            ]}
            onchange={(v) => (loadout = String(v))}
          />
        {/if}
        <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={sortItems}>{t("items.sort")}</button>
      </div>
      <div class="scroll">
        {#if data}
          {#each listItems as it (it.id)}
            {@const where = equippedIn(data.slots, it.id!)}
            {@const badge = usedInBadge(it.usedIn)}
            <!-- svelte-ignore a11y_click_events_have_key_events -->
            <div
              class="row"
              class:sel={selectedId === it.id}
              role="option"
              aria-selected={selectedId === it.id}
              tabindex="0"
              draggable="true"
              ondragstart={(e) => dragStart(e, it.id!)}
              ondragend={() => (dragId = null)}
              onclick={(e) => rowClick(e, it)}
              ondblclick={() => openEdit({ id: it.id! })}
              onmouseenter={(e) => tipItem(it.id!, where[0]?.name, e)}
              onmouseleave={hideTip}
            >
              <span class="nm" style:color={rarityColor(it.rarity)}>{it.nameZh || it.name}</span>
              {#if badge?.kind === "unused"}<span class="where unused">{t("items.unused")}</span>
              {:else if badge?.kind === "elsewhere"}<span class="where" title={t("items.usedIn", { where: badge.where })}>{t("items.usedIn", { where: badge.where })}</span>
              {:else if where.length}{@const ws = where.map((s) => s.labelZh || s.label).join(", ")}<span class="where" title={ws}>{ws}</span>{/if}
              {#if it.unsupported}<span class="bad small">!</span>{/if}
            </div>
          {/each}
        {/if}
      </div>
      <p class="dim small hint">{t("items.listHint")}</p>
      {#if selected && data}
        <div class="actions">
          <button class="btn sm" onclick={() => openEdit({ id: selected!.id! })}>{t("items.edit")}</button>
          <!-- a menu: picking a slot equips, the control goes back to its prompt -->
          <Select class="select sm iv" action placeholder={t("items.equipTo")} options={slotsFor(data.slots, selected.id!).map((s) => ({ value: s.name, label: s.labelZh || s.label }))} onchange={(v) => void equipTo(selected!.id!, String(v))} />
          <button class="btn sm" disabled={!equippedIn(data.slots, selected.id!).length} onclick={() => unequipAll(selected!.id!)}>{t("items.unequip")}</button>
          <button class="btn sm" class:ok={copyState === "ok"} class:danger={copyState === "fail"} onclick={() => copyItem(selected!.id!)}>
            {copyState === "ok" ? t("items.copied") : copyState === "fail" ? t("items.copyFailed") : t("items.copy")}
          </button>
          <button class="btn sm" onclick={() => shareItem(selected!.id!)}>{t("items.shareThis")}</button>
          {#if app.has("buySimilar")}
            <button class="btn sm" title={t("items.buySimilarHint")} onclick={() => buySimilar(selected!.id!)}>{t("items.buySimilar")}</button>
          {/if}
          <button class="btn sm danger" onclick={() => deleteItem(selected!.id!)}>{t("items.delete")}</button>
        </div>
      {/if}
      <div class="foot">
        <button class="btn sm" disabled={!data?.items.some((i) => !i.usedIn) || app.busy > 0} onclick={deleteUnused}>{t("items.delUnused")}</button>
        <button class="btn sm danger" disabled={!data?.items.length || app.busy > 0} onclick={deleteAll}>{t("items.delAll")}</button>
      </div>
    </section>

    <!-- 右:新增 / 編輯 / 資料庫 -->
    <section class="col right">
      <div class="rhead">
        <div class="tabs">
          <button class:on={rightTab === "paste"} onclick={() => (rightTab = "paste")}>{t("items.paste")}</button>
          {#if edit}<button class:on={rightTab === "edit"} onclick={() => (rightTab = "edit")}>{t("edit.title")}</button>{/if}
          <button class:on={rightTab === "db"} onclick={() => (rightTab = "db")}>{t("items.db")}</button>
          <button class:on={rightTab === "craft"} disabled={!data} onclick={openCraft}>{t("items.craft")}</button>
        </div>
      </div>
      {#if rightTab === "edit" && edit}
        {#if pasteNote}<div class="ok edit-note">{pasteNote}</div>{/if}
        <ItemEditor item={edit} report={editReport} onchange={(s) => (edit = s)} onclose={editClosed} ondone={editDone} />
      {:else if rightTab === "paste"}
        <div class="pane">
          <p class="dim">{t("items.pasteHint")}</p>
          <textarea class="input area" rows="14" bind:value={pasteText} onpaste={onPasteBox} placeholder="Rarity: RARE&#10;…"></textarea>
          {#if pasteErr}<div class="bad selectable">{pasteErr}</div>{/if}
          {#if pasteNote}<div class="ok">{pasteNote}</div>{/if}
          <div class="btns">
            <button class="btn primary" disabled={!pasteText.trim() || app.busy > 0} onclick={() => addPasted(true)}>{t("items.addEquip")}</button>
            <button class="btn" disabled={!pasteText.trim() || app.busy > 0} onclick={() => addPasted(false)}>{t("items.add")}</button>
            <button class="btn ghost" disabled={!pasteText.trim() || app.busy > 0} onclick={previewPasted} onmouseleave={() => (tip = null)}>{t("items.preview")}</button>
            <button class="btn ghost" disabled={!pasteText.trim() || app.busy > 0} onclick={() => openEdit({ raw: pasteText.trim() })}>{t("items.edit")}</button>
          </div>
        </div>
      {:else if rightTab === "craft"}
        <div class="pane craft">
          <p class="dim">{t("items.craftTitle")}</p>
          {#if craft}
            <div class="crow">
              <span class="k">{t("items.craftRarity")}</span>
              <Select class="select" ariaLabel={t("items.craftRarity")} value={craft.rarity} options={craft.opts.rarities.map((r, i) => ({ value: i + 1, label: r.labelZh || r.label }))} onchange={(v) => (craft!.rarity = Number(v))} />
            </div>
            {#if craft.rarity >= 3}
              <div class="crow">
                <span class="k">{t("items.craftName")}</span>
                <input class="input" bind:value={craft.title} />
              </div>
            {/if}
            <div class="crow">
              <span class="k">{t("items.craftType")}</span>
              <Select class="select" ariaLabel={t("items.craftType")} value={craft.type} options={craft.opts.types.map((ty, i) => ({ value: i + 1, label: ty.typeZh || ty.type }))} onchange={(v) => { craft!.type = Number(v); craft!.base = 1; }} />
            </div>
            <div class="crow">
              <span class="k">{t("items.craftBase")}</span>
              <Select class="select" ariaLabel={t("items.craftBase")} value={craft.base} options={(craft.opts.types[craft.type - 1]?.bases ?? []).map((b, i) => ({ value: i + 1, label: b.labelZh || b.label }))} onchange={(v) => (craft!.base = Number(v))} onactive={craftBaseActive} />
            </div>
            <div class="btns">
              <button class="btn primary" disabled={app.busy > 0} onclick={craftCreate}>{t("items.craftCreate")}</button>
            </div>
          {:else}
            <p class="dim pulse">{t("app.booting")}</p>
          {/if}
        </div>
      {:else}
        <ItemDb onadd={addFromDb} onedit={(it) => openEdit({ raw: it.raw! })} ontip={tipRaw} onleave={hideTip} />
      {/if}
    </section>
  </div>

  {#if tip}
    <TooltipCard lines={tip.lines} accent={tip.color} x={tip.x} y={tip.y} avoid={tip.avoid ?? null} />
  {/if}

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{setDialog.mode === "new" ? t("items.newSet") : t("items.itemSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}
          <label class="chk"><input type="checkbox" class="sw" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>
        {/if}
        <div class="btns right-align">
          <button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button>
          <button class="btn primary" disabled={!setDialog.title.trim()} onclick={setDialogOk}>OK</button>
        </div>
      </div>
    </div>
  {/if}

</div>

{#if tradeOpen}
  <TradeDialog onclose={() => (tradeOpen = false)} />
{/if}

{#if sharedOpen}
  <SharedItemsDialog onclose={() => (sharedOpen = false)} onedit={(raw) => openEdit({ raw })} />
{/if}

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .toolbar {
    flex: none;
  }
  .tlabel {
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
  }
  .toolbar :global(.select.iv) {
    min-width: 0;
    max-width: 200px;
  }
  .ico {
    min-width: 24px;
    justify-content: center;
  }
  .btn.accent {
    border-color: var(--gold);
    background: var(--gold-soft);
    color: var(--gold-text);
  }
  .btn.accent:hover:not(:disabled) {
    background: color-mix(in srgb, var(--gold) 26%, transparent);
    border-color: var(--gold);
    color: var(--gold-text);
  }
  .btn.on {
    border-color: var(--edge-2);
    color: var(--ink-0);
  }
  .cols {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: 316px minmax(260px, 1fr) minmax(360px, 1.2fr);
  }
  /* RWD on the main area's width (App's `view` container, sidebar excluded).
     Medium: equipment over the item list on the left, the add / edit / craft
     column keeps the full height on the right. Narrow: one column, each part
     with a usable height, the page scrolling as a whole. */
  @container view (max-width: 979px) {
    .cols {
      grid-template-columns: minmax(0, 1fr) minmax(0, 1.15fr);
      grid-template-rows: minmax(200px, 1fr) minmax(200px, 1fr);
      overflow-y: auto;
    }
    .slots {
      grid-area: 1 / 1;
    }
    .list {
      grid-area: 2 / 1;
      border-left: 0;
      border-top: 1px solid var(--edge-0);
    }
    .right {
      grid-area: 1 / 2 / 3 / 3;
    }
  }
  @container view (max-width: 759px) {
    .cols {
      grid-template-columns: minmax(0, 1fr);
      grid-template-rows: 360px 360px 600px;
    }
    .slots,
    .list,
    .right {
      grid-area: auto;
      border-left: 0;
    }
    .list,
    .right {
      border-top: 1px solid var(--edge-0);
    }
  }
  /* column rules as borders, not a solid fill behind a 1px gap: the fill hid the
     background image, and a second surface here stacked on the one .view paints */
  .col {
    display: flex;
    flex-direction: column;
    min-height: 0;
    min-width: 0;
  }
  .col + .col {
    border-left: 1px solid var(--edge-0);
  }
  .chead {
    flex: none;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 4px 8px;
    min-height: 40px;
    padding: 6px 14px;
  }
  .chead .card-title {
    white-space: nowrap;
  }
  .tools {
    flex: none;
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 0 10px 6px;
    flex-wrap: wrap;
  }
  .tools :global(.select.iv) {
    flex: 1;
    min-width: 0;
    max-width: 200px;
  }
  .grow {
    flex: 1;
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 0 8px 12px;
  }
  .group {
    margin-bottom: 10px;
  }
  .gtitle {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    margin: 6px 6px 4px;
  }
  .slot {
    display: grid;
    grid-template-columns: 72px minmax(0, 1fr) auto;
    align-items: center;
    gap: 8px;
    min-height: 38px;
    padding: 0 6px;
    border-radius: var(--radius-m);
  }
  .slot:hover {
    background: var(--surface-hover);
  }
  .slot.drop {
    outline: 1px dashed var(--gold);
    outline-offset: -1px;
  }
  .slot.sub {
    opacity: 0.85;
  }
  .slot.sub .sl {
    padding-left: 10px;
    color: var(--ink-3);
  }
  .sl {
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .page :global(.select.sm.iv) {
    height: 24px;
    font-size: var(--fs-xs);
  }
  /* the slot drop-down is SlotSelect's button: reach it across the component */
  .slot :global(.slotdd) {
    width: 100%;
    min-width: 0;
    height: 28px;
    font-size: var(--fs-xs);
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    min-height: 34px;
    padding: 0 10px;
    border-radius: var(--radius-m);
    font-size: var(--fs-sm);
    cursor: default;
  }
  .row:hover {
    background: var(--surface-hover);
  }
  .row.sel {
    background: var(--gold-soft);
  }
  .nm {
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  /* 用在哪裡:固定成右側一欄,方便往下掃 */
  .where {
    flex: none;
    width: 30%;
    max-width: 140px;
    text-align: right;
    font-size: var(--fs-2xs);
    color: var(--ink-3);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .where.unused {
    color: var(--warn);
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .hint {
    flex: none;
    margin: 0;
    padding: 4px 14px 6px;
  }
  .actions {
    flex: none;
    display: flex;
    gap: 6px;
    padding: 8px 10px;
    border-top: 1px solid var(--edge-0);
    flex-wrap: wrap;
  }
  .foot {
    flex: none;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 6px;
    padding: 8px 10px;
    border-top: 1px solid var(--edge-0);
  }
  .foot .btn {
    justify-content: center;
  }
  .btn.danger:hover:not(:disabled) {
    color: var(--bad);
    border-color: var(--bad);
  }
  .rhead {
    flex: none;
    padding: 4px 16px 0;
    border-bottom: 1px solid var(--edge-0);
  }
  .rhead .tabs {
    flex-wrap: wrap;
    row-gap: 0;
  }
  .rhead .tabs > button {
    font-size: var(--fs-sm);
  }
  .rhead .tabs > button:disabled {
    opacity: var(--dim-opacity);
  }
  .pane {
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 12px 16px;
    min-height: 0;
  }
  .pane p {
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
  .btns {
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
  }
  .right-align {
    justify-content: flex-end;
  }
  .ok {
    color: var(--ok);
    font-size: var(--fs-xs);
  }
  .edit-note {
    flex: none;
    padding: 6px 16px 0;
  }
  .bad {
    color: var(--bad);
    font-size: var(--fs-xs);
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
    width: 340px;
    padding: 14px 16px;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .slot.flash {
    outline: 2px solid var(--gold);
    outline-offset: -1px;
  }
  .pane.craft .crow + .crow {
    margin-top: 8px;
  }
  .crow {
    display: grid;
    grid-template-columns: 64px 1fr;
    align-items: center;
    gap: 8px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
</style>
