<!-- Borrowed from pob-redux (MIT, (c) 2026 Judd). See NOTICE.md. -->
<script lang="ts">
  import { parsePobText } from "$lib/pobtext";

  let { text, defaultColor = null }: { text: string | null | undefined; defaultColor?: string | null } = $props();
  const spans = $derived(parsePobText(text));
</script>

{#each spans as s}<span style:color={s.color ?? defaultColor ?? undefined}>{s.text}</span>{/each}
