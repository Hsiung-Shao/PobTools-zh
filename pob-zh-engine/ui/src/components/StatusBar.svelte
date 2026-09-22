<!-- 底部狀態列:引擎狀態、POB 版本、相容閘門、POB 自己的更新、錯誤/提示、rev。 -->
<script lang="ts">
  import { onMount } from "svelte";
  import { api, type PobUpdateInfo, type UpdateStatus } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import PobText from "./PobText.svelte";

  // POB's own update check runs inside the engine (its subscript thread, at
  // boot and every 12 h, or when asked here); we only ask what it found.
  let upd = $state<UpdateStatus | null>(null);
  let applying = $state(false);
  let dialog = $state<PobUpdateInfo | null>(null);
  // a check the user started is polled every second until POB answers
  let watching = false;
  let timer = 0;
  async function poll() {
    clearTimeout(timer);
    if (app.engine === "ready") {
      try {
        upd = await api.getUpdateStatus();
      } catch {
        upd = null;
      }
    }
    const running = !!upd?.checking;
    if (watching && !running) {
      watching = false;
      if (upd?.available === "none") app.notice = t("status.updateNone");
    }
    timer = window.setTimeout(poll, running || watching ? 1000 : 30000);
  }
  onMount(() => {
    void poll();
    return () => clearTimeout(timer);
  });

  /** POB's "Check for Update" button: launch:CheckForUpdate. */
  async function check() {
    const r = await app.run(() => api.checkUpdateAsync());
    if (!r) return;
    watching = true;
    void poll();
  }
  /** POB's "Update Ready" button: its dialog lists what changed first. */
  async function openUpdate() {
    const r = await app.run(() => api.pobUpdateInfo());
    if (r) dialog = r;
  }

  async function apply(saveFirst: boolean) {
    const mode = upd?.available;
    if (mode !== "normal" && mode !== "basic") return;
    if (saveFirst) {
      await app.save();
      if (app.info?.unsaved) return;
    }
    dialog = null;
    applying = true;
    if (mode === "basic") {
      // The engine hands the runtime files to Update.exe and exits inside this
      // call, so no reply comes back; the host closes this window and the
      // updater reopens it. A timeout here is the expected outcome.
      app.notice = t("status.updating");
      try {
        await api.applyUpdate(mode, 8000);
      } catch {
        /* expected: the child exited before answering */
      }
      return;
    }
    await app.run(() => api.applyUpdate(mode));
    applying = false;
  }

  const tone = $derived(app.engine === "ready" ? "ok" : app.engine === "gone" ? "bad" : "warn");
  const unsaved = $derived(!!app.info?.unsaved);
  const canSave = $derived(!!app.info?.dbFileName);
</script>

