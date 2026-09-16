import { describe, expect, it } from "vitest";
import { Bridge, type Transport } from "./bridge";

class FakeTransport implements Transport {
  sent: string[] = [];
  handler: ((line: string) => void) | null = null;
  send(line: string) {
    this.sent.push(line);
  }
  onMessage(fn: (line: string) => void) {
    this.handler = fn;
  }
  reply(obj: unknown) {
    this.handler?.(JSON.stringify(obj));
  }
}

describe("Bridge", () => {
  it("matches responses to requests by id", async () => {
    const t = new FakeTransport();
    const b = new Bridge(t);
    const p1 = b.call<{ n: number }>("a");
    const p2 = b.call<{ n: number }>("b");
    const [r1, r2] = t.sent.map((s) => JSON.parse(s));
    t.reply({ id: r2.id, result: { n: 2 } });
    t.reply({ id: r1.id, result: { n: 1 } });
    expect(await p1).toEqual({ n: 1 });
    expect(await p2).toEqual({ n: 2 });
  });
  it("rejects with the error object", async () => {
    const t = new FakeTransport();
    const b = new Bridge(t);
    const p = b.call("x");
    const req = JSON.parse(t.sent[0]);
    t.reply({ id: req.id, error: { code: "lua_error", message: "boom" } });
    await expect(p).rejects.toEqual({ code: "lua_error", message: "boom" });
  });
  it("times out", async () => {
    const t = new FakeTransport();
    const b = new Bridge(t);
    await expect(b.call("slow", {}, 10)).rejects.toMatchObject({ code: "timeout" });
  });
  it("dispatches events and treats non-JSON as stray", () => {
    const t = new FakeTransport();
    const b = new Bridge(t);
    const got: unknown[] = [];
    b.on("hello", (d) => got.push(d));
    b.on("host.stray", (d) => got.push(d));
    t.reply({ event: "hello", data: { protocol: 1 } });
    t.handler?.("not json");
    expect(got).toEqual([{ protocol: 1 }, { line: "not json" }]);
  });
});
