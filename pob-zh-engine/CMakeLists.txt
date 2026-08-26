cmake_minimum_required(VERSION 3.15)
project(SimpleGraphic C CXX)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

if (APPLE)
    enable_language(OBJCXX)
endif ()

if (MSVC)
    add_compile_options("/Zi")
    add_link_options("/DEBUG:FULL")
endif ()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(${PROJECT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake)

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "engine")
include(InstallRequiredSystemLibraries)
add_subdirectory(dep/glm)

set(SIMPLEGRAPHIC_SOURCES
    "config.h"
    "dep/stb/stb_image.h"
    "dep/stb/stb_image_resize.h"
    "dep/stb/stb_image_write.h"
    "engine/common.h"
    "engine/common/base64.c"
    "engine/common/base64.h"
    "engine/common/common.cpp"
    "engine/common/console.cpp"
    "engine/common/console.h"
    "engine/common/keylist.h"
    "engine/common/memtrak3.cpp"
    "engine/common/memtrak3.h"
    "engine/common/streams.cpp"
    "engine/common/streams.h"
    "engine/core/core_compress.cpp"
    "engine/core/core_compress.h"
    "engine/core/core_config.cpp"
    "engine/core/core_config.h"
    "engine/core/core_image.cpp"
    "engine/core/core_image.h"
    "engine/core/core_main.cpp"
    "engine/core/core_main.h"
    "engine/core/core_tex_manipulation.cpp"
    "engine/core/core_tex_manipulation.h"
    "engine/core/core_video.cpp"
    "engine/core/core_video.h"
    "engine/render/r_font.cpp"
    "engine/render/r_font.h"
    "engine/render/r_main.cpp"
    "engine/render/r_main.h"
    "engine/render/r_texture.cpp"
    "engine/render/r_texture.h"
    "engine/system/win/sys_local.h"
    "engine/system/win/sys_main.cpp"
    "engine/system/win/sys_opengl.cpp"
    "engine/system/win/sys_video.cpp"
    "engine/system/sys_console.h"
    "engine/system/sys_main.h"
    "engine/system/sys_opengl.h"
    "engine/system/sys_video.h"
    "win/entry.cpp"
    "ui.h"
    "ui_api.cpp"
    # Shared failure log. Compiled into BOTH targets on purpose: the engine
    # cannot call into the host, and the POB patch failures worth recording
    # happen in ui_api.cpp. Appends are atomic, so two copies is not a race.
    "host/error_log.cpp"
    "ui_console.cpp"
    "ui_console.h"
    "ui_debug.cpp"
    "ui_debug.h"
    "ui_local.h"
    "ui_main.cpp"
    "ui_main.h"
    "ui_subscript.cpp"
    "ui_subscript.h"
    "translate/translation_manager.cpp"
    "translate/translation_manager.h"
    "translate/startup_trace.cpp"
    "translate/startup_trace.h"
)

set (SIMPLEGRAPHIC_PLATFORM_SOURCES)
if (APPLE)
    set (SIMPLEGRAPHIC_PLATFORM_SOURCES
        "engine/system/win/sys_macos.mm"
    )
endif()

if (WIN32)
    set (SIMPLEGRAPHIC_PLATFORM_SOURCES
        "engine/system/win/sys_console.cpp"
        "SimpleGraphic.rc"
    )
else()
    set (SIMPLEGRAPHIC_PLATFORM_SOURCES
        "engine/system/win/sys_console_unix.cpp"
    )
endif()

add_library(SimpleGraphic SHARED
    ${SIMPLEGRAPHIC_SOURCES}
    ${SIMPLEGRAPHIC_PLATFORM_SOURCES}
)
target_compile_definitions(SimpleGraphic
    PRIVATE
    "UNICODE"
    "_CRT_SECURE_NO_DEPRECATE"
    "_CRT_SECURE_NO_WARNINGS"
    "_SCL_SECURE_NO_DEPRECATE"
    "_SCL_SECURE_NO_WARNINGS"
    "GLFW_INCLUDE_NONE"
    "GL_SILENCE_DEPRECATION"
    "SIMPLEGRAPHIC_EXPORTS"
)

target_include_directories(SimpleGraphic
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine"
)

