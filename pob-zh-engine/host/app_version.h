/* PobTools application version — the single source of truth.
 *
 * Consumed by C++ (launcher UI, app updater) and by rc.exe (pob-zh.rc
 * VERSIONINFO), so this header must stay plain #define with a classic
 * include guard (rc.exe does not understand #pragma once).
 *
 * Release checklist: bump these three numbers, add a matching entry to
 * changelog.h, rebuild pob-zh, then run tools/package_release.ps1
 * -Version <same version> -DataTag data-<n> (the script asserts the version
 * matches this header AND that the version has not been released already).
 *
 * ⚠ v0.19.0 起「patch bump = 純翻譯釋出、靜默套用」那條慣例作廢。翻譯資料自成
 * 一條 data-<n> 發佈線,程式這條線的**每一次** bump 都會在啟動器出現更新提示
 * (patch 也一樣)—— 套用程式更新一定要 rename pob-zh.exe + engine\* 並重啟行程,
 * 那不能在使用者沒按下去的情況下發生。
 */
#ifndef POBTOOLS_APP_VERSION_H
#define POBTOOLS_APP_VERSION_H

#define POBTOOLS_VER_MAJOR 1
#define POBTOOLS_VER_MINOR 8
#define POBTOOLS_VER_PATCH 7

#define PT_VER_STR2(x) #x
#define PT_VER_STR(x) PT_VER_STR2(x)
/* beta 建置:cmake -DPOBTOOLS_VERSION_SUFFIX=beta-<commit 短雜湊> → "1.8.6-beta-1ef80a6"。
 * 版號本身不 bump,beta 跟著目前的正式版走(見 skill pobtools-release「Beta 線」)。 */
#ifdef POBTOOLS_VERSION_SUFFIX
#define POBTOOLS_VERSION_STRING POBTOOLS_VERSION_BASE "-" POBTOOLS_VERSION_SUFFIX
#else
#define POBTOOLS_VERSION_STRING POBTOOLS_VERSION_BASE
#endif
#define POBTOOLS_VERSION_BASE \
	PT_VER_STR(POBTOOLS_VER_MAJOR) "." PT_VER_STR(POBTOOLS_VER_MINOR) "." PT_VER_STR(POBTOOLS_VER_PATCH)

#endif
