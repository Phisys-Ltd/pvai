cmake_minimum_required(VERSION 3.21)

project(pvai
    VERSION 0.5.73
    DESCRIPTION "pvai: Claude Code and Codex sessions reachable by voice from the PhiVoice AI app"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

find_package(nlohmann_json REQUIRED)
find_package(OpenSSL 3 REQUIRED)

# The daemon's code as one library: a change recompiles its file only, and
# the programs and tests link it instead of compiling their own copies.
# The version is generated into its own file: a release recompiles that one.
configure_file(src/version.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/version.cpp @ONLY)

add_library(pvai_core STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/version.cpp
    src/codex.cpp
    src/common.cpp
    src/claude.cpp
    src/codex_backend.cpp
    src/kimi_backend.cpp
    src/rpc_backend.cpp
    src/daemon.cpp
    src/session.cpp
    src/run.cpp
    src/screen.cpp
    src/setup.cpp
    src/service.cpp
    src/hook.cpp
    src/pairing.cpp
    src/tls.cpp
    src/transcript.cpp
    src/text.cpp
    third_party/qrcodegen/qrcodegen.cpp
)
target_include_directories(pvai_core PUBLIC src PRIVATE third_party/qrcodegen)
target_compile_options(pvai_core PRIVATE -Wall -Wextra)
target_link_libraries(pvai_core PUBLIC nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(pvai_core PUBLIC util)
endif()

add_executable(pvai src/main.cpp)
target_include_directories(pvai PRIVATE third_party/qrcodegen)
target_compile_options(pvai PRIVATE -Wall -Wextra)
target_link_libraries(pvai PRIVATE pvai_core)

install(TARGETS pvai RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Tests: direct access against a real daemon (ctest). Off in the package.
include(CTest)
if(BUILD_TESTING)
    function(pvai_test name source)
        add_executable(${name} ${source})
        target_include_directories(${name} PRIVATE src)
        target_link_libraries(${name} PRIVATE pvai_core)
    endfunction()
    pvai_test(pvai-direct-test tests/direct_test.cpp)
    add_test(NAME direct-access COMMAND pvai-direct-test $<TARGET_FILE:pvai>)
    pvai_test(pvai-agent-test tests/agent_test.cpp)
    add_test(NAME agent-messages COMMAND pvai-agent-test $<TARGET_FILE:pvai>)
    pvai_test(pvai-peers-test tests/peers_test.cpp)
    add_test(NAME peers COMMAND pvai-peers-test $<TARGET_FILE:pvai>)
    pvai_test(pvai-session-test tests/session_test.cpp)
    add_test(NAME sessions COMMAND pvai-session-test $<TARGET_FILE:pvai>)
    pvai_test(pvai-screen-test tests/screen_test.cpp)
    add_test(NAME screen COMMAND pvai-screen-test)
    pvai_test(pvai-transcript-test tests/transcript_test.cpp)
    add_test(NAME transcript COMMAND pvai-transcript-test)
endif()

# libFuzzer on what a client may send before login (clang only):
#   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DPVAI_FUZZ=ON && cmake --build build-fuzz --target fuzz-pair
option(PVAI_FUZZ "build the libFuzzer targets" OFF)
if(PVAI_FUZZ)
    add_executable(fuzz-pair tests/fuzz_pair.cpp src/common.cpp src/pairing.cpp src/text.cpp ${CMAKE_CURRENT_BINARY_DIR}/version.cpp)
    target_include_directories(fuzz-pair PRIVATE src)
    target_compile_options(fuzz-pair PRIVATE -fsanitize=fuzzer,address,undefined -g)
    target_link_options(fuzz-pair PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz-pair PRIVATE nlohmann_json::nlohmann_json)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(fuzz-pair PRIVATE util)
    endif()
endif()
