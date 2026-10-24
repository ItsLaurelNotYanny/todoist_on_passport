cmake_minimum_required(VERSION 3.5)
project(todoist_on_passport LANGUAGES C CXX)

option(PASSPORT_BB10 "Build the native BlackBerry 10 target" OFF)

set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags for strict C++03 and POSIX conformance
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wno-long-long -std=c++03")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
endif()

if(PASSPORT_BB10)
    add_definitions(-DPASSPORT_BB10)
    add_compile_options(-fno-use-cxa-atexit)
    set(TARGET_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/bb10-deps")
    set(PLATFORM_LIBS socket m)
    include_directories(${TARGET_DEPS_DIR}/include /opt/bb10-sysroot/usr/include /opt/bb10-sysroot/usr/include/qt4)

    # Automatically build or verify BB10 dependencies with sha256 stamps
    execute_process(
        COMMAND sh "${CMAKE_SOURCE_DIR}/scripts/build-openssl-bb10.sh"
        RESULT_VARIABLE OPENSSL_BB10_RES
    )
    if(NOT OPENSSL_BB10_RES EQUAL 0)
        message(FATAL_ERROR "Failed to build BB10 OpenSSL 3.5.9")
    endif()

    execute_process(
        COMMAND sh "${CMAKE_SOURCE_DIR}/scripts/build-curl-bb10.sh"
        RESULT_VARIABLE CURL_BB10_RES
    )
    if(NOT CURL_BB10_RES EQUAL 0)
        message(FATAL_ERROR "Failed to build BB10 libcurl 8.23.0")
    endif()
else()
    set(TARGET_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/host-deps")
    set(PLATFORM_LIBS pthread ${CMAKE_DL_LIBS} m)

    # Automatically verify host dependencies; internal sha256 stamps trigger rebuilds when recipes change
    execute_process(
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/build-openssl-host.sh"
        RESULT_VARIABLE OPENSSL_BUILD_RES
    )
    if(NOT OPENSSL_BUILD_RES EQUAL 0)
        message(FATAL_ERROR "Failed to build host OpenSSL 3.5.9")
    endif()

    execute_process(
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/build-curl-host.sh"
        RESULT_VARIABLE CURL_BUILD_RES
    )
    if(NOT CURL_BUILD_RES EQUAL 0)
        message(FATAL_ERROR "Failed to build host libcurl 8.23.0")
    endif()
endif()

# Include directories
include_directories(
    include
    third_party/sqlite
    third_party/picojson
)

# SQLite library target
add_library(sqlite3_amalgamation STATIC
    third_party/sqlite/sqlite3.c
)
target_include_directories(sqlite3_amalgamation PUBLIC third_party/sqlite)
target_link_libraries(sqlite3_amalgamation PUBLIC ${PLATFORM_LIBS})

# Todoist Storage static library (Milestone 1)
set(STORAGE_SOURCES
    src/storage/uuid_util.cpp
    src/storage/db_manager.cpp
    src/storage/task_repository.cpp
    src/storage/sync_command_queue.cpp
    src/storage/temp_id_reconciler.cpp
    src/storage/sync_metadata_repo.cpp
)

add_library(todoist_storage STATIC
    ${STORAGE_SOURCES}
)
target_link_libraries(todoist_storage PUBLIC sqlite3_amalgamation)

# Todoist Sync static library (Milestone 2)
set(SYNC_SOURCES
    src/sync/sync_payload_builder.cpp
    src/sync/sync_response_parser.cpp
    src/sync/sync_engine.cpp
)

add_library(todoist_sync STATIC
    ${SYNC_SOURCES}
)
target_link_libraries(todoist_sync PUBLIC todoist_storage)

# BB10 TLS Universal Subsystem (Milestone 3 Phase 3A)
set(TLS_SOURCES
    src/tls/bb10_tls.cpp
    src/tls/bb10_tls_crypto.cpp
)

if(PASSPORT_BB10)
    list(APPEND TLS_SOURCES src/bb10_compat.cpp)
endif()

add_library(bb10_tls STATIC
    ${TLS_SOURCES}
)
target_include_directories(bb10_tls
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${TARGET_DEPS_DIR}/include
)
target_link_libraries(bb10_tls PUBLIC
    ${TARGET_DEPS_DIR}/lib/libssl.a
    ${TARGET_DEPS_DIR}/lib/libcrypto.a
    ${PLATFORM_LIBS}
)
target_link_options(bb10_tls INTERFACE "-Wl,--exclude-libs,ALL")

# Todoist Network static library (Milestone 3 Phase 3B)
set(NETWORK_SOURCES
    src/network/curl_sync_transport.cpp
    src/network/sync_worker_thread.cpp
)

add_library(todoist_network STATIC
    ${NETWORK_SOURCES}
)
target_include_directories(todoist_network
    PUBLIC
        include
    PRIVATE
        ${TARGET_DEPS_DIR}/include
)
target_link_libraries(todoist_network PUBLIC
    todoist_sync
    todoist_storage
    ${TARGET_DEPS_DIR}/lib/libcurl.a
    ${TARGET_DEPS_DIR}/lib/libssl.a
    ${TARGET_DEPS_DIR}/lib/libcrypto.a
    ${PLATFORM_LIBS}
)
target_link_options(todoist_network INTERFACE "-Wl,--exclude-libs,ALL")

# Todoist UI & Application Controller static library (Milestone 4)
set(UI_SOURCES
    src/ui/todoist_app_controller.cpp
)

add_library(todoist_ui STATIC
    ${UI_SOURCES}
)
target_include_directories(todoist_ui
    PUBLIC
        include
    PRIVATE
        ${TARGET_DEPS_DIR}/include
)
target_link_libraries(todoist_ui PUBLIC
    todoist_network
    todoist_sync
    todoist_storage
    sqlite3_amalgamation
    bb10_tls
    ${PLATFORM_LIBS}
)
target_link_options(todoist_ui INTERFACE "-Wl,--exclude-libs,ALL")

# BB10 compatibility stub for missing __dso_handle
set(BB10_COMPAT_SOURCES "")
if(PASSPORT_BB10)
    set(BB10_COMPAT_SOURCES src/bb10_compat.cpp)
endif()

# Native BlackBerry 10 Cascades Application Executable (Milestone 4)
if(PASSPORT_BB10)
    include(BB10Qt4)
    set(BB10_UI_MOC_HEADERS
        include/todoist/ui/cascades_task_list_model.hpp
        include/todoist/ui/cascades_app_bridge.hpp
    )
    qt4_wrap_cpp(BB10_UI_MOC_SOURCES ${BB10_UI_MOC_HEADERS})

    add_executable(todoist-passport
        src/ui/main_bb10.cpp
        src/ui/cascades_task_list_model.cpp
        src/ui/cascades_app_bridge.cpp
        ${BB10_UI_MOC_SOURCES}
        ${BB10_COMPAT_SOURCES}
    )
    target_include_directories(todoist-passport PRIVATE
        include
        ${TARGET_DEPS_DIR}/include
    )
    target_link_options(todoist-passport PRIVATE "-Wl,--exclude-libs,ALL")
    target_link_libraries(todoist-passport PRIVATE
        todoist_ui
        todoist_network
        todoist_sync
        todoist_storage
        sqlite3_amalgamation
        bb10_tls
        bbcascades
        bbdata
        bb
        QtDeclarative
        QtScript
        QtSvg
        QtSql
        QtXmlPatterns
        QtGui
        QtNetwork
        QtCore
        z
        ${PLATFORM_LIBS}
    )
endif()

# Diagnostic TLS Probe Executable
add_executable(bb10_tls_probe
    src/tools/bb10_tls_probe.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(bb10_tls_probe PRIVATE include ${TARGET_DEPS_DIR}/include)
target_link_libraries(bb10_tls_probe PRIVATE bb10_tls ${PLATFORM_LIBS})

# Standalone External Consumer Test
add_executable(test_standalone_consumer
    tests/standalone_consumer/test_consumer.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(test_standalone_consumer PRIVATE include)
target_link_libraries(test_standalone_consumer PRIVATE bb10_tls ${PLATFORM_LIBS})

# Storage Test Suite
add_executable(test_storage
    tests/storage/test_storage_all.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(test_storage PRIVATE tests)
target_link_libraries(test_storage PRIVATE todoist_storage)

# Sync Test Suite (Milestone 2)
add_executable(test_sync
    tests/sync/test_sync_all.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(test_sync PRIVATE tests)
target_link_libraries(test_sync PRIVATE todoist_sync todoist_storage)

# TLS Test Suite (Milestone 3 Phase 3A)
add_executable(test_bb10_tls
    tests/tls/test_bb10_tls_all.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(test_bb10_tls PRIVATE include ${TARGET_DEPS_DIR}/include)
target_link_libraries(test_bb10_tls PRIVATE bb10_tls ${PLATFORM_LIBS})

# Network Test Suite (Milestone 3 Phase 3B)
add_executable(test_network
    tests/network/test_network_all.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(test_network PRIVATE tests include ${TARGET_DEPS_DIR}/include)
target_link_libraries(test_network PRIVATE todoist_network)

# UI & Full-System Integration Test Suite (Milestone 4)
add_executable(test_ui
    tests/ui/test_ui_all.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(test_ui PRIVATE tests include ${TARGET_DEPS_DIR}/include)
target_link_libraries(test_ui PRIVATE todoist_ui)

# Coexistence Test Executable (Gate 2 / Gate 3)
add_executable(test_target_coexistence
    tests/target_coexistence/test_target_coexistence.cpp
    ${BB10_COMPAT_SOURCES}
)
target_include_directories(test_target_coexistence PRIVATE include ${TARGET_DEPS_DIR}/include)
if(PASSPORT_BB10)
    target_link_libraries(test_target_coexistence PRIVATE
        bb10_tls
        bbcascades
        QtNetwork
        QtCore
        ${PLATFORM_LIBS}
    )
else()
    target_link_libraries(test_target_coexistence PRIVATE
        bb10_tls
        ${PLATFORM_LIBS}
    )
endif()

# Enable CTest (on host)
enable_testing()
add_test(NAME StorageTestSuite COMMAND test_storage)
add_test(NAME SyncTestSuite COMMAND test_sync)
add_test(NAME TlsTestSuite COMMAND test_bb10_tls)
add_test(NAME StandaloneConsumerTest COMMAND test_standalone_consumer)
add_test(NAME NetworkTestSuite COMMAND test_network)
add_test(NAME UiTestSuite COMMAND test_ui)

# Relocatable CMake Package Installation Rules
include(CMakePackageConfigHelpers)
set(CONFIG_INSTALL_DIR "lib/cmake/bb10_tls")

if(PASSPORT_BB10)
    set(BB10_TLS_IS_BB10 1)
else()
    set(BB10_TLS_IS_BB10 0)
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bb10_tls-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/bb10_tls-config.cmake"
    INSTALL_DESTINATION "${CONFIG_INSTALL_DIR}"
    PATH_VARS CMAKE_INSTALL_PREFIX
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bb10_tls-config-version.cmake"
    VERSION 1.0.0
    COMPATIBILITY AnyNewerVersion
)

install(TARGETS bb10_tls
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/bb10_tls DESTINATION include)
install(FILES
    "${TARGET_DEPS_DIR}/lib/libssl.a"
    "${TARGET_DEPS_DIR}/lib/libcrypto.a"
    DESTINATION lib
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/bb10_tls-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/bb10_tls-config-version.cmake"
    DESTINATION "${CONFIG_INSTALL_DIR}"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sources/cacert.pem")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sources/cacert.pem"
        DESTINATION share/bb10_tls/certs
    )
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/share/bb10_tls/LICENSE")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/share/bb10_tls/LICENSE"
        DESTINATION share/bb10_tls
    )
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/share/bb10_tls/README.md")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/share/bb10_tls/README.md"
        DESTINATION share/bb10_tls
    )
endif()
