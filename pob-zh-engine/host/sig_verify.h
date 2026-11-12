// PobTools 發佈簽章驗證(ECDSA P-256 / SHA-256,Windows CNG)。
//
// 為什麼 sha256 不夠:它是 GitHub API 在同一個 JSON 回應裡報的 digest,只證明位元組沒在傳輸中壞掉,
// 不證明檔案是維護者做的(能改 JSON 的人可連 digest 一起改)。簽章把信任根搬到「編進 exe 的公鑰」,
// 攻擊者還得拿到維護者私鑰。
//
// 形狀刻意只有一種(不用 DER/PEM、不做演算法協商;換演算法靠 manifest 的 schema 欄位整組換):
//   公鑰   = 未壓縮點的 X||Y,64 bytes,128 個十六進位字元(update_pubkeys.h)
//   簽章   = IEEE P1363 的 r||s,64 bytes,128 個十六進位字元(.sig 資產)
//
// ⚠ .NET 的 ECDsa.SignData 產出正好是 P1363 r||s,與 BCryptVerifySignature 期望的相同;改用
// X509/DER 的簽章工具會**安靜地**產出驗不過的簽章(自檢 fixture 釘住這個介面)。
#pragma once

#include <string>

// 用「編進這個 exe 的公鑰清單」驗一段資料的分離式簽章。
// sigHex 是 .sig 資產的內容(允許前後空白與換行);任何一把公鑰驗過就回 true,
// 並把命中的索引寫進 *keyIndex(0=primary、1=backup)。
// 失敗時 *err(若給)會拿到一句 ASCII 的原因,供寫進更新記錄檔 —— UI 顯示的是
// app_update.cpp 那邊的固定中文訊息,不是這句。
bool VerifyReleaseSignature(const void* data, size_t size, const std::string& sigHex,
                            int* keyIndex, std::string* err);

// 同上,但公鑰清單由呼叫端給。只為讓自檢用臨時金鑰驅動驗過之後的各分支(tag 不符、資產不在清單、
// digest 打架…),C++ 端拿不到真正的發佈私鑰。
// ⚠ 正式路徑一律走上面那個版本,不要在產品程式碼裡自己傳金鑰進來。
bool VerifyReleaseSignatureWithKeys(const void* data, size_t size, const std::string& sigHex,
                                    const char* const* keysHex, int keyCount,
                                    int* keyIndex, std::string* err);

// 同上,但公鑰由呼叫端指定(128 個十六進位字元);自檢用來做「換一把金鑰就必須驗不過」的突變測試。
bool VerifyDetachedSignature(const void* data, size_t size, const std::string& sigHex,
                             const std::string& pubKeyHex, std::string* err);

// 自檢專用:現場產生臨時 P-256 金鑰,對 data 簽章,回傳公鑰與簽章(與發佈管線相同的十六進位形式)。
// 驗證邏輯本身不需外部 fixture;fixture 留給「PowerShell 簽的東西 C++ 驗不驗得過」的跨工具介面。
bool SignWithEphemeralKeyForTest(const void* data, size_t size,
                                 std::string* pubKeyHex, std::string* sigHex);

// "pob-zh.exe --verify-manifest <manifest> <signature>":打包腳本反向驗證剛簽出的東西。判準必須是
// **這支 exe 內嵌的公鑰**,而不是腳本手上那份(否則兩邊共享盲點,一起錯就一起 PASS)。
int RunVerifyManifestCli(const std::wstring& manifestPath, const std::wstring& sigPath);
