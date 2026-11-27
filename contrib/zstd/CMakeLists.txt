# Minimal static libzstd for Assimp SPZ (NGSP v4). Sources under lib/.
cmake_minimum_required(VERSION 3.10)
project(assimp_zstd C)

set(ZSTD_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")

file(GLOB ASSIMP_ZSTD_COMMON "${ZSTD_LIB_DIR}/common/*.c")
file(GLOB ASSIMP_ZSTD_COMPRESS "${ZSTD_LIB_DIR}/compress/*.c")
file(GLOB ASSIMP_ZSTD_DECOMPRESS "${ZSTD_LIB_DIR}/decompress/*.c")
file(GLOB ASSIMP_ZSTD_DICT "${ZSTD_LIB_DIR}/dictBuilder/*.c")

add_library(assimp_zstd STATIC
  ${ASSIMP_ZSTD_COMMON}
  ${ASSIMP_ZSTD_COMPRESS}
  ${ASSIMP_ZSTD_DECOMPRESS}
  ${ASSIMP_ZSTD_DICT}
)

target_include_directories(assimp_zstd PUBLIC "${ZSTD_LIB_DIR}")
target_include_directories(assimp_zstd PRIVATE "${ZSTD_LIB_DIR}/common")
target_compile_definitions(assimp_zstd PRIVATE
  ZSTD_DISABLE_ASM
  ZSTD_MULTITHREAD=0
  XXH_NAMESPACE=ZSTD_
)
set_target_properties(assimp_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
