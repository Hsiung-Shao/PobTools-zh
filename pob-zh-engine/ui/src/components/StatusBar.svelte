<!-- Structure after pob-redux's StatusBar (MIT, (c) 2026 Judd); content is ours. -->
<script lang="ts">
  import { onMount } from "svelte";
  import { api, type UpdateStatus } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  // POB's own update check runs in the engine (a subscript thread, every 12 h
  // and at boot); we only ask what it found.
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
    await app.run(() => api.applyUpdate());
    applying = false;
  }

  const stateColor = $derived(app.engine === "ready" ? "var(--ok)" : app.engine === "gone" ? "var(--bad)" : "var(--warn)");
</script>

<footer class="statusbar">
  <div class="seg">
    <span class="dot" style:background={stateColor} class:pulse={app.engine === "booting" || app.busy > 0}></span>
    <span>{t("status.engine")} {app.engine}</span>
  </div>
  {#if app.version}
    <div class="seg">
      <span class="dim">{t("status.pob")}</span>
      <span class="num">{app.version.pobVersion}</span>
      <span class="dim">{app.version.pobBranch}</span>
    </div>
  {/if}
  {#if app.gate}
    <div class="seg" class:bad={!app.gate.ok} title={app.gate.failed.join("; ")}>
      <span>{app.gate.ok ? t("app.gateOk") : t("app.gateFailed", { failed: app.gate.failed.join("; ") })}</span>
    </div>
  {/if}
  {#if upd?.available === "normal" || upd?.available === "basic"}
    <button class="seg warn" onclick={apply} disabled={applying}>
      <span>{t("status.updateAvailable")}</span>
      <span class="btn sm">{t("status.updateApply")}</span>
    </button>
  {:else if upd?.checking}
    <div class="seg dim"><span>{t("status.updateChecking")}</span></div>
  {:else if upd?.error}
    <div class="seg bad" title={upd.error}><span>{upd.error}</span></div>
  {/if}
  <div class="grow"></div>
  {#if app.error}
    <button class="seg bad" onclick={() => (app.error = null)}><span>{app.error}</span></button>
  {:else if app.notice}
    <button class="seg ok" onclick={() => (app.notice = null)}><span>{app.notice}</span></button>
  {/if}
  <div class="seg dim" title={t("status.updateBlocked")}><span>{t("status.rev")}</span><span class="num">{app.rev}</span></div>
</footer>

<style>
  .statusbar {
    height: var(--statusbar-h);
    display: flex;
    align-items: stretch;
    background: var(--bg-1);
    border-top: 1px solid var(--line-0);
    font-size: var(--fs-xs);
    color: var(--fg-2);
    letter-spacing: 0.02em;
  }
  .seg {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 0 10px;
    border-right: 1px solid var(--line-0);
    white-space: nowrap;
    appearance: none;
    background: none;
    border-top: 0;
    border-bottom: 0;
    border-left: 0;
    color: inherit;
    font: inherit;
    max-width: 50vw;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .grow {
    flex: 1;
  }
  .dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
  }
  .pulse {
    animation: pulse 1.2s ease-in-out infinite;
  }
  .bad {
    color: var(--bad);
  }
  .ok {
    color: var(--ok);
  }
  .warn {
    color: var(--warn);
    cursor: pointer;
  }
</style>
