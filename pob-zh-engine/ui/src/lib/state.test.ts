import { describe, expect, it } from "vitest";
import { app } from "./state.svelte";

describe("AppState.run", () => {
  it("counts busy while a call is in flight and clears the error on success", async () => {
    app.error = "old";
    let resolve!: (v: number) => void;
    const p = app.run(() => new Promise<number>((r) => (resolve = r)));
    expect(app.busy).toBe(1);
    resolve(7);
    await expect(p).resolves.toBe(7);
    expect(app.busy).toBe(0);
    expect(app.error).toBeNull();
  });
  it("turns a bridge error into a readable message and returns undefined", async () => {
    const r = await app.run(() => Promise.reject({ code: "lua_error", message: "boom" }));
    expect(r).toBeUndefined();
    expect(app.busy).toBe(0);
    expect(app.error).toBe("lua_error: boom");
  });
  it("reset clears the build and returns to the build list", () => {
    app.view = "tree";
    app.reset();
    expect(app.view).toBe("builds");
    expect(app.loaded).toBe(false);
    expect(app.rev).toBe(0);
  });
  it("the back button walks up the build list's folders first, then the previous screen", () => {
    app.reset();
    app.view = "settings";
    app.view = "builds";
    app.buildsSubPath = "a/b/";
    expect(app.goBack()).toBe(true);
    expect(app.buildsSubPath).toBe("a/");
    expect(app.goBack()).toBe(true);
    expect(app.buildsSubPath).toBe("");
    expect(app.goBack()).toBe(true);
    expect(app.view).toBe("settings");
    expect(app.goForward()).toBe(true);
    expect(app.view).toBe("builds");
  });
  it("build tabs are skipped once no build is loaded", () => {
    app.reset();
    app.view = "settings";
    app.view = "tree"; // (no build loaded in the test)
    app.view = "builds";
    expect(app.goBack()).toBe(true);
    expect(app.view).toBe("settings");
    expect(app.goBack()).toBe(true); // the list the page started on
    expect(app.view).toBe("builds");
    expect(app.goBack()).toBe(false);
  });
});
