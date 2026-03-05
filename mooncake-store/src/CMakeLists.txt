# Find Python package
add_subdirectory(cachelib_memory_allocator)

option(STORE_USE_SPDK "Enable SPDK 23.01 NVMeoF(TCP) data path in mooncake-store" OFF)

set(MOONCAKE_STORE_SOURCES
    allocator.cpp
    master_service.cpp
    client_service.cpp
    client_metric.cpp
    types.cpp
    master_client.cpp
    utils.cpp
    master_metric_manager.cpp
    storage_backend.cpp
    thread_pool.cpp
    etcd_helper.cpp
    ha_helper.cpp
    segment.cpp
    transfer_task.cpp
    etcd_helper.cpp
    ha_helper.cpp
    rpc_service.cpp
    offset_allocator.cpp
    posix_file.cpp
    client_buffer.cpp
    aligned_client_buffer.cpp
    ssd/ssd_io_engine.cpp
    real_client.cpp
    dummy_client.cpp
    shm_helper.cpp
    http_metadata_server.cpp
    file_storage.cpp
    serialize/serializer.cpp
    serialize/serializer_backend.cpp
    utils/type_util.cpp
    utils/file_util.cpp
    task_manager.cpp
    local_hot_cache.cpp
)

set(EXTRA_LIBS "")
set(STORE_SPDK_LIBS "")
set(MOONCAKE_STORE_USE_SPDK 0)

if (STORE_USE_SPDK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SPDK_NVME QUIET IMPORTED_TARGET spdk_nvme)
  pkg_check_modules(SPDK_ENV_DPDK QUIET IMPORTED_TARGET spdk_env_dpdk)
  if (NOT SPDK_NVME_FOUND)
    set(_LOCAL_SPDK_PKGCONFIG_DIR
        "${CMAKE_SOURCE_DIR}/extern/spdk-23.01/build/lib/pkgconfig")
    if (EXISTS "${_LOCAL_SPDK_PKGCONFIG_DIR}/spdk_nvme.pc")
      set(ENV{PKG_CONFIG_PATH}
          "${_LOCAL_SPDK_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
      pkg_check_modules(SPDK_NVME QUIET IMPORTED_TARGET spdk_nvme)
      pkg_check_modules(SPDK_ENV_DPDK QUIET IMPORTED_TARGET spdk_env_dpdk)
    endif()
  endif()
  if (NOT SPDK_NVME_FOUND)
    message(FATAL_ERROR
      "STORE_USE_SPDK=ON requires pkg-config package 'spdk_nvme' from SPDK 23.01.x. "
      "Build/install SPDK and export PKG_CONFIG_PATH (for example from extern/spdk-23.01/build/lib/pkgconfig).")
  endif()
  if (NOT SPDK_ENV_DPDK_FOUND)
    message(FATAL_ERROR
      "STORE_USE_SPDK=ON requires pkg-config package 'spdk_env_dpdk' from SPDK 23.01.x. "
      "Build/install SPDK env library and export PKG_CONFIG_PATH.")
  endif()

  set(_SPDK_EFFECTIVE_VERSION "${SPDK_NVME_VERSION}")
  set(_SPDK_VERSION_HEADER "")
  set(_SPDK_VERSION_HEADER_CANDIDATES ${SPDK_NVME_INCLUDE_DIRS})
  if (SPDK_NVME_INCLUDEDIR)
    list(APPEND _SPDK_VERSION_HEADER_CANDIDATES "${SPDK_NVME_INCLUDEDIR}")
  endif()
  foreach(_SPDK_INC_DIR IN LISTS _SPDK_VERSION_HEADER_CANDIDATES)
    if (EXISTS "${_SPDK_INC_DIR}/spdk/version.h")
      set(_SPDK_VERSION_HEADER "${_SPDK_INC_DIR}/spdk/version.h")
      break()
    endif()
  endforeach()

  if (_SPDK_VERSION_HEADER)
    file(STRINGS "${_SPDK_VERSION_HEADER}" _SPDK_MAJOR_LINE
      REGEX "^#define[ \t]+SPDK_VERSION_MAJOR[ \t]+[0-9]+")
    file(STRINGS "${_SPDK_VERSION_HEADER}" _SPDK_MINOR_LINE
      REGEX "^#define[ \t]+SPDK_VERSION_MINOR[ \t]+[0-9]+")
    string(REGEX REPLACE ".*SPDK_VERSION_MAJOR[ \t]+([0-9]+).*" "\\1"
      _SPDK_MAJOR "${_SPDK_MAJOR_LINE}")
    string(REGEX REPLACE ".*SPDK_VERSION_MINOR[ \t]+([0-9]+).*" "\\1"
      _SPDK_MINOR "${_SPDK_MINOR_LINE}")
    if (_SPDK_MAJOR_LINE AND _SPDK_MINOR_LINE)
      set(_SPDK_EFFECTIVE_VERSION "${_SPDK_MAJOR}.${_SPDK_MINOR}")
    endif()
  endif()

  if (NOT _SPDK_EFFECTIVE_VERSION MATCHES "^23\\.(01|1)(\\.|$)")
    message(FATAL_ERROR
      "STORE_USE_SPDK=ON requires SPDK 23.01.x. "
      "Detected pkg-config spdk_nvme version '${SPDK_NVME_VERSION}' and "
      "effective SPDK version '${_SPDK_EFFECTIVE_VERSION}'.")
  endif()

  set(STORE_SPDK_LIBS
      PkgConfig::SPDK_NVME
      PkgConfig::SPDK_ENV_DPDK)
  set(MOONCAKE_STORE_USE_SPDK 1)
endif()

# Find AWS SDK
find_package(AWSSDK QUIET COMPONENTS s3)
if(AWSSDK_FOUND)
    message(STATUS "AWS SDK found: ${AWSSDK_VERSION}")
    set(HAVE_AWS_SDK TRUE)
    # Add S3 related source files
    list(APPEND MOONCAKE_STORE_SOURCES utils/s3_helper.cpp)
else()
    message(STATUS "AWS SDK not found, S3 functionality will be disabled")
    set(HAVE_AWS_SDK FALSE)
endif()

# Find zstd library
find_library(ZSTD_LIBRARY NAMES zstd)
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "zstd library not found")
endif()

