// svelte-check does not run markup preprocessors, so it cannot see the shared
// scripts svelte.config.js inlines. Check an expanded copy of src/ instead.
import { cpSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { SHARED_SCRIPT } from "../svelte.config.js";

const out = ".check";
rmSync(out, { recursive: true, force: true });
cpSync("src", `${out}/src`, { recursive: true });
const inlined = new Set();
for (const e of readdirSync(`${out}/src`, { recursive: true, withFileTypes: true })) {
  if (!e.isFile() || !e.name.endsWith(".svelte")) continue;
  const f = `${e.parentPath}/${e.name}`;
  const src = readFileSync(f, "utf8");
  const code = src.replace(SHARED_SCRIPT, (_, p) => {
    inlined.add(`${out}/src/lib/${p}`);
    return `<script lang="ts">\n${readFileSync(`${out}/src/lib/${p}`, "utf8")}</script>`;
  });
  if (code !== src) writeFileSync(f, code);
}
for (const f of inlined) rmSync(f); // only meaningful inside a component
writeFileSync(`${out}/tsconfig.json`, JSON.stringify({ extends: "../tsconfig.json", include: ["src/**/*.ts", "src/**/*.svelte"] }));
const r = spawnSync("npx", ["svelte-check", "--workspace", out, "--tsconfig", "./tsconfig.json", "--fail-on-warnings"], { stdio: "inherit", shell: true });
rmSync(out, { recursive: true, force: true });
process.exit(r.status ?? 1);
