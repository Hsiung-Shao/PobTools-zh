import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { vitePreprocess } from "@sveltejs/vite-plugin-svelte";

const lib = fileURLToPath(new URL("./src/lib/", import.meta.url));

// <script lang="ts" src="$lib/views/X.ts"></script> becomes that file inlined, so
// both themes compile one script against their own markup and relative imports.
// scripts/check.mjs does the same expansion before svelte-check, which ignores this.
export const SHARED_SCRIPT = /<script lang="ts" src="\$lib\/([^"]+)"><\/script>/;
const sharedScript = {
  markup({ content }) {
    const dependencies = [];
    const code = content.replace(SHARED_SCRIPT, (_, p) => {
      dependencies.push(lib + p);
      return `<script lang="ts">\n${readFileSync(lib + p, "utf8")}</script>`;
    });
    return { code, dependencies };
  },
};

export default {
  preprocess: [sharedScript, vitePreprocess()],
};
