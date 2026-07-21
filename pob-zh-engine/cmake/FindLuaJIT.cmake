# - Try to find luajit
# Once done this will define
#  LuaJIT_FOUND - System has luajit
#  LuaJIT_INCLUDE_DIRS - The luajit include directories
#  LuaJIT_LIBRARIES - The libraries needed to use luajit

if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(LuaJIT_SEARCH_ROOT ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET})

    find_path(LuaJIT_INCLUDE_DIR luajit.h
        PATHS ${LuaJIT_SEARCH_ROOT}/include
        PATH_SUFFIXES luajit
        NO_DEFAULT_PATH)

    find_library(LuaJIT_LIBRARY_RELEASE NAMES lua51
        PATHS ${LuaJIT_SEARCH_ROOT}
        PATH_SUFFIXES lib
        NO_DEFAULT_PATH)

    find_library(LuaJIT_LIBRARY_DEBUG NAMES lua51
        PATHS ${LuaJIT_SEARCH_ROOT}
        PATH_SUFFIXES debug/lib
        NO_DEFAULT_PATH)

    include(SelectLibraryConfigurations)
    select_library_configurations(LuaJIT)

    find_package_handle_standard_args(LuaJIT
        REQUIRED_VARS LuaJIT_LIBRARY LuaJIT_INCLUDE_DIR)
    mark_as_advanced(LuaJIT_INCLUDE_DIR LuaJIT_LIBRARY)

    if(LuaJIT_FOUND)
        set(LuaJIT_INCLUDE_DIRS ${LuaJIT_INCLUDE_DIR})

        if(NOT LuaJIT_LIBRARIES)
          set(LuaJIT_LIBRARIES ${LuaJIT_LIBRARY})
        endif()

        if(NOT TARGET LuaJIT::LuaJIT)
          add_library(LuaJIT::LuaJIT UNKNOWN IMPORTED)
          set_target_properties(LuaJIT::LuaJIT PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LuaJIT_INCLUDE_DIRS}")

          if(LuaJIT_LIBRARY_RELEASE)
            set_property(TARGET LuaJIT::LuaJIT APPEND PROPERTY
              IMPORTED_CONFIGURATIONS RELEASE)
            set_target_properties(LuaJIT::LuaJIT PROPERTIES
              IMPORTED_LOCATION_RELEASE "${LuaJIT_LIBRARY_RELEASE}")
          endif()

          if(LuaJIT_LIBRARY_DEBUG)
            set_property(TARGET LuaJIT::LuaJIT APPEND PROPERTY
              IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(LuaJIT::LuaJIT PROPERTIES
              IMPORTED_LOCATION_DEBUG "${LuaJIT_LIBRARY_DEBUG}")
          endif()

          if(NOT LuaJIT_LIBRARY_RELEASE AND NOT LuaJIT_LIBRARY_DEBUG)
            set_property(TARGET LuaJIT::LuaJIT APPEND PROPERTY
              IMPORTED_LOCATION "${LuaJIT_LIBRARY}")
          endif()
        endif()
    endif()
    unset(LuaJIT_SEARCH_ROOT)
else ()
    find_package(PkgConfig)
    if (PKG_CONFIG_FOUND)
      pkg_check_modules(PC_LUAJIT QUIET luajit)
    endif()

    set(LUAJIT_DEFINITIONS ${PC_LUAJIT_CFLAGS_OTHER})

    find_path(LUAJIT_INCLUDE_DIR luajit.h
              PATHS ${PC_LUAJIT_INCLUDEDIR} ${PC_LUAJIT_INCLUDE_DIRS}
              PATH_SUFFIXES luajit-2.0 luajit-2.1)

    if(MSVC)
      list(APPEND LUAJIT_NAMES lua51)
    elseif(MINGW)
      list(APPEND LUAJIT_NAMES libluajit libluajit-5.1)
    else()
      list(APPEND LUAJIT_NAMES luajit-5.1)
    endif()

    find_library(LUAJIT_LIBRARY NAMES ${LUAJIT_NAMES}
                 PATHS ${PC_LUAJIT_LIBDIR} ${PC_LUAJIT_LIBRARY_DIRS})

    set(LUAJIT_LIBRARIES ${LUAJIT_LIBRARY})
    set(LUAJIT_INCLUDE_DIRS ${LUAJIT_INCLUDE_DIR})

    include(FindPackageHandleStandardArgs)
    # handle the QUIETLY and REQUIRED arguments and set LUAJIT_FOUND to TRUE
    # if all listed variables are TRUE
    find_package_handle_standard_args(LuaJit DEFAULT_MSG
                                      LUAJIT_LIBRARY LUAJIT_INCLUDE_DIR)

    mark_as_advanced(LUAJIT_INCLUDE_DIR LUAJIT_LIBRARY)
endif()
