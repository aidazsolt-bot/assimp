# Idempotent TinyUSDZ patch apply for FetchContent PATCH_COMMAND.
# Usage: cmake -DTINYUSDZ_SRC=<repo-src> -DTINYUSDZ_PATCH_DIR=<patches> -P apply-tinyusdz-patches.cmake
if (NOT TINYUSDZ_SRC OR NOT TINYUSDZ_PATCH_DIR)
    message(FATAL_ERROR "TINYUSDZ_SRC and TINYUSDZ_PATCH_DIR required")
endif ()
find_package(Git REQUIRED)
set(_prim "${TINYUSDZ_SRC}/src/prim-types.hh")
set(_already FALSE)
if (EXISTS "${_prim}")
    file(READ "${_prim}" _prim_txt)
    string(FIND "${_prim_txt}" "kilogramsPerUnit" _kg)
    if (NOT _kg EQUAL -1)
        set(_already TRUE)
    endif ()
endif ()
if (_already)
    message(STATUS "TinyUSDZ patches already present - skip git apply")
    return()
endif ()
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply "${TINYUSDZ_PATCH_DIR}/tinyusdz.patch"
    WORKING_DIRECTORY "${TINYUSDZ_SRC}"
    RESULT_VARIABLE _r1
    ERROR_VARIABLE _e1)
if (NOT _r1 EQUAL 0)
    message(FATAL_ERROR "git apply tinyusdz.patch failed: ${_e1}")
endif ()
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply "${TINYUSDZ_PATCH_DIR}/tinyusdz-unknown-metas.patch"
    WORKING_DIRECTORY "${TINYUSDZ_SRC}"
    RESULT_VARIABLE _r2
    ERROR_VARIABLE _e2)
if (NOT _r2 EQUAL 0)
    message(FATAL_ERROR "git apply tinyusdz-unknown-metas.patch failed: ${_e2}")
endif ()
message(STATUS "TinyUSDZ patches applied")