find_package(unofficial-angle CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(gli CONFIG REQUIRED)
find_package(LuaJIT REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Freetype REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(zstd REQUIRED)
find_package(ZLIB REQUIRED)
find_package(WebP)

add_library(cmp_core STATIC
    dep/compressonator/cmp_core/source/cmp_core.cpp
    dep/compressonator/cmp_core/source/cmp_core.h
    dep/compressonator/cmp_core/shaders/bc3_encode_kernel.cpp
    dep/compressonator/cmp_core/shaders/bc7_encode_kernel.cpp
)

target_include_directories(cmp_core PRIVATE
    dep/compressonator/applications/_libs/cmp_math
)

target_include_directories(cmp_core PUBLIC
    dep/compressonator/cmp_core/shaders
    dep/compressonator/cmp_core/source
)

# Shared between the SimpleGraphic target (imgui static lib, /MD) and the
# pob-zh host exe, which compiles these sources directly so they pick up the
# exe's static CRT (/MT) — see the pob-zh target below.
set(IMGUI_SOURCES
    dep/imgui/imconfig.h
    dep/imgui/imgui.cpp
    dep/imgui/imgui.h
    dep/imgui/imgui_demo.cpp
    dep/imgui/imgui_draw.cpp
    dep/imgui/imgui_internal.h
    dep/imgui/imgui_tables.cpp
    dep/imgui/imgui_widgets.cpp
    dep/imgui/imstb_rectpack.h
    dep/imgui/imstb_textedit.h
    dep/imgui/imstb_truetype.h
    dep/imgui/backends/imgui_impl_glfw.cpp
    dep/imgui/backends/imgui_impl_glfw.h
    dep/imgui/backends/imgui_impl_opengl3.cpp
    dep/imgui/backends/imgui_impl_opengl3.h
    dep/imgui/backends/imgui_impl_opengl3_loader.h
    dep/imgui/misc/cpp/imgui_stdlib.cpp
    dep/imgui/misc/cpp/imgui_stdlib.h
)

add_library(imgui STATIC ${IMGUI_SOURCES})

target_compile_definitions(imgui PUBLIC
    "IMGUI_IMPL_OPENGL_ES2"
)

target_include_directories(imgui PUBLIC
    dep/imgui
    dep/imgui/backends
    dep/imgui/misc/cpp
)

target_link_libraries(imgui PUBLIC
    unofficial::angle::libGLESv2
    glfw
)

target_include_directories(SimpleGraphic
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dep/glad/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dep/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/nlohmann
    ${CMAKE_CURRENT_SOURCE_DIR}/translate
)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(SimpleGraphic
        PRIVATE
        "-export-dynamic"
    )
endif ()

if (WIN32)
    target_compile_options(SimpleGraphic
        PRIVATE
        "/EHa"
        # Same reason as the pob-zh target below: ui_api.cpp now carries UTF-8
        # CJK literals (what each POB patch is FOR, so the failure log can say it
        # in the user's words). Without this MSVC reads the file as Big5, where a
        # trailing 0x5C escapes the closing quote -- the error it produces is
        # "newline in constant", which points nowhere near the real problem.
        # Verified safe: before this change the engine had ZERO non-ASCII string
        # literals; every other non-ASCII byte in these sources is in a comment.
        "/utf-8"
    )
    target_compile_definitions(SimpleGraphic
        PRIVATE
        "_CRT_SECURE_NO_DEPRECATE=1"
        "_CRT_SECURE_NO_WARNINGS=1"
        "_SCL_SECURE_NO_DEPRECATE=1"
        "_SCL_SECURE_NO_WARNINGS=1"
    )
    target_link_libraries(SimpleGraphic
        PRIVATE
        "winmm.lib"
    )
endif ()

if (APPLE)
    find_library(CORE_FOUNDATION_LIBRARY CoreFoundation)
    find_library(APPLICATION_SERVICES_LIBRARY ApplicationServices)
    target_link_libraries(SimpleGraphic
        PRIVATE
        ${CORE_FOUNDATION_LIBRARY}
        ${APPLICATION_SERVICES_LIBRARY}
    )
endif ()

target_link_libraries(SimpleGraphic
    PRIVATE
    unofficial::angle::libEGL
    unofficial::angle::libGLESv2
    cmp_core
    fmt::fmt
    Freetype::Freetype
    glfw
    gli
    glm::glm
    imgui
    LuaJIT::LuaJIT
    Microsoft.GSL::GSL
    re2::re2
    sol2
    Threads::Threads
    WebP::webpdecoder
    ZLIB::ZLIB
    zstd::libzstd_shared
)

install(FILES $<TARGET_RUNTIME_DLLS:SimpleGraphic> DESTINATION "engine")
install(TARGETS SimpleGraphic RUNTIME DESTINATION "engine")

# PoeCharm: engine-owned runtime CJK patch script, loaded by the engine into
# POB's Lua state after OnInit. Ships in engine/ (never the POB folder).
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/poecharm_inject.lua" DESTINATION "engine")

# Fallback pure-Lua runtime modules. POB's built-in updater does not ship new
# runtime lua files (e.g. sha2.lua, required by PoEAPI since POB 2.66), so a
# user who updated POB in place lacks them. package.path prefers the POB
# folder's own copy (engine/lua is appended last in ui_api.cpp InitAPI).
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_lua/sha2.lua" DESTINATION "engine/lua")

if (WIN32)
    set(DEPS_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

    if (NOT VCPKG_TARGET_TRIPLET MATCHES "-release")
        find_file(LuaJIT_DLL_DEBUG NAMES "lua51.dll"
            PATHS "${DEPS_DIR}" PATH_SUFFIXES "debug/bin" REQUIRED NO_DEFAULT_PATH)
        find_file(ZLIB_DLL_DEBUG NAMES "zlibd1.dll"
            PATHS "${DEPS_DIR}" PATH_SUFFIXES "debug/bin" REQUIRED NO_DEFAULT_PATH)

        install(FILES
            ${LuaJIT_DLL_DEBUG}
            ${ZLIB_DLL_DEBUG}
            DESTINATION "engine"
            CONFIGURATIONS Debug
        )
    endif ()

    find_file(LuaJIT_DLL_RELEASE NAMES "lua51.dll"
        PATHS "${DEPS_DIR}" PATH_SUFFIXES "bin" REQUIRED NO_DEFAULT_PATH)
    find_file(ZLIB_DLL_RELEASE NAMES "zlib1.dll"
        PATHS "${DEPS_DIR}" PATH_SUFFIXES "bin" REQUIRED NO_DEFAULT_PATH)

    # freetype's private vcpkg deps; $<TARGET_RUNTIME_DLLS> cannot see them,
    # so install them explicitly or SimpleGraphic.dll fails to load at runtime.
    find_file(BZ2_DLL_RELEASE NAMES "bz2.dll"
        PATHS "${DEPS_DIR}" PATH_SUFFIXES "bin" REQUIRED NO_DEFAULT_PATH)
    find_file(PNG_DLL_RELEASE NAMES "libpng16.dll"
        PATHS "${DEPS_DIR}" PATH_SUFFIXES "bin" REQUIRED NO_DEFAULT_PATH)
    find_file(BROTLIDEC_DLL_RELEASE NAMES "brotlidec.dll"
        PATHS "${DEPS_DIR}" PATH_SUFFIXES "bin" REQUIRED NO_DEFAULT_PATH)
    find_file(BROTLICOMMON_DLL_RELEASE NAMES "brotlicommon.dll"
        PATHS "${DEPS_DIR}" PATH_SUFFIXES "bin" REQUIRED NO_DEFAULT_PATH)

    install(FILES
        ${LuaJIT_DLL_RELEASE}
        ${ZLIB_DLL_RELEASE}
        ${BZ2_DLL_RELEASE}
        ${PNG_DLL_RELEASE}
        ${BROTLIDEC_DLL_RELEASE}
        ${BROTLICOMMON_DLL_RELEASE}
        DESTINATION "engine"
        CONFIGURATIONS Release MinSizeRel RelWithDebInfo
    )
endif ()


# pob-zh host exe: ImGui launcher UI + thin loader that loads SimpleGraphic.dll
# and runs an external POB Launch.lua in a child process.
#
# The exe lives in the dist ROOT while every DLL lives in engine\, so:
#   - static CRT (/MT): no msvcp140/vcruntime load-time imports
#   - imgui sources are compiled into the exe (the imgui static lib is /MD)
#   - glfw3/libGLESv2 are DELAYLOADed; wWinMain calls SetDllDirectoryW(engine)
#     before the first glfw/GL call so they resolve from engine\
if (WIN32)
    add_executable(pob-zh WIN32
        host/pob-zh.rc
        host/host_main.cpp
        host/error_log.cpp
        host/error_log_selftest.cpp
        host/launcher_config.cpp
        host/launcher_strings_io.cpp
        host/ui_theme.cpp
        host/launcher_ui.cpp
        host/launcher_editor.cpp
        host/editor_data.cpp
        host/editor_selftest.cpp
        host/paste_selftest.cpp
        # POB 執行時拼出來的物品名(「<標題>, <基底>」)的正向翻譯回歸
        host/item_name_selftest.cpp
        host/paste_trace.cpp
        # 多佔位符詞條的填位回歸 + 全量 A/B 快照工具
        host/placeholder_selftest.cpp
        host/pob_launch.cpp
        host/window_manager.cpp
        host/window_dock.cpp
        # the editor selftest asks the REAL loader what the engine would
        # return; without this it could only re-implement the merge and
        # would be testing its own copy of the rule instead of the engine
        translate/translation_manager.cpp
        translate/startup_trace.cpp
        host/filter_parser.cpp
        host/filter_doc_editor.cpp
        host/filter_selftest.cpp
        host/filter_schema.cpp
        host/filter_card_ui.cpp
        host/filter_batch.cpp
        host/custom_rules_io.cpp
        host/sound_library_service.cpp
        host/section_filteredit.cpp
        host/section_sounds.cpp
        host/filter_preview.cpp
        host/filter_item_import.cpp
        host/filter_data.cpp
        host/filter_i18n.cpp
        host/image_tex.cpp
        # dropped from the build when the old filter tier-list view was retired;
        # back in for the atlas planner's scarab icons
        host/icon_manager.cpp
        host/item_library.cpp
        host/audio_player.cpp
        host/sound_manager.cpp
        host/clipboard_util.cpp
        host/editor_util.cpp
        host/editor_shell.cpp
        host/filter_editor.cpp
        # one panel's content, drawn either into its own window (tool_window) or
        # into a launcher tab -- the per-frame code exists once for both
        host/tool_window.cpp
        # drives a panel exactly as a launcher tab does; no other selftest ever
        # creates an ImGui frame, so nothing else can see that path
        host/panel_selftest.cpp
        host/atlas_tree_data.cpp
        host/atlas_view.cpp
        host/atlas_planner.cpp
        host/atlas_import.cpp
        host/atlas_update.cpp
        host/atlas_stat_agg.cpp
        host/atlas_scarabs.cpp
        host/atlas_astrolabes.cpp
        host/atlas_maps.cpp
        # league-mechanic grouping behind "show me every 裂痕 cluster"; the map
        # itself is generated per season by the updater/importer, not shipped as
        # a hand-maintained data file
        host/atlas_mechanics.cpp
        # shared by the scarab / astrolabe / map pickers
        host/fuzzy_match.cpp
        host/atlas_optimize.cpp
        host/atlas_persist.cpp
        host/atlas_i18n.cpp
        host/atlas_version_index.cpp
        host/atlas_diff.cpp
        host/timeless_jewel.cpp
        # Abyss jewels are keyed by socket, not by radius; separate engine
        host/timeless_jewel_abyss.cpp
        host/timeless_jewel_ui.cpp
        host/passive_tree_data.cpp
        host/passive_tree_view.cpp
        host/passive_import.cpp
        host/passive_tree_update.cpp
        host/app_update.cpp
        host/zip_extract.cpp
        host/hash_sha256.cpp
        host/sig_verify.cpp
        # 搜尋字串產生器：核心 + 資料 + UI + 自檢
        host/regex_gen.cpp
        host/regex_data.cpp
        host/regex_state.cpp
        host/regex_tool_ui.cpp
        host/regex_selftest.cpp
        dep/miniz/miniz.c
        engine/common/base64.c
        host/http_client.cpp
        ${IMGUI_SOURCES}
    )
    target_compile_definitions(pob-zh PRIVATE "UNICODE" "_UNICODE" "IMGUI_IMPL_OPENGL_ES2")
    # 更新來源的 repo。不設就是 app_update.cpp 裡的正式值。
    #
    # 存在的理由:「簽章會放行正確的更新」只能對著一個真的 release 證明,而拿正式
    # repo 做那個實驗會讓全體使用者的客戶端看到測試用的 release —— 尤其 Data 線掃
    # /releases 全清單、prerelease 不會被跳過。
    #
    #   cmake -B build -DPOBTOOLS_UPDATE_REPO=你的帳號/測試repo
    #   cmake -B build -UPOBTOOLS_UPDATE_REPO      # 改回正式值
    #
    # ⚠ 這是編譯期的,執行期改不了 —— 更新來源是安全邊界的一部分。
    # ⚠ 打包腳本 Assert-Step 16 會問 exe 自己(--update-source)並斷言是正式 repo,
    #    所以測試建置不可能被打包發出去。
    if(POBTOOLS_UPDATE_REPO)
        message(STATUS "pob-zh: 更新來源改為 ${POBTOOLS_UPDATE_REPO}(測試建置,不可發布)")
        target_compile_definitions(pob-zh PRIVATE "POBTOOLS_UPDATE_REPO=\"${POBTOOLS_UPDATE_REPO}\"")
    endif()
    # host sources contain UTF-8 CJK string literals; force UTF-8 source parsing
    # so MSVC does not misread them under the system (Big5) code page.
    target_compile_options(pob-zh PRIVATE "/utf-8")
    target_include_directories(pob-zh PRIVATE
        dep/imgui
        dep/imgui/backends
        dep/imgui/misc/cpp
        dep/stb
        dep/miniz
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/nlohmann
    )
    set_property(TARGET pob-zh PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(pob-zh PRIVATE glfw unofficial::angle::libGLESv2 delayimp)
    target_link_options(pob-zh PRIVATE "/DELAYLOAD:glfw3.dll" "/DELAYLOAD:libGLESv2.dll")
    set_target_properties(pob-zh PROPERTIES OUTPUT_NAME "pob-zh")
    install(TARGETS pob-zh RUNTIME DESTINATION ".")
    # Bundled base-type -> item-class map (generated from POB Data/Bases) and
    # item-name -> icon art-path map (generated from RePoE).
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/base_classes.json" DESTINATION "Data")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/icon_paths.json" DESTINATION "Data")
    # Static economy prices + value->tier thresholds (offline fallback; live in 5b).
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/economy_poe1.json" DESTINATION "Data")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/economy_tiers_poe1.json" DESTINATION "Data")
    # Complete Traditional-Chinese item dataset (repoe-fork: names + class + tags).
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/filter_items_zh.json" DESTINATION "Data")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/item_meta.json" DESTINATION "Data")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/item_classes_zh.json" DESTINATION "Data")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/item_classes_en.json" DESTINATION "Data")
    # Bundled starter filter (NeverSink POE1) for the "load default template" preset.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/default.filter" DESTINATION "Filters")
    # PoE1 atlas passive tree (generated by tools/gen_atlas_tree.py) + sprite sheets.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/timeless_jewels.json" DESTINATION "Data")
    # PoE1 character passive tree geometry for the Timeless Jewel tree view
    # (tools/gen_passive_tree.py). Sprite SHEETS are read from the user's POB
    # TreeData folder at runtime, never bundled.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/passive_tree_poe1.json" DESTINATION "Data")
    # Bundled passive-tree sprite sheets (zoom -3, from GGG skilltree-export):
    # lets the jewel tree view draw a league PoB hasn't shipped yet; the view
    # still falls back to the user's PoB TreeData/<ver>/ when a sheet is absent.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/host/data/tree/" DESTINATION "Data/tree")
    # PoE1 atlas passive tree — versioned layout: two seasons live side by side
    # under Data/atlas_versions/<tag>/ (condensed tree + EN/TC node mapping +
    # sprite sheets, each self-consistent) so the planner can diff them.
    # atlas_index.json registers the installed seasons and the active/compare
    # selection; the in-app updater installs new seasons and prunes to two.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/host/data/atlas_versions/" DESTINATION "Data/atlas_versions")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/atlas_index.json" DESTINATION "Data")
    # Scarab catalogue for the planner's map-device section: names + official
    # effect text in both locales, art paths, and the placement rules
    # (per-scarab copy limit, mutual-exclusion family). Regenerated from a local
    # Content.ggpk by tools/ggpk_zh/gen_scarabs.py --verify; unlike the atlas
    # tree it has no online updater, so a new league needs a fresh run.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/scarabs_poe1.json" DESTINATION "Data")
    # Astrolabe catalogue (3.29 Shaped Regions) and the atlas map list behind the
    # planner's quadrant slots and main-map pick. Regenerated from a local
    # Content.ggpk by tools/ggpk_zh/gen_astrolabes.py --verify and
    # gen_atlas_maps.py --verify. Like the scarabs these have NO online updater,
    # so a new league needs a fresh run of both.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/astrolabes_poe1.json" DESTINATION "Data")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/atlas_maps_poe1.json" DESTINATION "Data")
    # Search-string generator catalogue: every list the tool can build a query
    # over (map and logbook modifiers for PoE1; waystone, tablet, relic and
    # remnant modifiers for PoE2), each entry carrying the line the game prints
    # with '#' where a roll goes. Regenerated from a local Content.ggpk by
    # tools/gen_regex_data.py and gen_regex_data2.py; like the scarabs neither has
    # an online updater, so a new league needs a fresh run.
    # BOTH files ship: the panel loads every catalogue it finds and labels each
    # page with its game, so a PoE1 player can still read the PoE2 list.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/regex_poe1.json" DESTINATION "Data")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/regex_poe2.json" DESTINATION "Data")
    # The shipped CJK font. Until 2026-08 this file existed ONLY in dist\Fonts\,
    # which is gitignored and has no install rule -- so a `cmake --install` into a
    # fresh prefix, or any cleanup of dist\, silently lost it. Nothing reported the
    # loss either: the launcher falls through ResolveFontPath to the next candidate
    # and the engine to its CJK fallback, so the first symptom is missing glyphs on
    # someone else's machine. host/data/fonts/ is now the single source, mirrored by
    # the sha256 assertion in tools/package_release.ps1.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host/data/fonts/NotoSansTC-Regular.ttf" DESTINATION "Fonts")
