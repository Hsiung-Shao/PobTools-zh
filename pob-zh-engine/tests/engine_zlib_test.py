#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""引擎 Deflate/Inflate 格式測試(gzip 與 zlib)。

POB 交易功能呼叫 Deflate(s, true),期望 gzip(1F 8B);不帶第二參數要是 zlib(0x78)。
做法:暫存資料夾放 Launch.lua,以 `<exe> --engine-headless <dir>` 執行,
Lua 把結果以 hex 寫檔,Python 讀回判定(結束碼不算判準)。

Run:  python pob-zh-engine/tests/engine_zlib_test.py [exe 路徑]
"""

import gzip
import os
import subprocess
import sys
import tempfile
import zlib

DEFAULT_EXE = (r"D:\codeproject\Pob2\.claude\worktrees\translation-error-red-water-ff8277"
               r"\pob-zh-engine\dist\pob-zh.exe")
EXE = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE

SAMPLE = '{"query":{"status":{"option":"securable"},"stats":[{"type":"weight"}]}}' * 5

# 結果檔每行 "key=value";value 為 hex 或 ERR:訊息。
LAUNCH_LUA = r'''
local dir = "%(dir)s"
local s = "%(sample)s"
local function hex(x)
    return (x:gsub(".", function(c) return string.format("%%02x", c:byte()) end))
end
local out = {}
local function run(key, fn)
    local ok, r = pcall(fn)
    if ok and type(r) == "string" then
        out[#out + 1] = key .. "=" .. hex(r)
    else
        out[#out + 1] = key .. "=ERR:" .. tostring(r):gsub("[\r\n]", " ")
    end
end
run("gz", function() return Deflate(s, true) end)
run("zl", function() return Deflate(s) end)
run("inf_gz", function() return Inflate(Deflate(s, true)) end)
run("inf_zl", function() return Inflate(Deflate(s)) end)
local f = io.open(dir .. "/result.txt", "wb")
f:write(table.concat(out, "\n"), "\n")
f:close()
os.exit(0)
'''


def run_engine():
    """回傳 (結果 dict, 失敗原因或 None)。"""
    with tempfile.TemporaryDirectory() as tmp:
        lua = LAUNCH_LUA % {"dir": tmp.replace("\\", "/"), "sample": SAMPLE.replace('"', '\\"')}
        with open(os.path.join(tmp, "Launch.lua"), "w", encoding="utf-8") as f:
            f.write(lua)
        try:
            p = subprocess.run([EXE, "--engine-headless", tmp], cwd=os.path.dirname(EXE),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=60)
        except (OSError, subprocess.TimeoutExpired) as e:
            return {}, "引擎執行失敗: %r" % (e,)
        rp = os.path.join(tmp, "result.txt")
        if not os.path.exists(rp):
            head = (p.stdout + b"\n" + p.stderr).decode("utf-8", "replace").splitlines()[:8]
            return {}, "無結果檔(exit=%s):%s" % (p.returncode, " | ".join(head))
        res = {}
        with open(rp, encoding="utf-8") as f:
            for line in f.read().splitlines():
                k, _, v = line.partition("=")
                res[k] = v
        return res, None


def unhex(res, key):
    """回傳 (bytes 或 None, 錯誤訊息)。"""
    v = res.get(key)
    if v is None:
        return None, "缺少 %s" % key
    if v.startswith("ERR:"):
        return None, "引擎呼叫失敗 %s" % v[4:]
    return bytes.fromhex(v), None


results = []


def report(code, title, ok, detail=""):
    results.append(ok)
    print("%s %s %s (%s)" % ("PASS" if ok else "FAIL", code, title, detail))


def main():
    res, err = run_engine()
    titles = {
        "Z1": "gzip 模式輸出可被標準 gzip 解開",
        "Z2": "gzip 模式輸出以 1F 8B 開頭",
        "Z3": "不帶第二參數仍輸出 zlib",
        "Z4": "Inflate 解得開 gzip 與 zlib",
    }
    if err:
        for c, t in titles.items():
            report(c, t, False, err)
        print("RESULT FAIL")
        sys.exit(1)

    want = SAMPLE.encode("ascii")

    gz, e = unhex(res, "gz")
    if e:
        report("Z1", titles["Z1"], False, e)
        report("Z2", titles["Z2"], False, e)
    else:
        try:
            got = gzip.decompress(gz)
            report("Z1", titles["Z1"], got == want, "解開長度 %d,預期 %d" % (len(got), len(want)))
        except Exception as ex:  # noqa: BLE001
            report("Z1", titles["Z1"], False, "gzip.decompress 失敗: %s; 開頭=%s" % (ex, gz[:4].hex()))
        report("Z2", titles["Z2"], gz[:2] == b"\x1f\x8b", "開頭=%s" % gz[:2].hex())

    zl, e = unhex(res, "zl")
    if e:
        report("Z3", titles["Z3"], False, e)
    else:
        try:
            got = zlib.decompress(zl)
            report("Z3", titles["Z3"], got == want, "開頭=%s" % zl[:2].hex())
        except Exception as ex:  # noqa: BLE001
            report("Z3", titles["Z3"], False, "zlib.decompress 失敗: %s; 開頭=%s" % (ex, zl[:4].hex()))

    a, ea = unhex(res, "inf_gz")
    b, eb = unhex(res, "inf_zl")
    if ea or eb:
        report("Z4", titles["Z4"], False, "; ".join(x for x in (ea, eb) if x))
    else:
        report("Z4", titles["Z4"], a == want and b == want,
               "gzip 往返=%s, zlib 往返=%s" % (a == want, b == want))

    ok = all(results)
    print("RESULT PASS" if ok else "RESULT FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