set(EXTRA_LIBS ${ZSTD_LIBRARY})

# If AWS SDK is found, add it to EXTRA_LIBS
if(HAVE_AWS_SDK)
    list(APPEND EXTRA_LIBS ${AWSSDK_LINK_LIBRARIES})
    add_definitions(-DHAVE_AWS_SDK)
    if(AWS_S3_LIB AND AWS_CORE_LIB)
        list(APPEND EXTRA_LIBS ${AWS_S3_LIB} ${AWS_CORE_LIB})
        add_definitions(-DHAVE_AWS_SDK)
    endif()
endif()

if(USE_3FS)
  add_subdirectory(hf3fs)
  list(APPEND MOONCAKE_STORE_SOURCES  ${HF3FS_SOURCES})
  find_library(HF3FS_API_LIB hf3fs_api_shared PATHS /usr/lib NO_DEFAULT_PATH)
  if(NOT HF3FS_API_LIB)
    message(FATAL_ERROR "hf3fs_api_shared library not found in /usr/lib")
  endif()
  set(EXTRA_LIBS ${HF3FS_API_LIB})
endif()

# io_uring support (auto-detected)
find_library(URING_LIB uring PATHS /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64)
find_path(URING_INCLUDE liburing.h PATHS /usr/include /usr/local/include)
if(URING_LIB AND URING_INCLUDE)
  message(STATUS "io_uring: Enabled for mooncake_store")
  list(APPEND MOONCAKE_STORE_SOURCES uring_file.cpp)
  list(APPEND EXTRA_LIBS ${URING_LIB})
