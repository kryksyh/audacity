# Vendors the muse_deps consume scripts into the source tree (deps_bundle/) so
# offline / sandboxed builds — notably Linux distro packaging with SYSTEM deps —
# need no network at configure time. Run when cutting a release source tarball
# (e.g. via the `bundle_deps` target), then source-package the tree.
#
#   cmake -P buildscripts/cmake/BundleDeps.cmake
#
# Reads the manifest (DependencyManifest.cmake) for MUSE_DEPS_URL and the dep
# list, and downloads each version-agnostic consume script. SYSTEM resolution
# then runs entirely offline (find_* against system libraries).

cmake_minimum_required(VERSION 3.16)

set(_self_dir "${CMAKE_CURRENT_LIST_DIR}")
if (NOT DEFINED BUNDLE_DIR)
    set(BUNDLE_DIR "${_self_dir}/deps_bundle")
endif()

# Bundle every dep regardless of the manifest's OS/option guards, so the
# resulting source tarball is portable across platforms.
set(OS_IS_LIN FALSE)
set(OS_IS_WIN FALSE)
set(AU_USE_LIBCURL ON)

function(require_dep name)
    set(dst "${BUNDLE_DIR}/${name}/${name}.cmake")
    file(DOWNLOAD "${MUSE_DEPS_URL}/${name}/${name}.cmake" "${dst}"
         HTTPHEADER "Cache-Control: no-cache" STATUS st)
    list(GET st 0 code)
    file(READ "${dst}" content)
    if (NOT code EQUAL 0 OR NOT content MATCHES "function\\(")
        message(FATAL_ERROR "[bundle_deps] failed to fetch ${name} (${st})")
    endif()
    message(STATUS "[bundle_deps] ${name}/${name}.cmake")
endfunction()

include("${_self_dir}/DependencyManifest.cmake")
message(STATUS "[bundle_deps] consume scripts vendored into ${BUNDLE_DIR}")
