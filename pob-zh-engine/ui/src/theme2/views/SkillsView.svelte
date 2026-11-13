<!-- 技能頁:左 = 插槽組清單(啟用、標籤、槽位、主技能標記),右 = 選中組的寶石表
     (名稱搜尋、等級/品質/啟用/全域/數量)。每個改動都經 POB 的 ProcessSocketGroup。 -->
<script lang="ts" src="$lib/views/SkillsView.ts"></script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="page" onmouseleave={hideTip}>
  <div class="toolbar">
    {#if data}
      <span class="tlabel">{t("skills.skillSet")}</span>
      <Select class="select sm setsel" value={String(data.activeSkillSetId)} options={data.skillSets.map((s) => ({ value: String(s.id), label: setTitle(s) }))} onchange={(v) => setChange(String(v))} title={t("skills.skillSet")} />
      <button class="btn ghost sm ico" title={t("items.newSet")} aria-label={t("items.newSet")} onclick={() => (setDialog = { mode: "new", title: t("skills.newSetName"), copy: false })}>+</button>
      <button class="btn ghost sm ico" title={t("builds.rename")} aria-label={t("builds.rename")} onclick={() => (setDialog = { mode: "rename", title: data!.skillSets.find((s) => s.id === data!.activeSkillSetId)?.title ?? "", copy: false })}>✎</button>
      <button class="btn ghost sm ico" title={t("items.deleteSet")} aria-label={t("items.deleteSet")} disabled={data.skillSets.length <= 1} onclick={deleteSet}>×</button>
    {/if}
    <span class="grow"></span>
    <button class="btn ghost sm" onclick={openPaste}>{t("skills.pasteGroup")}</button>
    <button class="btn ghost sm" disabled={!group} title={t("skills.copyGroupHint")} onclick={copyGroup}>{t("skills.copyGroup")}</button>
    <button class="btn sm danger" disabled={!group || group.source} onclick={deleteGroup}>{t("skills.deleteGroup")}</button>
    <button class="btn sm accent" onclick={addGroup}>{t("skills.newGroup")}</button>
  </div>

  <div class="cols">
    <section class="col left">
      <div class="chead"><span class="card-title">{t("skills.groups")}</span></div>
      <div class="scroll">
        {#if data}
          {#each data.groups as g (g.index)}
            {@const slotLabel = g.slot ? (data.slotOptions.find((s) => s.name === g.slot)?.labelZh ?? g.slot) : ""}
            <!-- svelte-ignore a11y_click_events_have_key_events -->
            <div
              class="grp"
              class:sel={g.index === sel}
              class:off={!g.enabled || !g.slotEnabled}
              role="option"
              aria-selected={g.index === sel}
              tabindex="0"
              onclick={(e) => rowClick(e, g)}
              oncontextmenu={(e) => rowContext(e, g)}
              onmouseenter={(e) => showTip(`g${g.index}`, () => api.groupTooltip(g.index), e)}
              onmouseleave={hideTip}
            >
              <!-- the enable checkbox, drawn as the status dot -->
              <input type="checkbox" class="en" title={t("skills.enabled")} aria-label={t("skills.enabled")} checked={g.enabled} onclick={(e) => e.stopPropagation()} onchange={(e) => app.run(() => api.setGroup(g.index, { enabled: e.currentTarget.checked })).then(changed)} />
              <span class="gtext">
                <span class="gname"><PobText text={g.displayLabelZh || g.displayLabel || g.label || "—"} /></span>
                {#if slotLabel || g.source}
                  <span class="gsub">{slotLabel}{#if slotLabel && g.source} · {/if}{#if g.source}{g.sourceName ?? t("skills.fromItem")}{/if}</span>
                {/if}
              </span>
              {#if g.isMain}<span class="badge gold">{t("skills.main")}</span>{/if}
              {#if g.includeInFullDPS}<span class="badge">{t("skills.inFullDps")}</span>{/if}
            </div>
          {/each}
          {#if !data.groups.length}<p class="dim pad">{t("skills.noGroups")}</p>{/if}
        {/if}
      </div>
      <div class="actions">
        <button class="btn ghost sm" title={t("skills.up")} aria-label={t("skills.up")} disabled={!group || group.index <= 1} onclick={() => moveGroup(-1)}>↑</button>
        <button class="btn ghost sm" title={t("skills.down")} aria-label={t("skills.down")} disabled={!group || !data || group.index >= data.groups.length} onclick={() => moveGroup(1)}>↓</button>
      </div>
      <p class="dim small mousehint">{t("skills.mouseHint")}</p>
    </section>

    <section class="col right">
      <div class="detail">
        {#if group && data}
          <div class="frow two">
            <label class="f">
              <span class="k">{t("skills.label")}</span>
              <input class="input" placeholder={t("skills.labelPlaceholder")} bind:value={labelDraft} onblur={() => labelDraft !== group!.label && patchGroup({ label: labelDraft })} onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()} />
            </label>
            <div class="f">
              <span class="k">{t("skills.slot")}</span>
              <Select
                class="select"
                ariaLabel={t("skills.slot")}
                value={group.slot ?? ""}
                options={[{ value: "", label: t("skills.slotNone") }, ...data.slotOptions.map((s) => ({ value: s.name, label: s.labelZh || s.label }))]}
                onchange={(v) => patchGroup({ slot: String(v) })}
              />
            </div>
          </div>
          <div class="frow">
            <label class="chk"><input type="checkbox" class="sw" checked={group.enabled} onchange={(e) => patchGroup({ enabled: e.currentTarget.checked })} /> {t("skills.enabled")}</label>
            <label class="chk"><input type="checkbox" class="sw" checked={group.includeInFullDPS} onchange={(e) => patchGroup({ includeInFullDPS: e.currentTarget.checked })} /> {t("skills.fullDps")}</label>
            {#if extras?.countShown}
              <label class="chk">
                {t("skills.groupCount")}
                <input class="input sm num cnt" type="number" min="1" bind:value={countDraft} onblur={applyCount} onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()} />
              </label>
            {/if}
            <span class="grow"></span>
            {#if group.isMain}
              <span class="badge gold">{t("skills.main")}</span>
            {:else}
              <button class="btn sm" disabled={!group.skills.length} onclick={setMain}>{t("skills.setMain")}</button>
            {/if}
          </div>
          {#if group.skills.length > 1}
            <div class="frow">
              <div class="f">
                <span class="k">{t("skills.mainSkill")}</span>
                <Select class="select sm" ariaLabel={t("skills.mainSkill")} value={group.mainActiveSkill} options={group.skills.map((s) => ({ value: s.index, label: s.nameZh || s.name || "" }))} onchange={(v) => patchGroup({ mainActiveSkill: Number(v) })} />
              </div>
            </div>
          {/if}
          {#if extras && (extras.imbued?.shown || extras.optimiseSockets?.shown)}
            <div class="frow">
              {#if extras.optimiseSockets?.shown}
                <button class="btn sm" disabled={!extras.optimiseSockets.enabled} title={t("skills.optimiseHint")} onclick={optimise}>{t("skills.optimise")}</button>
              {/if}
              {#if extras.imbued?.shown}
                <span class="f">
                  <span class="k">{t("skills.imbued")}</span>
                  <span class="imbrow">
                    <span class="combo imb">
                      <input class="input sm" disabled={!extras.imbued.enabled} placeholder={extras.imbued.nameZh || extras.imbued.name || t("skills.imbuedNone")}
                        bind:value={imbuedQuery}
                        oninput={() => { imbuedOpen = true; searchImbued(imbuedQuery); }}
                        onfocus={() => (imbuedOpen = true)}
                        onblur={() => setTimeout(() => (imbuedOpen = false), 150)} />
                      {#if imbuedOpen && imbuedHits.length}
                        <div class="drop">
                          {#each imbuedHits as h}
                            <button class="hit" onmousedown={(e) => { e.preventDefault(); void setImbued(h.gemId); }}>
                              <span style:color={gemColor(h)}>{h.nameZh || h.name}</span>
                              <span class="dim small">{h.name}</span>
                            </button>
                          {/each}
                        </div>
                      {/if}
                    </span>
                    {#if extras.imbued.name}
                      <span class="imbname">{extras.imbued.nameZh || extras.imbued.name}</span>
                      <button class="btn ghost sm" disabled={!extras.imbued.enabled} title={t("skills.imbuedClear")} onclick={() => setImbued()}>×</button>
                    {/if}
                  </span>
                </span>
              {/if}
            </div>
          {/if}
          {#if group.enabled && !group.slotEnabled}<div class="warn small">{t("skills.disabledSet")}</div>{/if}

          <div class="card gemcard">
            <table class="gems">
              <thead>
                <tr>
                  <th class="w"></th>
                  <th>{t("skills.gemName")}</th>
                  <th class="n">{t("skills.level")}</th>
                  <th class="n">{t("skills.quality")}</th>
                  <th class="c" title={t("skills.enabled")}>✓</th>
                  <th class="c">{t("skills.global1")}</th>
                  <th class="c">{t("skills.global2")}</th>
                  <th class="n">{t("skills.count")}</th>
                  <th class="w2"></th>
                </tr>
              </thead>
              <tbody>
                {#each group.gems as g (g.index)}
                  <tr class:off={!g.enabled}>
                    <td class="w"><span class="dot" style:background={gemColor(g)}></span></td>
                    <td class="name">
                      {#if editGem && editGem.index === g.index}
                        <div class="combo">
                          <!-- svelte-ignore a11y_autofocus -->
                          <input class="input" autofocus bind:value={editGem.query} oninput={() => search(editGem!.query, (h) => (editGem!.hits = h), g.index)} onkeydown={(e) => { if (e.key === "Escape") { editGem = null; hideTip(); } if (e.key === "Enter" && editGem?.hits[0]) void pickGem(g, editGem.hits[0]); }} onblur={() => setTimeout(() => { editGem = null; hideTip(); }, 150)} />
                          {#if editGem.hits.length}
                            <div class="drop">
                              {#each editGem.hits as h}
                                <button class="hit" onmousedown={(e) => { e.preventDefault(); void pickGem(g, h); }} onmouseenter={(e) => showCandidateTip(e, g.index, h)} onmouseleave={hideTip}>
                                  <span style:color={gemColor(h)}>{h.nameZh || h.name}</span>
                                  {#if h.support}<span class="dim small">{t("skills.support")}</span>{/if}
                                  {#if h.delta != null}
                                    <span class="small num delta" style:color={dpsDeltaColor(h.delta)}>{dpsDeltaText(h)}</span>
                                  {/if}
                                </button>
                              {/each}
                            </div>
                          {/if}
                        </div>
                      {:else}
                        <!-- svelte-ignore a11y_click_events_have_key_events -->
                        <span
                          class="gemname"
                          class:bad={!!g.errMsg}
                          class:fixed={g.fromItem || g.fromNode}
                          style:color={g.errMsg ? "var(--bad)" : gemColor(g)}
                          role="button"
                          tabindex="0"
                          title={g.errMsg ?? ""}
                          onclick={() => !g.fromItem && !g.fromNode && (editGem = { index: g.index, query: g.name ?? g.nameSpec, hits: [] })}
                          onmouseenter={(e) => showTip(`gem${group!.index}:${g.index}:${app.dataRev}`, () => api.gemTooltip(group!.index, g.index), e)}
                          onmouseleave={hideTip}
                        >
                          {g.nameZh || g.name || g.nameSpec}
                          {#if g.support}<span class="dim small"> · {t("skills.support")}</span>{/if}
                          {#if g.fromItem}<span class="dim small"> · {t("skills.fromItem")}</span>{/if}
                          {#if g.fromNode}<span class="dim small"> · {t("skills.fromNode")}</span>{/if}
                          {#if group.slot && !g.matchesSocket}<span class="warn small" title={t("skills.socketMismatch")}> ◇</span>{/if}
                        </span>
                      {/if}
                    </td>
                    <td class="n"><input class="input num" type="number" min="1" max="40" value={g.level} onchange={(e) => patchGem(g, { level: Number(e.currentTarget.value) })} /></td>
                    <td class="n"><input class="input num" type="number" min="0" max="23" value={g.quality} onchange={(e) => patchGem(g, { quality: Number(e.currentTarget.value) })} /></td>
                    <td class="c"><input type="checkbox" class="sw" aria-label={t("skills.enabled")} checked={g.enabled} onchange={(e) => patchGem(g, { enabled: e.currentTarget.checked })} /></td>
                    <td class="c">{#if g.hasGlobalEffect}<input type="checkbox" class="sw" aria-label={t("skills.global1")} checked={g.enableGlobal1} onchange={(e) => patchGem(g, { enableGlobal1: e.currentTarget.checked })} />{/if}</td>
                    <td class="c">{#if g.hasGlobalEffect}<input type="checkbox" class="sw" aria-label={t("skills.global2")} checked={g.enableGlobal2} onchange={(e) => patchGem(g, { enableGlobal2: e.currentTarget.checked })} />{/if}</td>
                    <td class="n"><input class="input num" type="number" min="1" value={g.count ?? 1} onchange={(e) => patchGem(g, { count: Number(e.currentTarget.value) })} /></td>
                    <td class="w2">
                      <button class="btn ghost sm" title={t("skills.up")} aria-label={t("skills.up")} disabled={g.index <= 1} onclick={() => moveGem(g, -1)}>↑</button>
                      <button class="btn ghost sm" title={t("skills.down")} aria-label={t("skills.down")} disabled={g.index >= group!.gems.length} onclick={() => moveGem(g, 1)}>↓</button>
                      <button class="btn ghost sm" disabled={g.fromItem || g.fromNode} title={t("skills.deleteGem")} aria-label={t("skills.deleteGem")} onclick={() => deleteGem(g)}>×</button>
                    </td>
                  </tr>
                {/each}
                <tr class="addrow">
                  <td class="w">
                    <svg class="plus" viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path d="M8 3v10M3 8h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" fill="none" /></svg>
                  </td>
                  <td class="name" colspan="7">
                    <div class="combo">
                      <input
                        class="input"
                        placeholder={t("skills.searchGem")}
                        aria-label={t("skills.addGem")}
                        bind:value={addQuery}
                        onfocus={() => void openSupportable()}
                        oninput={addInput}
                        onkeydown={(e) => { if (e.key === "Enter") void addGemByText(); if (e.key === "Escape") closeAdd(); }}
                        onblur={() => setTimeout(closeAdd, 150)}
                      />
                      {#if addOpen && (addHits.length || addBusy || (addMode === "supportable" && !addQuery.trim()))}
                        <div class="drop">
                          {#if addBusy}
                            <div class="drophint">{t("skills.calcDps")}</div>
                          {:else if addMode === "supportable" && !addQuery.trim()}
                            <div class="drophint">{addHits.length ? supportableHint : t("skills.noSupportable")}</div>
                          {:else if addHits.some((h) => h.dps != null)}
                            <div class="drophint">{dpsHint}</div>
                          {/if}
                          {#each addHits as h (h.gemId)}
                            <button class="hit" onmousedown={(e) => { e.preventDefault(); void addGem(h); }} onmouseenter={(e) => showCandidateTip(e, (group?.gems.length ?? 0) + 1, h)} onmouseleave={hideTip}>
                              <span class="hname">
                                <span style:color={gemColor(h)}>{h.nameZh || h.name}</span>
                                <span class="dim small">{h.name}{h.support ? ` · ${t("skills.support")}` : ""}</span>
                              </span>
                              {#if h.delta != null}
                                <span class="small num delta" style:color={dpsDeltaColor(h.delta)} title={dpsHint}>
                                  {dpsDeltaText(h)}{#if h.pct != null}<span class="amt">{formatSignedNumber(h.delta)}</span>{/if}
                                </span>
                              {:else if h.dps != null}
                                <span class="small num" style:color={dpsColor(h)} title={dpsHint}>{dpsText(h)}</span>
                              {/if}
                            </button>
                          {/each}
                        </div>
                      {/if}
                    </div>
                  </td>
                  <td class="w2"></td>
                </tr>
              </tbody>
            </table>
          </div>
        {:else}
          <div class="center dim">{t("skills.noGroups")}</div>
        {/if}

        <div class="card gemopts">
          <button class="opthead" aria-expanded={gemOptsOpen} onclick={toggleGemOpts}>
            <span class="card-title">{t("skills.gemOptions")}</span>
            <svg class="chev" class:open={gemOptsOpen} viewBox="0 0 16 16" width="12" height="12" aria-hidden="true"><path d="M6 4l4 4-4 4" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" fill="none" /></svg>
          </button>
          {#if gemOptsOpen && gemOpts}
            <div class="optgrid">
              <label class="k" for="sk-sortdps">{t("skills.sortByDps")}</label>
              <span class="ctl">
                <input id="sk-sortdps" type="checkbox" class="sw" checked={gemOpts.sortGemsByDPS} onchange={(e) => patchGemOpts({ sortGemsByDPS: e.currentTarget.checked })} />
                <Select class="select sm" value={gemOpts.sortGemsByDPSField} disabled={!gemOpts.sortGemsByDPS} options={gemOpts.sortFields.map((c) => ({ value: c.value, label: choiceLabel(c) }))} onchange={(v) => patchGemOpts({ sortGemsByDPSField: String(v) })} />
              </span>
              <span class="k">{t("skills.defaultLevel")}</span>
              <span class="ctl">
                <Select
                  class="select sm"
                  value={gemOpts.defaultGemLevel}
                  options={gemOpts.defaultGemLevels.map((c) => ({ value: c.value, label: choiceLabel(c), title: c.description || undefined }))}
                  onchange={(v) => patchGemOpts({ defaultGemLevel: String(v) })}
                  title={gemOpts.defaultGemLevels.find((c) => c.value === gemOpts!.defaultGemLevel)?.description ?? ""}
                />
              </span>
              <label class="k" for="sk-quality">{t("skills.defaultQuality")}</label>
              <span class="ctl">
                <input id="sk-quality" class="input sm num" type="number" min="0" max="23" bind:value={qualityDraft}
                  onchange={() => patchGemOpts({ defaultGemQuality: Math.max(0, Math.min(23, Math.floor(Number(qualityDraft) || 0))) })} />
              </span>
              <span class="k">{t("skills.showSupports")}</span>
              <span class="ctl">
                <Select class="select sm" value={gemOpts.showSupportGemTypes} options={gemOpts.supportGemTypes.map((c) => ({ value: c.value, label: choiceLabel(c) }))} onchange={(v) => patchGemOpts({ showSupportGemTypes: String(v) })} />
              </span>
              <label class="k" for="sk-legacy">{t("skills.showLegacy")}</label>
              <span class="ctl">
                <input id="sk-legacy" type="checkbox" class="sw" checked={gemOpts.showLegacyGems} onchange={(e) => patchGemOpts({ showLegacyGems: e.currentTarget.checked })} />
              </span>
            </div>
          {/if}
        </div>
      </div>
    </section>
  </div>

  {#if tip}<TooltipCard lines={tip.lines} x={tip.x} y={tip.y} width={360} />{/if}

  {#if pasteDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("skills.pasteGroup")}</div>
        <p class="dim small">{t("skills.pasteHint")}</p>
        <textarea class="input pastebox" bind:value={pasteDialog.text}></textarea>
        <div class="btns"><button class="btn ghost" onclick={() => (pasteDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" disabled={!pasteDialog.text.trim()} onclick={pasteOk}>OK</button></div>
      </div>
    </div>
  {/if}

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{setDialog.mode === "new" ? t("skills.skillSet") : t("skills.skillSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}<label class="chk"><input type="checkbox" class="sw" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>{/if}
        <div class="btns"><button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" disabled={!setDialog.title.trim()} onclick={setDialogOk}>OK</button></div>
      </div>
    </div>
  {/if}
</div>

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
  .toolbar :global(.select.setsel) {
    min-width: 0;
    max-width: 200px;
  }
  .ico {
    min-width: 24px;
    justify-content: center;
  }
  .grow {
    flex: 1;
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
  .btn.danger:hover:not(:disabled) {
    color: var(--bad);
    border-color: var(--bad);
  }
  .cols {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: minmax(280px, 340px) minmax(0, 1fr);
  }
  /* RWD on the main area's width: below 1100px the group list sits above the
     gem table (which needs ~540px of its own), each with a usable height,
     and the page scrolls as a whole when they do not both fit */
  @container view (max-width: 979px) {
    .cols {
      grid-template-columns: minmax(0, 1fr);
      grid-template-rows: minmax(180px, 32%) minmax(380px, 1fr);
      overflow-y: auto;
    }
    .col + .col {
      border-left: 0;
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
    align-items: center;
    min-height: 40px;
    padding: 6px 14px;
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 0 8px 8px;
  }
  .pad {
    padding: 8px;
    font-size: var(--fs-xs);
  }
  .grp {
    display: flex;
    align-items: center;
    gap: 10px;
    min-height: 52px;
    padding: 6px 10px;
    border-radius: var(--radius-m);
    font-size: var(--fs-sm);
    cursor: default;
  }
  .grp:hover {
    background: var(--surface-hover);
  }
  .grp.sel {
    background: var(--gold-soft);
  }
  /* the row's enable checkbox drawn as an 8px status dot (16px hit area) */
  input.en {
    appearance: none;
    flex: none;
    position: relative;
    width: 16px;
    height: 16px;
    margin: 0;
    border-radius: 50%;
    background: transparent;
    cursor: pointer;
  }
  input.en::after {
    content: "";
    position: absolute;
    inset: 4px;
    border-radius: 50%;
    background: var(--ink-3);
  }
  input.en:checked::after {
    background: var(--ok);
  }
  input.en:hover {
    background: var(--surface-active);
  }
  .gtext {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 1px;
  }
  .gname {
    color: var(--c-gem);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .gsub {
    font-size: var(--fs-2xs);
    color: var(--ink-3);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .grp.off .gname {
    color: var(--ink-3);
  }
  .badge {
    flex: none;
    font-size: var(--fs-2xs);
    padding: 1px 6px;
    border: 1px solid var(--edge-1);
    border-radius: 9px;
    color: var(--ink-2);
    white-space: nowrap;
  }
  .badge.gold {
    color: var(--hl);
    border-color: var(--hl-dim);
  }
  .actions {
    flex: none;
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    padding: 6px 10px;
    border-top: 1px solid var(--edge-0);
  }
  /* 插槽組列的右鍵 / Ctrl 操作原本只寫在 hover 提示裡 */
  .mousehint {
    flex: none;
    margin: 0;
    padding: 0 14px 8px;
  }
  .detail {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    padding: 14px 16px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  .frow {
    display: flex;
    align-items: center;
    gap: 8px 16px;
    flex-wrap: wrap;
  }
  .frow.two {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 180px;
    align-items: end;
  }
  .f {
    display: flex;
    flex-direction: column;
    gap: 4px;
    min-width: 0;
  }
  .k {
    font-size: var(--fs-xs);
    color: var(--ink-2);
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-size: var(--fs-sm);
    color: var(--ink-1);
    min-height: 28px;
  }
  .page :global(.select.sm),
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .input.num {
    width: 56px;
    text-align: right;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  .gemcard {
    flex: none;
  }
  .gems {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--fs-sm);
  }
  .gems th {
    height: 34px;
    text-align: left;
    font-size: var(--fs-xs);
    font-weight: 500;
    letter-spacing: 0.08em;
    color: var(--ink-2);
    padding: 0 6px;
    border-bottom: 1px solid var(--edge-0);
  }
  .gems td {
    height: 42px;
    padding: 0 6px;
    border-bottom: 1px solid var(--edge-0);
    vertical-align: middle;
  }
  .gems tr:last-child td {
    border-bottom: 0;
  }
  .gems th:first-child,
  .gems td:first-child {
    padding-left: 12px;
  }
  .gems th:last-child,
  .gems td:last-child {
    padding-right: 10px;
  }
  .gems tr.off td {
    opacity: 0.55;
  }
  .gems .w {
    width: 28px;
  }
  .gems .w2 {
    width: 100px;
    white-space: nowrap;
    text-align: right;
  }
  .gems .n {
    width: 64px;
    text-align: right;
  }
  .gems .c {
    width: 56px;
    text-align: center;
  }
  .gems .c .sw {
    vertical-align: middle;
  }
  /* 移動 / 刪除鈕常駐淡色,滑過該列才變亮 */
  td.w2 .btn:not(:disabled) {
    opacity: 0.4;
  }
  tr:hover td.w2 .btn:not(:disabled),
  td.w2 .btn:focus-visible {
    opacity: 1;
  }
  td.w2 .btn {
    padding: 0 6px;
  }
  .dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
  }
  .plus {
    display: block;
    color: var(--ink-3);
  }
  /* the gem name reads as an input; a click turns it into the search box */
  .gemname {
    display: block;
    height: 28px;
    line-height: 26px;
    padding: 0 9px;
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
    background: var(--surface-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    cursor: text;
  }
  .gemname.fixed {
    cursor: default;
    background: transparent;
    border-color: var(--edge-0);
  }
  .combo {
    position: relative;
  }
  .combo .input {
    width: 100%;
  }
  .drop {
    position: absolute;
    left: 0;
    top: calc(100% + 2px);
    z-index: 25;
    min-width: 280px;
    max-height: 260px;
    overflow-y: auto;
    background: var(--surface-2-c);
    border: 1px solid var(--edge-2);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    display: flex;
    flex-direction: column;
  }
  .drophint {
    position: sticky;
    top: 0;
    z-index: 1;
    padding: 4px 9px;
    font-size: var(--fs-xs);
    color: var(--ink-2);
    background: var(--surface-2-c);
    border-bottom: 1px solid var(--edge-0);
  }
  .hit {
    appearance: none;
    border: 0;
    background: transparent;
    color: var(--ink-0);
    text-align: left;
    padding: 5px 9px;
    display: flex;
    justify-content: space-between;
    gap: 10px;
    font-size: var(--fs-xs);
  }
  .hit:hover {
    background: var(--surface-hover);
  }
  .hname {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .delta {
    flex: none;
    text-align: right;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .delta .amt {
    margin-left: 6px;
    opacity: 0.75;
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .warn {
    color: var(--warn);
  }
  .bad {
    color: var(--bad);
  }
  .center {
    display: grid;
    place-items: center;
    min-height: 120px;
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
    width: 380px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .btns {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
  }
  .gemopts {
    flex: none;
  }
  .opthead {
    appearance: none;
    width: 100%;
    display: flex;
    align-items: center;
    justify-content: space-between;
    height: 40px;
    padding: 0 14px;
    border: 0;
    background: none;
    color: var(--ink-2);
    text-align: left;
  }
  .chev {
    transition: transform 120ms;
  }
  .chev.open {
    transform: rotate(90deg);
  }
  .optgrid {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px 16px;
    align-items: center;
    padding: 4px 14px 14px;
    border-top: 1px solid var(--edge-0);
    padding-top: 12px;
  }
  .ctl {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 8px;
  }
  .cnt {
    width: 70px;
  }
  .imbrow {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .imb {
    position: relative;
    width: 220px;
  }
  .imbname {
    color: var(--ink-0);
  }
  .pastebox {
    width: 100%;
    min-height: 140px;
    font-family: var(--font-mono, monospace);
  }
</style>
