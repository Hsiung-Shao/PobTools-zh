/* PobTools application version — the single source of truth.
 *
 * Consumed by C++ (launcher UI, app updater) and by rc.exe (pob-zh.rc
 * VERSIONINFO), so this header must stay plain #define with a classic
 * include guard (rc.exe does not understand #pragma once).
 *
 * Release checklist: bump these three numbers, add a matching entry to
 * changelog.h, rebuild pob-zh, then run tools/package_release.ps1
 * -Version <same version> (the script asserts the two match). Convention
 * consumed by the auto updater (app_update.cpp):
 * patch bump = translation-data-only release (applied silently);
 * minor/major bump = app release (update prompt in the launcher).
 */
#ifndef POBTOOLS_APP_VERSION_H
#define POBTOOLS_APP_VERSION_H

#define POBTOOLS_VER_MAJOR 0
#define POBTOOLS_VER_MINOR 4
#define POBTOOLS_VER_PATCH 0

#define PT_VER_STR2(x) #x
#define PT_VER_STR(x) PT_VER_STR2(x)
#define POBTOOLS_VERSION_STRING \
	PT_VER_STR(POBTOOLS_VER_MAJOR) "." PT_VER_STR(POBTOOLS_VER_MINOR) "." PT_VER_STR(POBTOOLS_VER_PATCH)

#endif