endif ()


# lcurl module

set(LCURL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/Lua-cURLv3)
file(GLOB LCURL_SOURCES ${LCURL_SOURCE_DIR}/src/**.c)
add_library(lcurl SHARED ${LCURL_SOURCES})

target_include_directories(lcurl
    PRIVATE
    ${LCURL_SOURCE_DIR}/src
)

target_link_libraries(lcurl
    PRIVATE
    CURL::libcurl
    LuaJIT::LuaJIT
)

install(TARGETS lcurl RUNTIME DESTINATION "engine")
install(FILES $<TARGET_RUNTIME_DLLS:lcurl> DESTINATION "engine")

# luautf8 module

add_library(lua-utf8 SHARED libs/luautf8/lutf8lib.c)

target_compile_definitions(lua-utf8
    PRIVATE
    LUA_BUILD_AS_DLL
)

target_include_directories(lua-utf8
    PRIVATE
)

target_link_libraries(lua-utf8
    PRIVATE
    LuaJIT::LuaJIT
)

install(TARGETS lua-utf8 RUNTIME DESTINATION "engine")
install(FILES $<TARGET_RUNTIME_DLLS:lua-utf8> DESTINATION "engine")

# luasocket module

add_library(luasocket SHARED
   "libs/luasocket/src/auxiliar.c"
   "libs/luasocket/src/buffer.c"
   "libs/luasocket/src/compat.c"
   "libs/luasocket/src/except.c"
   "libs/luasocket/src/inet.c"
   "libs/luasocket/src/io.c"
   "libs/luasocket/src/luasocket.c"
   "libs/luasocket/src/options.c"
   "libs/luasocket/src/select.c"
   "libs/luasocket/src/tcp.c"
   "libs/luasocket/src/timeout.c"
   "libs/luasocket/src/udp.c"
   "libs/luasocket/src/wsocket.c"
)

target_include_directories(luasocket
    PRIVATE
    ${LSOCKET_SOURCE_DIR}/src
)

target_link_libraries(luasocket
    PRIVATE
    LuaJIT::LuaJIT
    wsock32
    ws2_32
)

set_target_properties( luasocket PROPERTIES OUTPUT_NAME "socket" ) 
install(TARGETS luasocket RUNTIME DESTINATION "engine")
install(FILES $<TARGET_RUNTIME_DLLS:luasocket> DESTINATION "engine")

# lzip module

add_library(lzip SHARED libs/LZip/lzip.cpp)

target_include_directories(lzip
    PRIVATE
)

target_link_libraries(lzip
    PRIVATE
    LuaJIT::LuaJIT
    ZLIB::ZLIB
)

install(TARGETS lzip RUNTIME DESTINATION "engine")
install(FILES $<TARGET_RUNTIME_DLLS:lzip> DESTINATION "engine")

# Headless check of the engine's fallback font chain against the shipped TTFs
# (engine/render/r_font_fallback.h). Not installed, not built by default:
#   cmake --build build --config Release --target font_fallback_selftest
#   $env:PATH = "dist\engine;$env:PATH"; build\Release\font_fallback_selftest.exe dist\Fonts
add_executable(font_fallback_selftest EXCLUDE_FROM_ALL tests/font_fallback_selftest.cpp)
target_link_libraries(font_fallback_selftest PRIVATE Freetype::Freetype)
if (MSVC)
    target_compile_options(font_fallback_selftest PRIVATE "/utf-8")
endif ()
