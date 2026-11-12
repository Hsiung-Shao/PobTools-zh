// --mcp-selftest [pobDir]: 啟動 `pob-zh.exe --mcp` 子行程,用 stdio JSON-RPC 走完
// 契約(docs/notes/mcp_contract.md)的 M1-M11 測項。
// 報告寫到 <exeDir>mcp_selftest.txt;回傳失敗項目數。
#pragma once

#include <string>

int RunMcpSelfTest(const std::wstring& exeDir, const std::wstring& pobDirOverride);
