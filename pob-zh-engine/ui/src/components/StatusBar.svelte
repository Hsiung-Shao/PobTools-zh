<!-- 底部狀態列:引擎狀態、POB 版本、相容閘門、POB 自己的更新、錯誤/提示、rev。 -->
<script lang="ts">
  import { onMount } from "svelte";
  import { api, type UpdateStatus } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  // POB's own update check runs inside the engine (its subscript thread, at
  // boot and every 12 h); we only ask what it found.
  let upd = $state<UpdateStatus | null>(null);
  let applying = $state(false);
  onMount(() => {
    let timer = 0;
    const tick = async () => {
      if (app.engine === "ready") {
        try {
          upd = await api.getUpdateStatus();
        } catch {
          upd = null;
        }
      }
      timer = window.setTimeout(tick, 30000);
    };
    void tick();
    return () => clearTimeout(timer);
  });

  async function apply() {
    applying = true;
    const mode = upd?.available;
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
    await app.run(() => api.applyUpdate(mode ?? undefined));
    applying = false;
  }

  const tone = $derived(app.engine === "ready" ? "ok" : app.engine === "gone" ? "bad" : "warn");
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
    <button class="item act" onclick={apply} disabled={applying}>
      <span class="warn">{t("status.updateAvailable")}</span>
      <span class="chip">{t("status.updateApply")}</span>
    </button>
  {:else if upd?.checking}
    <span class="item dim">{t("status.updateChecking")}</span>
  {:else if upd?.error}
    <span class="item bad" title={upd.error}>{upd.error}</span>
  {/if}

  <span class="grow"></span>

  {#if app.error}
    <button class="item act bad" onclick={() => (app.error = null)}>{app.error}</button>
  {:else if app.notice}
    <button class="item act ok" onclick={() => (app.notice = null)}>{app.notice}</button>
  {/if}
  <span class="item dim" title={t("status.updateBlocked")}>{t("status.rev")} <span class="num">{app.rev}</span></span>
</footer>

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
    color: #17120a;
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
</style>
