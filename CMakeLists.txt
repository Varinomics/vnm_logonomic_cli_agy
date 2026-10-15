cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
# A build that embeds this package owns its cache, its try_compile settings and
# the SDK (the SDK's docs/package_contract.md, "Embedded build").
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()

project(logonomic_cli_agy VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must add the Logonomic CLI add-on SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

# SQLite documents this comment-embedded CSV as a stable interface for scripts.
# Reading its current archive URL and hash avoids making a tested release an
# accidental compatibility ceiling for this parser dependency.
set(AGY_SQLITE_METADATA_FILE "${CMAKE_CURRENT_BINARY_DIR}/_deps/sqlite-download-metadata.html")
file(DOWNLOAD "https://www.sqlite.org/download.html" "${AGY_SQLITE_METADATA_FILE}"
    STATUS AGY_SQLITE_METADATA_STATUS TLS_VERIFY ON)
list(GET AGY_SQLITE_METADATA_STATUS 0 AGY_SQLITE_METADATA_RESULT)
if(NOT AGY_SQLITE_METADATA_RESULT EQUAL 0)
    list(GET AGY_SQLITE_METADATA_STATUS 1 AGY_SQLITE_METADATA_ERROR)
    message(FATAL_ERROR "Could not retrieve SQLite download metadata: ${AGY_SQLITE_METADATA_ERROR}")
endif()

file(READ "${AGY_SQLITE_METADATA_FILE}" AGY_SQLITE_METADATA)
string(REPLACE "\r\n" "\n" AGY_SQLITE_METADATA "${AGY_SQLITE_METADATA}")
string(REPLACE "\r" "\n" AGY_SQLITE_METADATA "${AGY_SQLITE_METADATA}")
set(AGY_SQLITE_CSV_MARKER "<!-- Download product data for scripts to read")
string(FIND "${AGY_SQLITE_METADATA}" "${AGY_SQLITE_CSV_MARKER}" AGY_SQLITE_CSV_START)
if(AGY_SQLITE_CSV_START EQUAL -1)
    message(FATAL_ERROR "SQLite download metadata is malformed: script-data marker is missing.")
endif()
string(LENGTH "${AGY_SQLITE_CSV_MARKER}" AGY_SQLITE_CSV_MARKER_LENGTH)
math(EXPR AGY_SQLITE_CSV_START "${AGY_SQLITE_CSV_START} + ${AGY_SQLITE_CSV_MARKER_LENGTH}")
string(SUBSTRING "${AGY_SQLITE_METADATA}" ${AGY_SQLITE_CSV_START} -1 AGY_SQLITE_CSV_BLOCK)
string(FIND "${AGY_SQLITE_CSV_BLOCK}" "-->" AGY_SQLITE_CSV_END)
if(AGY_SQLITE_CSV_END EQUAL -1)
    message(FATAL_ERROR "SQLite download metadata is malformed: script-data comment is unterminated.")
endif()
string(SUBSTRING "${AGY_SQLITE_CSV_BLOCK}" 0 ${AGY_SQLITE_CSV_END} AGY_SQLITE_CSV_BLOCK)
string(REPLACE "\n" ";" AGY_SQLITE_CSV_LINES "${AGY_SQLITE_CSV_BLOCK}")

set(AGY_SQLITE_AMALGAMATION_URL "")
set(AGY_SQLITE_AMALGAMATION_HASH "")
set(AGY_SQLITE_AMALGAMATION_ARCHIVE_NAME "")
foreach(AGY_SQLITE_CSV_ROW IN LISTS AGY_SQLITE_CSV_LINES)
    string(STRIP "${AGY_SQLITE_CSV_ROW}" AGY_SQLITE_CSV_ROW)
    if(AGY_SQLITE_CSV_ROW STREQUAL "" OR AGY_SQLITE_CSV_ROW MATCHES "^PRODUCT,VERSION,")
        continue()
    endif()
    string(REPLACE "," ";" AGY_SQLITE_CSV_FIELDS "${AGY_SQLITE_CSV_ROW}")
    list(LENGTH AGY_SQLITE_CSV_FIELDS AGY_SQLITE_FIELD_COUNT)
    if(AGY_SQLITE_FIELD_COUNT LESS 5)
        message(FATAL_ERROR "SQLite download metadata product row has fewer than five fields: [${AGY_SQLITE_CSV_ROW}]")
    endif()
    list(GET AGY_SQLITE_CSV_FIELDS 0 AGY_SQLITE_PRODUCT)
    list(GET AGY_SQLITE_CSV_FIELDS 2 AGY_SQLITE_RELATIVE_URL)
    list(GET AGY_SQLITE_CSV_FIELDS 3 AGY_SQLITE_ARCHIVE_SIZE)
    list(GET AGY_SQLITE_CSV_FIELDS 4 AGY_SQLITE_ARCHIVE_HASH)
    if(NOT AGY_SQLITE_PRODUCT STREQUAL "PRODUCT")
        message(FATAL_ERROR "SQLite download metadata has an invalid product marker: [${AGY_SQLITE_CSV_ROW}]")
    endif()
    if(NOT AGY_SQLITE_ARCHIVE_SIZE MATCHES "^[0-9]+$" OR AGY_SQLITE_ARCHIVE_SIZE STREQUAL "0")
        message(FATAL_ERROR "SQLite download metadata has an invalid archive size: [${AGY_SQLITE_CSV_ROW}]")
    endif()
    if(NOT AGY_SQLITE_ARCHIVE_HASH MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "SQLite download metadata has a malformed SHA3-256 field: [${AGY_SQLITE_CSV_ROW}]")
    endif()
    string(LENGTH "${AGY_SQLITE_ARCHIVE_HASH}" AGY_SQLITE_ARCHIVE_HASH_LENGTH)
    if(NOT AGY_SQLITE_ARCHIVE_HASH_LENGTH EQUAL 64)
        message(FATAL_ERROR "SQLite download metadata has a malformed SHA3-256 field: [${AGY_SQLITE_CSV_ROW}]")
    endif()
    if(AGY_SQLITE_RELATIVE_URL MATCHES "sqlite-amalgamation")
        if(NOT AGY_SQLITE_RELATIVE_URL MATCHES "^[0-9][0-9][0-9][0-9]/sqlite-amalgamation-[0-9]+\\.zip$")
            message(FATAL_ERROR "SQLite amalgamation metadata contains an unsafe or unexpected relative URL: ${AGY_SQLITE_RELATIVE_URL}")
        endif()
        if(NOT AGY_SQLITE_AMALGAMATION_URL STREQUAL "")
            message(FATAL_ERROR "SQLite download metadata contains multiple sqlite-amalgamation products.")
        endif()
        get_filename_component(AGY_SQLITE_AMALGAMATION_ARCHIVE_NAME "${AGY_SQLITE_RELATIVE_URL}" NAME)
        string(REGEX REPLACE "\\.zip$" "" AGY_SQLITE_AMALGAMATION_SOURCE_DIR "${AGY_SQLITE_AMALGAMATION_ARCHIVE_NAME}")
        set(AGY_SQLITE_AMALGAMATION_URL "https://www.sqlite.org/${AGY_SQLITE_RELATIVE_URL}")
        set(AGY_SQLITE_AMALGAMATION_HASH "${AGY_SQLITE_ARCHIVE_HASH}")
    endif()
endforeach()

if(AGY_SQLITE_AMALGAMATION_URL STREQUAL "" OR AGY_SQLITE_AMALGAMATION_HASH STREQUAL "")
    message(FATAL_ERROR "SQLite download metadata has no valid sqlite-amalgamation product row.")
endif()

set(AGY_SQLITE_DEPENDENCY_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/agy_sqlite_source")
set(AGY_SQLITE_ARCHIVE "${AGY_SQLITE_DEPENDENCY_DIR}/${AGY_SQLITE_AMALGAMATION_ARCHIVE_NAME}")
set(AGY_SQLITE_SOURCE_ROOT "${AGY_SQLITE_DEPENDENCY_DIR}/src/${AGY_SQLITE_AMALGAMATION_SOURCE_DIR}")
file(MAKE_DIRECTORY "${AGY_SQLITE_DEPENDENCY_DIR}" "${AGY_SQLITE_DEPENDENCY_DIR}/src")
file(DOWNLOAD "${AGY_SQLITE_AMALGAMATION_URL}" "${AGY_SQLITE_ARCHIVE}"
    EXPECTED_HASH "SHA3_256=${AGY_SQLITE_AMALGAMATION_HASH}"
    STATUS AGY_SQLITE_ARCHIVE_STATUS TLS_VERIFY ON)
list(GET AGY_SQLITE_ARCHIVE_STATUS 0 AGY_SQLITE_ARCHIVE_RESULT)
if(NOT AGY_SQLITE_ARCHIVE_RESULT EQUAL 0)
    list(GET AGY_SQLITE_ARCHIVE_STATUS 1 AGY_SQLITE_ARCHIVE_ERROR)
    message(FATAL_ERROR "Could not download or verify the SQLite amalgamation archive: ${AGY_SQLITE_ARCHIVE_ERROR}")
endif()

# Match only the two files below the archive's expected top-level directory.
# This prevents unrelated archive entries from being extracted into the build tree.
file(ARCHIVE_EXTRACT INPUT "${AGY_SQLITE_ARCHIVE}"
    DESTINATION "${AGY_SQLITE_DEPENDENCY_DIR}/src"
    PATTERNS "${AGY_SQLITE_AMALGAMATION_SOURCE_DIR}/sqlite3.c"
        "${AGY_SQLITE_AMALGAMATION_SOURCE_DIR}/sqlite3.h")
if(NOT EXISTS "${AGY_SQLITE_SOURCE_ROOT}/sqlite3.c" OR
   NOT EXISTS "${AGY_SQLITE_SOURCE_ROOT}/sqlite3.h")
    message(FATAL_ERROR "The verified SQLite amalgamation archive is missing sqlite3.c or sqlite3.h.")
endif()

add_library(agy_sqlite STATIC "${AGY_SQLITE_SOURCE_ROOT}/sqlite3.c")
target_include_directories(agy_sqlite PUBLIC "${AGY_SQLITE_SOURCE_ROOT}")
target_compile_definitions(agy_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0)

function(agy_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endfunction()

add_library(agy_provider STATIC
    src/agy_events.cpp
    src/agy_launcher.cpp
    src/agy_preparation.cpp
    src/agy_quota.cpp
    src/agy_sessions.cpp
    src/agy_startup.cpp
    src/agy_usage_scan.cpp)
agy_target(agy_provider)
target_include_directories(agy_provider PUBLIC src)
target_link_libraries(agy_provider PUBLIC logonomic_cli_addon_sdk::sdk)
target_link_libraries(agy_provider PRIVATE agy_sqlite)

add_executable(agy_adapter src/adapter.cpp)
target_link_libraries(agy_adapter PRIVATE agy_provider)
agy_target(agy_adapter)

add_executable(agy_helper src/agy_helper.cpp)
target_link_libraries(agy_helper PRIVATE agy_provider)
agy_target(agy_helper)
logonomic_cli_addon_add_launch_bridge(agy_launch_bridge)
agy_target(agy_launch_bridge)
add_executable(agy_resume_bridge src/agy_resume_bridge.cpp)
target_link_libraries(agy_resume_bridge PRIVATE agy_provider)
if(WIN32)
    target_link_libraries(agy_resume_bridge PRIVATE winhttp advapi32)
endif()
agy_target(agy_resume_bridge)

logonomic_cli_addon_package_platform(AGY_PLATFORM)
set(AGY_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
logonomic_cli_addon_package_directory(logonomic.cli.agy AGY_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/*")
# Directory membership is an input too: removing a resource, or adding one
# with an older timestamp, must rebuild the indexed package. file(GENERATE)
# changes this file only when membership changes, preserving no-op builds.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt"
    CONTENT "${package_files}\n")
add_custom_command(
    OUTPUT "${AGY_PACKAGE_DIRECTORY}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${AGY_PACKAGE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${AGY_PACKAGE_DIRECTORY}/adapter" "${AGY_PACKAGE_DIRECTORY}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/assets" "${AGY_PACKAGE_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities" "${AGY_PACKAGE_DIRECTORY}/capabilities"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance" "${AGY_PACKAGE_DIRECTORY}/provenance"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/notices" "${AGY_PACKAGE_DIRECTORY}/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${AGY_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:agy_adapter>"
        "${AGY_PACKAGE_DIRECTORY}/adapter/agy_adapter${AGY_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:agy_helper>"
        "${AGY_PACKAGE_DIRECTORY}/helpers/agy_helper${AGY_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:agy_launch_bridge>"
        "${AGY_PACKAGE_DIRECTORY}/helpers/agy_launch_bridge${AGY_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:agy_resume_bridge>"
        "${AGY_PACKAGE_DIRECTORY}/helpers/agy_resume_bridge${AGY_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${AGY_PACKAGE_DIRECTORY}"
        --third-party "assets/antigravity-color.png" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
    DEPENDS agy_adapter agy_helper agy_launch_bridge agy_resume_bridge logonomic_cli_addon_index
        "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt" ${package_files}
    VERBATIM)
add_custom_target(agy_package ALL DEPENDS "${AGY_PACKAGE_DIRECTORY}/package.files.json")
logonomic_cli_addon_install_package("${AGY_PACKAGE_DIRECTORY}" logonomic.cli.agy TARGET agy_package)

# The tests belong to the package's own build. A build that embeds the package
# stages it and runs tests of its own, so nothing below is added there.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()
enable_testing()
add_executable(agy_provider_tests tests/agy_provider_tests.cpp)
target_link_libraries(agy_provider_tests PRIVATE agy_provider)
agy_target(agy_provider_tests)
add_dependencies(agy_provider_tests agy_package)
add_test(NAME agy_provider_tests COMMAND agy_provider_tests "${AGY_PACKAGE_DIRECTORY}")
add_executable(agy_usage_scan_tests tests/agy_usage_scan_tests.cpp)
target_link_libraries(agy_usage_scan_tests PRIVATE agy_provider agy_sqlite)
agy_target(agy_usage_scan_tests)
add_test(NAME agy_usage_scan_tests COMMAND agy_usage_scan_tests)
add_executable(agy_fake_vnmctl tests/agy_fake_vnmctl.cpp)
agy_target(agy_fake_vnmctl)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME agy_resume_bridge_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/agy_resume_bridge_tests.py" "$<TARGET_FILE:agy_resume_bridge>")
add_test(NAME agy_helper_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/agy_helper_tests.py"
    "$<TARGET_FILE:agy_helper>" "$<TARGET_FILE:agy_fake_vnmctl>")
add_test(NAME agy_package_validation COMMAND logonomic_cli_addon_validate
    "${AGY_PACKAGE_DIRECTORY}" --platform "${AGY_PLATFORM}")
add_test(NAME agy_package_conformance COMMAND logonomic_cli_addon_conformance
    "${AGY_PACKAGE_DIRECTORY}")
set_tests_properties(agy_provider_tests agy_usage_scan_tests agy_helper_tests agy_package_validation agy_package_conformance agy_resume_bridge_tests
    PROPERTIES TIMEOUT 120)

option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    # Exercise this repository's actual producer, never a substitute packaging
    # implementation. The isolated nested configure uses this same SDK source;
    # it does not change the owned master dependency or edit an installed package.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME agy_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=agy_package" "--package-id=logonomic.cli.agy"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${AGY_PLATFORM}")
    set_tests_properties(agy_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
if(_provider_tests)
    set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
endif()
