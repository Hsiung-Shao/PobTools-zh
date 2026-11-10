<!-- The classic Craft Item base dropdown previews its pointed-at option without
     selecting/creating it. Keep this separate from the build-item hover card. -->
<script lang="ts">
  import { api, type CraftOptions } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import Select from "./Select.svelte";
  import TooltipCard from "./TooltipCard.svelte";

  let {
    value,
    itemType,
    rarity,
    title,
    onchange,
  }: {
    value: number;
    itemType: CraftOptions["types"][number] | undefined;
    rarity: string;
    title: string;
    onchange: (v: string | number) => void;
  } = $props();

  let active = $state<{ value: number; rect: DOMRect } | null>(null);
  let tip = $state<Awaited<ReturnType<typeof api.craftItemTooltip>> | null>(null);
  const cache = new Map<string, NonNullable<typeof tip>>();

  $effect(() => {
    const base = active && itemType?.bases[active.value - 1];
    const params = base && itemType ? { rarity, type: itemType.type, base: base.name, title: title.trim() || undefined } : null;
    const key = JSON.stringify([params, app.dataRev, app.langRev]);
    tip = null;
    if (!params) return;
    const hit = cache.get(key);
    if (hit) {
      tip = hit;
      return;
    }
    // Moving, closing, switching language, or unmounting invalidates the old
    // response, even when POB finishes it after the next option's response.
    let live = true;
    const timer = window.setTimeout(async () => {
      try {
        const result = await api.craftItemTooltip(params);
        if (live) {
          cache.set(key, result);
          tip = result;
        }
      } catch {
        // Preview failures must not prevent choosing or creating an item.
      }
    }, 120);
    return () => {
      live = false;
      clearTimeout(timer);
    };
  });
</script>

<Select
  class="select"
  ariaLabel={t("items.craftBase")}
  {value}
  options={(itemType?.bases ?? []).map((b, i) => ({ value: i + 1, label: b.labelZh || b.label }))}
  {onchange}
  onactive={(v, rect) => (active = v != null && rect ? { value: Number(v), rect } : null)}
/>
{#if tip && active && tip.lines.length}
  <TooltipCard lines={tip.lines} accent={tip.color} x={active.rect.right + 8} y={active.rect.top} avoid={active.rect} layer={1110} />
{/if}
