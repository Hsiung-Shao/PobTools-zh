// Build config. The page is served by WebView2 from dist\ui\ through the
// https://app.pobtools/ virtual host, so every URL must be relative; the dev
// server (port 1420) only serves the mock-transport page during `vite dev`.
import { fileURLToPath, URL } from "node:url";
import { defineConfig } from "vitest/config"; // vite's defineConfig plus the `test` block
import { svelte } from "@sveltejs/vite-plugin-svelte";

export default defineConfig({
  plugins: [svelte()],
  base: "./",
  resolve: {
    alias: { $lib: fileURLToPath(new URL("./src/lib", import.meta.url)) },
  },
  clearScreen: false,
  server: { port: 1420, strictPort: true },
  build: {
    // WebView2 is an evergreen Chromium; ES2022 is safe.
    target: ["es2022", "chrome110"],
    outDir: "../dist/ui",
    emptyOutDir: true,
    sourcemap: false,
  },
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
});
