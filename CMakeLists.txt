cmake_minimum_required(VERSION 3.16)
project(chronobent VERSION 0.3.0 LANGUAGES C CXX)

option(CHRONOBENT_BUILD_TESTS "Build synthetic regression tests" ON)
option(CHRONOBENT_BUILD_EXAMPLES "Build the WAV renderer and benchmark" ON)
option(CHRONOBENT_SANITIZE "Enable address/undefined behavior sanitizers" OFF)
option(CHRONOBENT_BUILD_PITCH_LAB "Build the native macOS audition app" OFF)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS sources.txt)
file(STRINGS sources.txt CHRONOBENT_SOURCES)
add_library(chronobent ${CHRONOBENT_SOURCES})
add_library(chronobent::chronobent ALIAS chronobent)
target_compile_features(chronobent PRIVATE cxx_std_17)
target_include_directories(chronobent PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(chronobent PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION} SOVERSION 0)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(chronobent PRIVATE CHRONOBENT_BUILD PUBLIC CHRONOBENT_SHARED)
endif()
if(MSVC)
    target_compile_options(chronobent PRIVATE /W4 /WX /fp:precise)
else()
    target_compile_options(chronobent PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-fast-math)
endif()
if(CHRONOBENT_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "This sanitizer target requires Clang or GCC")
    endif()
    target_compile_options(chronobent PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(chronobent PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all)
endif()

if(CHRONOBENT_BUILD_TESTS)
    enable_testing()
    add_executable(chronobent-test tests/test_chronobent.cpp)
    if(BUILD_SHARED_LIBS)
        # The independent FFT oracle needs the private implementation; the DLL
        # intentionally exports only the public C API.
        target_sources(chronobent-test PRIVATE src/fft.cpp)
    endif()
    target_link_libraries(chronobent-test PRIVATE chronobent)
    target_compile_features(chronobent-test PRIVATE cxx_std_17)
    add_test(NAME chronobent-regression COMMAND chronobent-test)
    add_executable(chronobent-c-test tests/test_c.c)
    target_link_libraries(chronobent-c-test PRIVATE chronobent)
    set_target_properties(chronobent-c-test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME chronobent-c-api COMMAND chronobent-c-test)
    add_executable(chronobent-processor-test tests/test_processor.cpp)
    target_compile_features(chronobent-processor-test PRIVATE cxx_std_17)
    target_link_libraries(chronobent-processor-test PRIVATE chronobent)
    add_test(NAME chronobent-processor COMMAND chronobent-processor-test)
endif()
if(CHRONOBENT_BUILD_PITCH_LAB OR CHRONOBENT_BUILD_TESTS)
    find_package(Threads REQUIRED)
    add_library(chronobent-audition STATIC examples/pitch_lab/player.cpp)
    target_compile_features(chronobent-audition PUBLIC cxx_std_17)
    target_link_libraries(chronobent-audition PUBLIC chronobent Threads::Threads)
endif()
if(CHRONOBENT_BUILD_TESTS)
    add_executable(chronobent-player-test tests/test_player.cpp)
    target_link_libraries(chronobent-player-test PRIVATE chronobent-audition)
    add_test(NAME chronobent-player COMMAND chronobent-player-test)
endif()
if(CHRONOBENT_BUILD_PITCH_LAB)
    if(NOT APPLE)
        message(FATAL_ERROR "ChronoBent Lab uses the macOS Cocoa and AVFoundation frameworks")
    endif()
    enable_language(OBJCXX)
    add_executable(chronobent-pitch-lab MACOSX_BUNDLE examples/pitch_lab/main.mm)
    set_target_properties(chronobent-pitch-lab PROPERTIES
        OUTPUT_NAME "ChronoBent Lab" MACOSX_BUNDLE_GUI_IDENTIFIER "local.chronobent.pitchlab"
        MACOSX_BUNDLE_BUNDLE_NAME "ChronoBent Lab" MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
    target_compile_options(chronobent-pitch-lab PRIVATE -fobjc-arc)
    target_link_libraries(chronobent-pitch-lab PRIVATE chronobent-audition "-framework Cocoa" "-framework AVFoundation")
    if(CHRONOBENT_BUILD_TESTS)
        add_test(NAME chronobent-native-audio COMMAND chronobent-pitch-lab --self-test)
        add_test(NAME chronobent-native-ui COMMAND chronobent-pitch-lab --ui-self-test)
    endif()
endif()
if(CHRONOBENT_BUILD_EXAMPLES)
    add_executable(chronobent-render examples/render_wav.cpp)
    target_compile_features(chronobent-render PRIVATE cxx_std_17)
    target_link_libraries(chronobent-render PRIVATE chronobent)
    add_executable(chronobent-benchmark examples/benchmark.cpp)
    target_compile_features(chronobent-benchmark PRIVATE cxx_std_17)
    target_link_libraries(chronobent-benchmark PRIVATE chronobent)
    add_executable(chronobent-example-c examples/processor_c.c)
    set_target_properties(chronobent-example-c PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(chronobent-example-c PRIVATE chronobent)
    add_executable(chronobent-example-cpp examples/processor_cpp.cpp)
    target_compile_features(chronobent-example-cpp PRIVATE cxx_std_17)
    target_link_libraries(chronobent-example-cpp PRIVATE chronobent)
    if(CHRONOBENT_BUILD_TESTS)
        add_test(NAME chronobent-example-c COMMAND chronobent-example-c)
        add_test(NAME chronobent-example-cpp COMMAND chronobent-example-cpp)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME chronobent-quality COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/measure_quality.py $<TARGET_FILE:chronobent-render>)
        add_test(NAME chronobent-wav COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wav.py $<TARGET_FILE:chronobent-render>)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS chronobent EXPORT chronobentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT chronobentTargets FILE chronobentTargets.cmake NAMESPACE chronobent::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chronobent)
configure_package_config_file(cmake/chronobentConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/chronobentConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chronobent)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/chronobentConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chronobentConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/chronobentConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chronobent)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/chronobent)
