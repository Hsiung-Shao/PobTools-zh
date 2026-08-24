// PobTools 發佈簽章驗證(ECDSA P-256 / SHA-256,Windows CNG)。
//
// 為什麼需要這一層,而 sha256 不夠:更新器原本比對的 sha256 是 GitHub API 自己
// 在同一個 JSON 回應裡報的 digest。那證明「下載回來的位元組沒在傳輸途中壞掉」,
// 但完全不證明「這個檔是維護者做的」—— 能改那份 JSON 的人(帳號被盜、token 外
// 洩、release 被刪掉重傳)可以連 digest 一起改,客戶端會很有信心地驗過然後安裝。
//
// 簽章把信任根從「GitHub 上的資料」搬到「編進 exe 的公鑰」。攻擊者要騙過它,
// 除了 GitHub 之外還得拿到維護者的私鑰。
//
// 形狀刻意固定成只有一種:
//   公鑰   = 未壓縮點的 X||Y,64 bytes,寫成 128 個十六進位字元(update_pubkeys.h)
//   簽章   = IEEE P1363 的 r||s,64 bytes,寫成 128 個十六進位字元(.sig 資產)
// 不用 DER、不用 PEM、不做演算法協商。可以走錯的分支愈少愈好,而演算法要換版
// 時是靠 manifest 的 schema 欄位整組換掉,不是靠在這裡加 if。
//
// ⚠ .NET 的 ECDsa.SignData 產出的正好就是 P1363 的 r||s,與 BCryptVerifySignature
// 期望的形狀相同,兩邊不需要轉換。改用 X509/DER 那套簽章工具會**安靜地**產出
// 驗不過的簽章 —— 自檢的 fixture 就是釘住這個介面用的。
#pragma once

#include <string>

// 用「編進這個 exe 的公鑰清單」驗一段資料的分離式簽章。
// sigHex 是 .sig 資產的內容(允許前後空白與換行);任何一把公鑰驗過就回 true,
// 並把命中的索引寫進 *keyIndex(0=primary、1=backup)。
// 失敗時 *err(若給)會拿到一句 ASCII 的原因,供寫進更新記錄檔 —— UI 顯示的是
// app_update.cpp 那邊的固定中文訊息,不是這句。
bool VerifyReleaseSignature(const void* data, size_t size, const std::string& sigHex,
                            int* keyIndex, std::string* err);

// 同上,但公鑰清單由呼叫端給。存在的理由只有一個:讓自檢能用現場產生的臨時金鑰
// 驅動「驗過之後那些判斷」的每一條分支(tag 不符、資產不在清單裡、digest 打架…)。
// 那些分支沒有這個接縫就完全測不到 —— 因為 C++ 這端拿不到真正的發佈私鑰。
// ⚠ 正式路徑一律走上面那個版本,不要在產品程式碼裡自己傳金鑰進來。
bool VerifyReleaseSignatureWithKeys(const void* data, size_t size, const std::string& sigHex,
                                    const char* const* keysHex, int keyCount,
                                    int* keyIndex, std::string* err);

// 同上,但公鑰由呼叫端指定(128 個十六進位字元)。自檢用來做「換一把金鑰就必須
// 驗不過」的突變測試,以及讓打包腳本以外的人可以驗任意金鑰。
bool VerifyDetachedSignature(const void* data, size_t size, const std::string& sigHex,
                             const std::string& pubKeyHex, std::string* err);

// 自檢專用:現場產生一把臨時的 P-256 金鑰,對 data 簽章,回傳公鑰與簽章
// (與發佈管線相同的十六進位形式)。有了它,「驗證邏輯本身對不對」不需要任何
// 外部 fixture 就能測 —— fixture 留給「PowerShell 簽的東西 C++ 驗不驗得過」
// 這個真正有風險的跨工具介面。
bool SignWithEphemeralKeyForTest(const void* data, size_t size,
                                 std::string* pubKeyHex, std::string* sigHex);

// "pob-zh.exe --verify-manifest <manifest> <signature>":打包腳本用它來反向
// 驗證自己剛簽出來的東西。判準必須是**這支 exe 內嵌的公鑰**,而不是腳本手上
// 那份 —— 否則驗證器與被驗程式共享同一個盲點,兩邊一起錯就會一起說 PASS。
int RunVerifyManifestCli(const std::wstring& manifestPath, const std::wstring& sigPath);
