cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()

project(logonomic_cli_agy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/sdk")
else()
    include(FetchContent)
    FetchContent_Declare(logonomic_sdk_source
        GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
    FetchContent_MakeAvailable(logonomic_sdk_source)
endif()

function(agy_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(agy_provider STATIC
    src/agy_events.cpp
    src/agy_launcher.cpp
    src/agy_preparation.cpp
    src/agy_quota.cpp)
agy_target(agy_provider)
target_include_directories(agy_provider PUBLIC src)
target_link_libraries(agy_provider PUBLIC logonomic_cli_addon_sdk::sdk)

add_executable(agy_adapter src/adapter.cpp)
target_link_libraries(agy_adapter PRIVATE agy_provider)
agy_target(agy_adapter)

add_executable(agy_helper src/agy_helper.cpp)
target_link_libraries(agy_helper PRIVATE agy_provider)
agy_target(agy_helper)
add_executable(agy_launch_bridge src/agy_launch_bridge.cpp)
agy_target(agy_launch_bridge)

logonomic_cli_addon_package_platform(AGY_PLATFORM)
set(AGY_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
logonomic_cli_addon_package_directory(logonomic.cli.agy AGY_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/*")
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
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${AGY_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:agy_adapter>"
        "${AGY_PACKAGE_DIRECTORY}/adapter/agy_adapter${AGY_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:agy_helper>"
        "${AGY_PACKAGE_DIRECTORY}/helpers/agy_helper${AGY_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:agy_launch_bridge>"
        "${AGY_PACKAGE_DIRECTORY}/helpers/agy_launch_bridge${AGY_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${AGY_PACKAGE_DIRECTORY}"
        --third-party "assets/antigravity-color.png" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
    DEPENDS agy_adapter agy_helper agy_launch_bridge logonomic_cli_addon_index
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" ${package_files}
    VERBATIM)
add_custom_target(agy_package ALL DEPENDS "${AGY_PACKAGE_DIRECTORY}/package.files.json")
enable_testing()
add_executable(agy_provider_tests tests/agy_provider_tests.cpp)
target_link_libraries(agy_provider_tests PRIVATE agy_provider)
agy_target(agy_provider_tests)
add_test(NAME agy_provider_tests COMMAND agy_provider_tests)
add_executable(agy_fake_vnmctl tests/agy_fake_vnmctl.cpp)
agy_target(agy_fake_vnmctl)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME agy_helper_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/agy_helper_tests.py"
    "$<TARGET_FILE:agy_helper>" "$<TARGET_FILE:agy_fake_vnmctl>")
add_test(NAME agy_package_validation COMMAND logonomic_cli_addon_validate
    "${AGY_PACKAGE_DIRECTORY}" --platform "${AGY_PLATFORM}")
add_test(NAME agy_package_conformance COMMAND logonomic_cli_addon_conformance
    "${AGY_PACKAGE_DIRECTORY}")
set_tests_properties(agy_provider_tests agy_helper_tests agy_package_validation agy_package_conformance
    PROPERTIES TIMEOUT 120)
logonomic_cli_addon_install_package("${AGY_PACKAGE_DIRECTORY}" logonomic.cli.agy)