<footer class="bar">
  <span class="item">
    <i class="lamp {tone}" class:pulse={app.engine === "booting" || app.busy > 0}></i>
    {t("status.engine")} {app.engine}
  </span>
  {#if app.version}
    <span class="item">
      <span class="dim">{t("status.pob")}</span>
      <span class="num">{app.version.pobVersion}</span>
      <span class="dim">{app.version.pobBranch}</span>
    </span>
  {/if}
  {#if app.gate}
    <span class="item" class:bad={!app.gate.ok} title={app.gate.failed.join("; ")}>
      {app.gate.ok ? t("app.gateOk") : t("app.gateFailed", { failed: app.gate.failed.join("; ") })}
    </span>
  {/if}
  {#if upd?.available === "normal" || upd?.available === "basic"}
    <button class="item act" onclick={openUpdate} disabled={applying || app.busy > 0}>
      <span class="warn">{t("status.updateAvailable")}</span>
      <span class="chip">{t("status.updateApply")}</span>
    </button>
  {:else if upd?.checking}
    <span class="item dim pulse">{upd.progress || t("status.updateChecking")}</span>
  {:else}
    <button class="item act" onclick={check} disabled={app.engine !== "ready" || app.busy > 0}>{t("status.updateCheck")}</button>
    {#if upd?.error}
      <span class="item bad" title={upd.error}>{upd.error}</span>
    {/if}
  {/if}

  <span class="grow"></span>

  {#if app.error}
    <button class="item act bad" onclick={() => (app.error = null)}>{app.error}</button>
  {:else if app.notice}
    <button class="item act ok" onclick={() => (app.notice = null)}>{app.notice}</button>
  {/if}
  <span class="item dim" title={t("status.updateBlocked")}>{t("status.rev")} <span class="num">{app.rev}</span></span>
</footer>

{#if dialog}
  <div class="modal">
    <div class="dialog">
      <div class="dhead">
        <span class="label">{t("status.updateTitle")}</span>
        <span class="dim small">{t("status.updateFrom", { version: dialog.version ?? "", branch: dialog.branch ?? "" })}</span>
      </div>
      <div class="log">
        {#each dialog.lines as l}
          {#if l.text}
            <div class="ll" class:head={(l.height ?? 14) >= 20}><PobText text={l.text} /></div>
          {:else}
            <div class="gap"></div>
          {/if}
        {/each}
        {#if dialog.truncated}<div class="dim small">{t("status.updateTruncated")}</div>{/if}
      </div>
      {#if unsaved}<p class="warn small">{t("status.updateUnsaved")}</p>{/if}
      <div class="btns">
        <button class="btn ghost" onclick={() => (dialog = null)}>{t("status.updateCancel")}</button>
        {#if unsaved && canSave}
          <button class="btn primary" disabled={app.busy > 0} onclick={() => apply(true)}>{t("status.updateSaveApply")}</button>
          <button class="btn" disabled={app.busy > 0} onclick={() => apply(false)}>{t("status.updateAnyway")}</button>
        {:else}
          <button class="btn primary" disabled={app.busy > 0} onclick={() => apply(false)}>{unsaved ? t("status.updateAnyway") : t("status.updateNow")}</button>
        {/if}
      </div>
    </div>
  </div>
{/if}

<style>
  .bar {
    height: var(--statusbar-h);
    display: flex;
    align-items: center;
    gap: 2px;
    padding: 0 8px;
    background: var(--surface-0);
    border-top: 1px solid var(--edge-0);
    font-size: var(--fs-xs);
    color: var(--ink-2);
  }
  .item {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 0 8px;
    height: 100%;
    white-space: nowrap;
    max-width: 48vw;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .item + .item::before {
    content: "·";
    color: var(--ink-4);
    margin-right: 6px;
  }
  .act {
    appearance: none;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    cursor: pointer;
  }
  .act:hover {
    background: var(--surface-hover);
  }
  .grow {
    flex: 1;
  }
  .lamp {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    display: inline-block;
  }
  .lamp.ok {
    background: var(--ok);
    box-shadow: 0 0 6px var(--ok);
  }
  .lamp.warn {
    background: var(--warn);
  }
  .lamp.bad {
    background: var(--bad);
  }
  .pulse {
    animation: pulse 1.2s ease-in-out infinite;
  }
  .chip {
    padding: 1px 7px;
    border-radius: 9px;
    background: var(--gold);
    color: var(--on-gold);
    font-weight: 600;
  }
  .bad {
    color: var(--bad);
  }
  .ok {
    color: var(--ok);
  }
  .warn {
    color: var(--warn);
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .modal {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: var(--backdrop);
    z-index: 50;
  }
  .dialog {
    width: min(760px, calc(100vw - 40px));
    max-height: calc(100vh - 60px);
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 14px 16px;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    font-size: var(--fs-sm);
    color: var(--ink-1);
  }
  .dhead {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 12px;
  }
  .log {
    flex: 1;
    min-height: 120px;
    overflow-y: auto;
    padding: 8px 10px;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    background: var(--surface-1);
    font-size: var(--fs-xs);
    line-height: 1.5;
  }
  .ll {
    white-space: pre-wrap;
  }
  .ll.head {
    margin-top: 4px;
    font-weight: 600;
    font-size: var(--fs-sm);
  }
  .gap {
    height: 8px;
  }
  .btns {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
  }
  p {
    margin: 0;
  }
</style>
