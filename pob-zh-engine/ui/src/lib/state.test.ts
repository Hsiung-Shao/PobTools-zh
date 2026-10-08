import { afterEach, describe, expect, it, vi } from "vitest";
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

describe("an Open in PoB link while a build is open (host.import_link)", () => {
  afterEach(() => {
    vi.restoreAllMocks();
    app.info = null;
    app.linkAsk = null;
    app.afterSaveAs = null;
  });
  it("nothing unsaved: the build is replaced at once, no question", () => {
    const imp = vi.spyOn(app, "importLink").mockResolvedValue(undefined);
    app.info = { unsaved: false } as any;
    app.onImportLink("pob://poeninja/a");
    expect(imp).toHaveBeenCalledWith("pob://poeninja/a");
    expect(app.linkAsk).toBeNull();
  });
  it("unsaved, Save: asks first, then saves and imports after the save", async () => {
    const imp = vi.spyOn(app, "importLink").mockResolvedValue(undefined);
    const save = vi.spyOn(app, "saveOrAsk").mockImplementation(async (then?: () => void) => {
      then?.();
      return true;
    });
    app.info = { unsaved: true } as any;
    app.onImportLink("pob://poeninja/b");
    expect(app.linkAsk).toBe("pob://poeninja/b");
    expect(imp).not.toHaveBeenCalled();
    await app.linkSave();
    expect(save).toHaveBeenCalledTimes(1);
    expect(imp).toHaveBeenCalledWith("pob://poeninja/b");
    expect(app.linkAsk).toBeNull();
  });
  it("unsaved, Save on a never-saved build: the import waits for Save As (afterSaveAs)", async () => {
    const imp = vi.spyOn(app, "importLink").mockResolvedValue(undefined);
    app.info = { unsaved: true } as any;
    app.header = { dbFileName: undefined } as any;
    app.onImportLink("pob://poeninja/c");
    const req = app.saveAsRequest;
    await app.linkSave();
    expect(app.saveAsRequest).toBe(req + 1);
    expect(imp).not.toHaveBeenCalled();
    expect(app.afterSaveAs).not.toBeNull();
    app.afterSaveAs?.();
    expect(imp).toHaveBeenCalledWith("pob://poeninja/c");
  });
  it("unsaved, Don't save: imports without saving", () => {
    const imp = vi.spyOn(app, "importLink").mockResolvedValue(undefined);
    const save = vi.spyOn(app, "saveOrAsk");
    app.info = { unsaved: true } as any;
    app.onImportLink("pob://poeninja/d");
    app.linkDiscard();
    expect(imp).toHaveBeenCalledWith("pob://poeninja/d");
    expect(save).not.toHaveBeenCalled();
    expect(app.linkAsk).toBeNull();
  });
  it("unsaved, Cancel: nothing happens", () => {
    const imp = vi.spyOn(app, "importLink").mockResolvedValue(undefined);
    const save = vi.spyOn(app, "saveOrAsk");
    app.info = { unsaved: true } as any;
    app.onImportLink("pob://poeninja/e");
    app.linkCancel();
    expect(imp).not.toHaveBeenCalled();
    expect(save).not.toHaveBeenCalled();
    expect(app.linkAsk).toBeNull();
  });
  it("a cancelled Save As drops the pending action (no import on a later save)", () => {
    app.afterSaveAs = () => {};
    app.cancelSaveAs();
    expect(app.afterSaveAs).toBeNull();
  });
  it("browser mode replays old events to a new page: a link nonce is handled once", () => {
    const imp = vi.spyOn(app, "importLink").mockResolvedValue(undefined);
    app.info = { unsaved: false } as any;
    const nonce = 1700000000000 + Math.floor(Math.random() * 1e6);
    app.onImportLink("pob://poeninja/f", nonce);
    app.onImportLink("pob://poeninja/f", nonce);
    expect(imp).toHaveBeenCalledTimes(1);
  });
});
