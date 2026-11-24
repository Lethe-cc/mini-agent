cmake_minimum_required(VERSION 3.23)
project(mini-agent VERSION 0.8.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "mini-agent supports Linux only; see docs/design/024-drop-macos-support.md")
endif()

# Default `cmake --install` prefix: ~/.local, so the binary lands in
# ~/.local/bin (commonly already on PATH). An explicit -DCMAKE_INSTALL_PREFIX
# (or a preset cache entry) always wins; CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT is
# only true when CMake picked its own default for this configure.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT WIN32 AND NOT "$ENV{HOME}" STREQUAL "")
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(STARTER_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" OFF)
option(STARTER_CLANG_TIDY "Enable installed clang-tidy during compilation" OFF)
option(STARTER_SANITIZERS "Enable AddressSanitizer and UBSan on Linux GCC/Clang" OFF)
option(MINI_AGENT_REFRESH_MODELS_DEV "Fetch the models.dev catalog at build time (vendored snapshot fallback)" ON)

if(STARTER_CLANG_TIDY)
    find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE}")
endif()

function(starter_options target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(STARTER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(STARTER_SANITIZERS)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "STARTER_SANITIZERS requires Linux GCC/Clang; other toolchains need explicit configuration")
        endif()
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

find_package(CURL REQUIRED)

# Build metadata for `mini-agent version` and the startup banners:
# short commit (+ "-dirty" when the tree has uncommitted changes) and a
# UTC configure-time stamp. Graceful fallbacks outside a git checkout
# (source tarballs); never a hard requirement.
find_package(Git QUIET)
set(MINI_AGENT_COMMIT "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE MINI_AGENT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT MINI_AGENT_COMMIT)
        set(MINI_AGENT_COMMIT "unknown")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE MINI_AGENT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(MINI_AGENT_DIRTY)
        string(APPEND MINI_AGENT_COMMIT "-dirty")
    endif()
endif()
string(TIMESTAMP MINI_AGENT_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

add_library(agent_core STATIC
    src/core/chat.cpp
    src/core/context.cpp
    src/core/config.cpp
    src/core/loop.cpp
    src/core/log.cpp
    src/core/project.cpp
    src/core/session.cpp
    src/core/session_serde.cpp
    src/provider/provider_openai.cpp
    src/provider/provider_parse.cpp
    src/provider/models_dev.cpp
    src/tools/diff.cpp
    src/tools/subprocess.cpp
    src/tools/tools.cpp
    src/tools/tools_file.cpp
    src/tools/tools_git.cpp
    src/tools/workspace.cpp
    src/mcp/mcp_rpc.cpp
    src/mcp/mcp_stdio.cpp
    src/mcp/mcp_http.cpp
    src/render/highlight.cpp
    src/render/md.cpp
    src/render/md_geometry.cpp
    src/tui/tui.cpp
    src/tui/tui_preview.cpp
    src/tui/tui_sidebar.cpp
    src/tui/tui_workers.cpp
    src/tui/tui_turn.cpp
    src/tui/tui_render.cpp
    src/tui/tui_history.cpp
    third_party/md4c/md4c.c
)
target_include_directories(agent_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"      # nlohmann/json.hpp
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog" # spdlog/ tree (header-only)
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c"   # md4c.h (ADR 004)
)
target_compile_features(agent_core PUBLIC cxx_std_17)
target_link_libraries(agent_core PUBLIC CURL::libcurl)

# Configure-time models.dev catalog refresh (design 010 §21, §21.4): the
# fetch runs when cmake (re-)configures, NEVER during make/ninja -- the old
# ALL custom target stalled every incremental build on the network (broken
# IPv6 hangs tens of seconds per attempt). The vendored header in the source
# tree is regenerated in place; --best-effort means a failed fetch/schema
# guard never breaks configure and never writes: the header stays at its
# last good content. Write-if-different keeps unchanged catalogs from
# dirtying git status or triggering recompiles; upstream updates DO show up
# as a modified header (expected, commit it along with the next change).
if(MINI_AGENT_REFRESH_MODELS_DEV)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        message(STATUS "full update command: ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/refresh_models_dev.py --output ${CMAKE_CURRENT_SOURCE_DIR}/include/mini_agent/models_dev_data.hpp --best-effort")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/refresh_models_dev.py"
                    --output "${CMAKE_CURRENT_SOURCE_DIR}/include/mini_agent/models_dev_data.hpp"
                    --best-effort
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE MINI_AGENT_MODELS_DEV_REFRESH_RESULT)
        if(NOT MINI_AGENT_MODELS_DEV_REFRESH_RESULT EQUAL 0)
            message(WARNING "models.dev refresh exited "
                            "${MINI_AGENT_MODELS_DEV_REFRESH_RESULT}; "
                            "keeping the existing catalog header")
        endif()
    else()
        message(STATUS "models.dev refresh: Python3 not found, using the vendored catalog snapshot")
    endif()
else()
    message(STATUS "models.dev refresh disabled (MINI_AGENT_REFRESH_MODELS_DEV=OFF), using the vendored catalog snapshot")
endif()

# The TUI banner (tui.cpp, part of agent_core) shows the version too,
# and /version reports the same build identity as the CLI subcommand.
target_compile_definitions(agent_core PRIVATE
    MINI_AGENT_VERSION="${PROJECT_VERSION}"
    MINI_AGENT_COMMIT="${MINI_AGENT_COMMIT}"
    MINI_AGENT_BUILD_TIME="${MINI_AGENT_BUILD_TIME}")
starter_options(agent_core)

add_executable(mini-agent src/cli/cli_main.cpp src/cli/cli_common.cpp src/cli/cli_commands.cpp)
target_link_libraries(mini-agent PRIVATE agent_core)
target_compile_definitions(mini-agent PRIVATE
    MINI_AGENT_VERSION="${PROJECT_VERSION}"
    MINI_AGENT_COMMIT="${MINI_AGENT_COMMIT}"
    MINI_AGENT_BUILD_TIME="${MINI_AGENT_BUILD_TIME}")
starter_options(mini-agent)

# `agent_core` is an internal static library (not a public API), so only the
# CLI/TUI executable is installed: cmake --install build/linux-gcc-debug
# copies mini-agent to ${CMAKE_INSTALL_PREFIX}/bin (default ~/.local/bin).
install(TARGETS mini-agent RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(agent_test tests/cpp/agent_test.cpp)
    target_link_libraries(agent_test PRIVATE agent_core)
    starter_options(agent_test)

    add_executable(starter_test tests/cpp/project_test.cpp)
    target_link_libraries(starter_test PRIVATE agent_core)
    starter_options(starter_test)

    add_executable(mcp_test tests/cpp/mcp_test.cpp)
    target_link_libraries(mcp_test PRIVATE agent_core)
    starter_options(mcp_test)

    add_test(NAME agent.core COMMAND agent_test)
    add_test(NAME mcp.client COMMAND mcp_test)
    add_test(NAME starter.project_name COMMAND starter_test)
endif()
