cmake_minimum_required(VERSION 3.16)
project(infinitylinkdirect C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
    message(FATAL_ERROR "Infinity-Link-Direct is a Windows program (ASIO drivers are Windows DLLs): tools/build-windows.sh")
endif()
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")  # one .exe, nothing beside it

# the Infinity Link library (MIT), as it is in Infinity-link (third_party/infinitylink/VERSION) — PCM only, no codec flac
set(IL ${CMAKE_CURRENT_SOURCE_DIR}/third_party/infinitylink)
add_library(infinitylink STATIC ${IL}/src/json.cpp ${IL}/src/link.cpp ${IL}/src/endpoint.cpp ${IL}/src/sender.cpp
    ${IL}/src/discovery.cpp ${IL}/src/pack.cpp)
target_include_directories(infinitylink PUBLIC ${IL}/include)
find_package(Threads REQUIRED)
target_link_libraries(infinitylink PUBLIC Threads::Threads ws2_32 iphlpapi)
target_compile_definitions(infinitylink PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(Infinity-Link-Direct src/link_direct.cpp)
target_include_directories(Infinity-Link-Direct PRIVATE third_party/asiosdk/common)
target_link_libraries(Infinity-Link-Direct infinitylink ole32 uuid advapi32 user32)
target_compile_options(Infinity-Link-Direct PRIVATE -Wall -Wextra -Wno-unused-parameter)
# no console window: Infinity Core's Run key starts it at sign-in, and a console program opened a window per shared
# device that stayed until closed (tester 2026-11-23, Infinity-Link-Windows alike)
target_link_options(Infinity-Link-Direct PRIVATE -mwindows)
