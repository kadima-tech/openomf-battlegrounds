# deobjects-edit: unpack .DEObjects files to editable JSON and pack them back.
# Standalone: builds with any C++20 compiler (MSVC, MinGW, Apple Clang, GCC) and no other
# libraries. It reuses the engine's container model and decompression code from ../../engine/src/de.
cmake_minimum_required(VERSION 3.20)
project(deobjects_editor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(ENGINE_DE ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/src/de)

add_library(deobjects STATIC
	src/json.cpp
	src/schema.cpp
	src/convert.cpp
	${ENGINE_DE}/contents.cpp
	${ENGINE_DE}/lzrw3a.cpp
	${ENGINE_DE}/md5.cpp
	${ENGINE_DE}/objects_model.cpp
)
target_include_directories(deobjects PUBLIC src ${ENGINE_DE}/..)

if(MSVC)
	target_compile_options(deobjects PUBLIC /W3 /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(deobjects PUBLIC -Wall -Wextra)
endif()
# MinGW: link the C++ runtime statically so the .exe runs on its own.
if(MINGW)
	target_link_options(deobjects PUBLIC -static)
endif()

add_executable(deobjects-edit src/main.cpp)
target_link_libraries(deobjects-edit PRIVATE deobjects)

enable_testing()
add_executable(deobjects-tests tests/tests.cpp)
target_link_libraries(deobjects-tests PRIVATE deobjects)
add_test(NAME deobjects-tests COMMAND deobjects-tests)
# Optional: round-trip real game files. Pass -DDEOBJECTS_SAMPLES=<dir with .DEObjects files>.
if(DEOBJECTS_SAMPLES)
	add_test(NAME deobjects-samples COMMAND deobjects-tests ${DEOBJECTS_SAMPLES})
endif()