else()
  message(STATUS "io_uring: Disabled (liburing not found)")
endif()

# The cache_allocator library
include_directories(${Python3_INCLUDE_DIRS})
add_library(mooncake_store ${MOONCAKE_STORE_SOURCES})
# Note: transfer_engine is PRIVATE to avoid propagating its dependencies (e.g., vendor-specific hardware)
# to targets that don't need it (e.g., mooncake_master).
# Targets that need transfer_engine should link it explicitly.
target_link_libraries(mooncake_store
    PUBLIC
        cachelib_memory_allocator
        ${ETCD_WRAPPER_LIB}
        glog::glog
        gflags::gflags
        ${EXTRA_LIBS}
        asio_shared
        ${STORE_SPDK_LIBS}
    PRIVATE
        transfer_engine
)
target_compile_definitions(mooncake_store
    PRIVATE
        MOONCAKE_STORE_USE_SPDK=${MOONCAKE_STORE_USE_SPDK}
)
if (STORE_USE_ETCD)
    add_dependencies(mooncake_store build_etcd_wrapper)
endif()

if(URING_LIB AND URING_INCLUDE)
    target_compile_definitions(mooncake_store PUBLIC USE_URING)
    target_include_directories(mooncake_store PRIVATE ${URING_INCLUDE})
endif()

if (USE_ASCEND_DIRECT OR USE_UBSHMEM)
    set(ACL_RUNTIME_HEADER_PATH "${ASCEND_INCLUDE_DIR}/acl/acl_rt.h")
    if(EXISTS "${ACL_RUNTIME_HEADER_PATH}")
        file(READ "${ACL_RUNTIME_HEADER_PATH}" ACL_RUNTIME_CONTENT)
        if("${ACL_RUNTIME_CONTENT}" MATCHES "ACL_MEM_P2P_HUGE1G")
            message(STATUS "ACL_MEM_P2P_HUGE1G exist.")
            add_compile_definitions(ASCEND_SUPPORT_FABRIC_MEM)
        else()
            message(STATUS "ACL_MEM_P2P_HUGE1G is not exist.")
        endif()
    else()
        message(WARNING "Acl runtime header file is not exist")
    endif()
endif()

if (BUILD_SHARED_LIBS)
  install(TARGETS mooncake_store DESTINATION lib)
endif()

# Master binary
add_executable(mooncake_master master.cpp)

set(MASTER_EXTRA_INCS)
set(MASTER_EXTRA_LIBS)

if (STORE_USE_JEMALLOC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JEMALLOC REQUIRED jemalloc)
    list(APPEND MASTER_EXTRA_INCS ${JEMALLOC_INCLUDE_DIRS})
    list(APPEND MASTER_EXTRA_LIBS ${JEMALLOC_STATIC_LIBRARIES})
endif()

target_include_directories(mooncake_master PRIVATE ${MASTER_EXTRA_INCS})
target_link_libraries(mooncake_master PRIVATE
    mooncake_store
    cachelib_memory_allocator
    pthread
    ibverbs
    mooncake_common
    ${ETCD_WRAPPER_LIB}
    ${MASTER_EXTRA_LIBS}
    asio_shared
)

if (STORE_USE_ETCD)
    add_dependencies(mooncake_master build_etcd_wrapper)
endif()

target_compile_options(mooncake_master PRIVATE -Os)
target_link_options(mooncake_master PRIVATE -Os -s)

# Client server binary
add_executable(mooncake_client real_client_main.cpp)
# Client needs transfer_engine for data transfer operations
target_link_libraries(mooncake_client PRIVATE mooncake_store transfer_engine asio_shared)
target_compile_options(mooncake_client PRIVATE -Os)
target_link_options(mooncake_client PRIVATE -Os -s)

install(TARGETS mooncake_master mooncake_client DESTINATION bin)
