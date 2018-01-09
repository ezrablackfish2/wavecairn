cmake_minimum_required(VERSION 3.20)
project(wavecairn VERSION 1.0.0 LANGUAGES CXX)

option(WAVECAIRN_BUILD_TESTS "Build WaveCairn tests" ON)
option(WAVECAIRN_BUILD_EXAMPLES "Build WaveCairn examples" ON)
option(WAVECAIRN_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB WAVECAIRN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(wavecairn_lib STATIC ${WAVECAIRN_SOURCES})
add_library(wavecairn::wavecairn ALIAS wavecairn_lib)
target_include_directories(wavecairn_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(wavecairn_lib PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(wavecairn_lib PRIVATE /W4 /permissive-)
else()
    target_compile_options(wavecairn_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WAVECAIRN_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(wavecairn_lib PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(wavecairn_lib PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
endif()

add_executable(wavecairn cli/main.cpp)
target_link_libraries(wavecairn PRIVATE wavecairn::wavecairn)

if(WAVECAIRN_BUILD_EXAMPLES)
    add_executable(wavecairn_make_demo examples/make_demo.cpp)
    target_link_libraries(wavecairn_make_demo PRIVATE wavecairn::wavecairn)
endif()

include(CTest)
if(BUILD_TESTING AND WAVECAIRN_BUILD_TESTS)
    add_executable(wavecairn_tests tests/wavecairn_tests.cpp)
    target_link_libraries(wavecairn_tests PRIVATE wavecairn::wavecairn)
    add_test(NAME wavecairn_tests COMMAND wavecairn_tests)
endif()

install(TARGETS wavecairn_lib wavecairn
        EXPORT WaveCairnTargets
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT WaveCairnTargets
        FILE WaveCairnTargets.cmake
        NAMESPACE wavecairn::
        DESTINATION lib/cmake/WaveCairn)
