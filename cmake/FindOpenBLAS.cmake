# FindOpenBLAS.cmake — locate OpenBLAS via pkg-config first, system paths next
# Exposes the IMPORTED target OpenBLAS::OpenBLAS so callers can
# use target_link_libraries(... OpenBLAS::OpenBLAS) without poking include /
# library paths manually. MKL is explicitly forbidden by — this
# module never falls back to MKL.

if(TARGET OpenBLAS::OpenBLAS)
    return()
endif()

# ── 1. Try pkg-config (preferred path on Ubuntu / Debian) ────────────────────
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(_QPL_OPENBLAS_PC QUIET openblas)
endif()

set(_QPL_OPENBLAS_INCLUDE_HINTS
    ${_QPL_OPENBLAS_PC_INCLUDE_DIRS}
    /usr/include
    /usr/include/x86_64-linux-gnu
    /usr/include/openblas
    /usr/local/include
    /opt/OpenBLAS/include)

set(_QPL_OPENBLAS_LIBRARY_HINTS
    ${_QPL_OPENBLAS_PC_LIBRARY_DIRS}
    /usr/lib/x86_64-linux-gnu
    /usr/lib64
    /usr/lib
    /usr/local/lib
    /opt/OpenBLAS/lib)

# ── 2. Header and library search ─────────────────────────────────────────────
find_path(OpenBLAS_INCLUDE_DIR
    NAMES cblas.h openblas_config.h
    HINTS ${_QPL_OPENBLAS_INCLUDE_HINTS})

find_library(OpenBLAS_LIBRARY
    NAMES openblas
    HINTS ${_QPL_OPENBLAS_LIBRARY_HINTS})

# ── 3. Version extraction (best effort) ──────────────────────────────────────
set(OpenBLAS_VERSION "${_QPL_OPENBLAS_PC_VERSION}")
if(NOT OpenBLAS_VERSION AND OpenBLAS_INCLUDE_DIR AND EXISTS "${OpenBLAS_INCLUDE_DIR}/openblas_config.h")
    file(STRINGS "${OpenBLAS_INCLUDE_DIR}/openblas_config.h" _ver_line
         REGEX "OPENBLAS_VERSION")
    if(_ver_line MATCHES "OpenBLAS ([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(OpenBLAS_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenBLAS
    REQUIRED_VARS OpenBLAS_LIBRARY OpenBLAS_INCLUDE_DIR
    VERSION_VAR   OpenBLAS_VERSION)

# ── 4. IMPORTED target ───────────────────────────────────────────────────────
if(OpenBLAS_FOUND)
    add_library(OpenBLAS::OpenBLAS UNKNOWN IMPORTED)
    set_target_properties(OpenBLAS::OpenBLAS PROPERTIES
        IMPORTED_LOCATION "${OpenBLAS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OpenBLAS_INCLUDE_DIR}")
endif()

mark_as_advanced(OpenBLAS_INCLUDE_DIR OpenBLAS_LIBRARY)
