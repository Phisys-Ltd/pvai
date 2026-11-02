cmake_minimum_required(VERSION 3.21)

project(pvai
    VERSION 0.5.37
    DESCRIPTION "pvai: Claude Code and Codex sessions reachable by voice from the PhiVoice AI app"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

find_package(nlohmann_json REQUIRED)
find_package(OpenSSL 3 REQUIRED)

add_executable(pvai
    src/main.cpp
    src/codex.cpp
    src/common.cpp
    src/claude.cpp
    src/codex_backend.cpp
    src/kimi_backend.cpp
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
    third_party/qrcodegen/qrcodegen.cpp
)
target_include_directories(pvai PRIVATE third_party/qrcodegen)
target_compile_definitions(pvai PRIVATE PVAI_VERSION="${PROJECT_VERSION}")
target_compile_options(pvai PRIVATE -Wall -Wextra)
target_link_libraries(pvai PRIVATE nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(pvai PRIVATE util)
endif()

install(TARGETS pvai RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Tests: direct access against a real daemon (ctest). Off in the package.
include(CTest)
if(BUILD_TESTING)
    add_executable(pvai-direct-test tests/direct_test.cpp src/common.cpp src/pairing.cpp src/tls.cpp)
    target_include_directories(pvai-direct-test PRIVATE src)
    target_compile_definitions(pvai-direct-test PRIVATE PVAI_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pvai-direct-test PRIVATE nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME direct-access COMMAND pvai-direct-test $<TARGET_FILE:pvai>)
    add_executable(pvai-agent-test tests/agent_test.cpp src/common.cpp)
    target_include_directories(pvai-agent-test PRIVATE src)
    target_compile_definitions(pvai-agent-test PRIVATE PVAI_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pvai-agent-test PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)
    add_test(NAME agent-messages COMMAND pvai-agent-test $<TARGET_FILE:pvai>)
    add_executable(pvai-session-test tests/session_test.cpp src/common.cpp)
    target_include_directories(pvai-session-test PRIVATE src)
    target_compile_definitions(pvai-session-test PRIVATE PVAI_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pvai-session-test PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)
    add_test(NAME sessions COMMAND pvai-session-test $<TARGET_FILE:pvai>)
    add_executable(pvai-screen-test tests/screen_test.cpp src/screen.cpp)
    target_include_directories(pvai-screen-test PRIVATE src)
    add_test(NAME screen COMMAND pvai-screen-test)
    add_executable(pvai-transcript-test tests/transcript_test.cpp src/transcript.cpp src/session.cpp src/common.cpp)
    target_include_directories(pvai-transcript-test PRIVATE src)
    target_compile_definitions(pvai-transcript-test PRIVATE PVAI_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pvai-transcript-test PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)
    add_test(NAME transcript COMMAND pvai-transcript-test)
endif()

# libFuzzer on what a client may send before login (clang only):
#   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DPVAI_FUZZ=ON && cmake --build build-fuzz --target fuzz-pair
option(PVAI_FUZZ "build the libFuzzer targets" OFF)
if(PVAI_FUZZ)
    add_executable(fuzz-pair tests/fuzz_pair.cpp src/common.cpp src/pairing.cpp)
    target_include_directories(fuzz-pair PRIVATE src)
    target_compile_definitions(fuzz-pair PRIVATE PVAI_VERSION="${PROJECT_VERSION}")
    target_compile_options(fuzz-pair PRIVATE -fsanitize=fuzzer,address,undefined -g)
    target_link_options(fuzz-pair PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz-pair PRIVATE nlohmann_json::nlohmann_json)
endif()
