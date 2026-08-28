set(extra_patches "")
if (VCPKG_TARGET_IS_OSX)
	list(APPEND extra_patches 005-do-not-pass-ld-e-macosx.patch)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO LuaJIT/LuaJIT
    REF 2460b3ff93a1c955de3d62cfc825de7d68dc272e  #2026-07-20
    SHA512 f7b35a4690b5bfc847dc9f94288cc7c51b7a3bd84a793b5841889ecc7dc2d60b67b3b9f31f3e713a0c8867a719c27b129289f6d90d64dccf54595e270c6c2977
    HEAD_REF v2.1
    PATCHES
        msvcbuild.patch
        003-do-not-set-macosx-deployment-target.patch
        pob-wide-crt.patch
        ${extra_patches}
)

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

if(VCPKG_DETECTED_MSVC)
    # Due to lack of better MSVC cross-build support, just always build the host
    # minilua tool with the target toolchain. This will work for native builds and
    # for targeting x86 from x64 hosts. (UWP and ARM64 is unsupported.)
    vcpkg_list(SET options)
    set(PKGCONFIG_CFLAGS "")
    if (VCPKG_LIBRARY_LINKAGE STREQUAL "static")
        list(APPEND options "MSVCBUILD_OPTIONS=static")
    else()
        set(PKGCONFIG_CFLAGS "/DLUA_BUILD_AS_DLL=1")
    endif()

    vcpkg_install_nmake(SOURCE_PATH "${SOURCE_PATH}"
        PROJECT_NAME "${CMAKE_CURRENT_LIST_DIR}/Makefile.nmake"
        OPTIONS
            ${options}
    )

    configure_file("${CMAKE_CURRENT_LIST_DIR}/luajit.pc.win.in" "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/luajit.pc" @ONLY)
    if(NOT VCPKG_BUILD_TYPE)
        configure_file("${CMAKE_CURRENT_LIST_DIR}/luajit.pc.win.in" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/luajit.pc" @ONLY)
    endif()

    vcpkg_copy_pdbs()
else()
    vcpkg_list(SET options)
    if(VCPKG_CROSSCOMPILING)
        list(APPEND options
            "LJARCH=${VCPKG_TARGET_ARCHITECTURE}"
            "BUILDVM_X=${CURRENT_HOST_INSTALLED_DIR}/manual-tools/${PORT}/buildvm-${VCPKG_TARGET_ARCHITECTURE}${VCPKG_HOST_EXECUTABLE_SUFFIX}"
        )
    endif()

    vcpkg_list(SET make_options "EXECUTABLE_SUFFIX=${VCPKG_TARGET_EXECUTABLE_SUFFIX}")
    set(strip_options "") # cf. src/Makefile
    if(VCPKG_TARGET_IS_OSX)
        vcpkg_list(APPEND make_options "TARGET_SYS=Darwin")
        set(strip_options " -x")
    elseif(VCPKG_TARGET_IS_IOS)
        vcpkg_list(APPEND make_options "TARGET_SYS=iOS")
        set(strip_options " -x")
    elseif(VCPKG_TARGET_IS_LINUX)
        vcpkg_list(APPEND make_options "TARGET_SYS=Linux")
    elseif(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_list(APPEND make_options "TARGET_SYS=Windows")
        set(strip_options " --strip-unneeded")
    endif()

    set(dasm_archs "")
    if("buildvm-32" IN_LIST FEATURES)
        string(APPEND dasm_archs " arm x86")
    endif()
    if("buildvm-64" IN_LIST FEATURES)
        string(APPEND dasm_archs " arm64 x64")
    endif()

    file(COPY "${CMAKE_CURRENT_LIST_DIR}/configure" DESTINATION "${SOURCE_PATH}")
    vcpkg_configure_make(SOURCE_PATH "${SOURCE_PATH}"
        COPY_SOURCE
        OPTIONS
            "BUILDMODE=${VCPKG_LIBRARY_LINKAGE}"
            ${options}
        OPTIONS_RELEASE
            "DASM_ARCHS=${dasm_archs}"
    )
    vcpkg_install_make(
        MAKEFILE "Makefile.vcpkg"
        OPTIONS
            ${make_options}
            "TARGET_AR=${VCPKG_DETECTED_CMAKE_AR} rcus"
            "TARGET_STRIP=${VCPKG_DETECTED_CMAKE_STRIP}${strip_options}"
    )
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/lib/lua"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/lib/lua"
    "${CURRENT_PACKAGES_DIR}/share/lua"
    "${CURRENT_PACKAGES_DIR}/share/man"
)

vcpkg_copy_tools(TOOL_NAMES luajit AUTO_CLEAN)

vcpkg_fixup_pkgconfig()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYRIGHT")
