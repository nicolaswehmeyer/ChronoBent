# SPDX-License-Identifier: MIT
if(NOT APPLE)
    message(FATAL_ERROR "The 0.6 instrument targets macOS AU and VST3")
endif()
if(NOT EXISTS "${IPLUG2_DIR}/iPlug2.cmake")
    message(FATAL_ERROR "Set IPLUG2_DIR to the reviewed external iPlug2 checkout; see plugin/README.md")
endif()
enable_language(OBJC OBJCXX)
set(IPLUG_DEPLOY_PLUGINS OFF CACHE BOOL "Do not install plugins during builds" FORCE)
include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)
# The framework declares object libraries for every graphics backend. They are
# irrelevant to the WebView editor and need SDK assets outside this build.
get_property(framework_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS framework_targets)
    if(target MATCHES "^iPlug2_")
        set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()
iplug_add_plugin(ChronoBent
    SOURCES ChronoBentPlugin.cpp ChronoBentPlugin.h host.hpp mac_editor.mm mac_audio.mm mac_audio.hpp config.h tuning_state.cpp tuning_state.hpp
        ../host/mac_tuning_editor.mm ../host/mac_controls.mm
    RESOURCES ../LICENSE THIRD_PARTY_NOTICES.md
    WEB_RESOURCES resources/web/index.html resources/web/style.css resources/web/script.js resources/web/controls.js
    LINK chronobent-instrument chronobent-tuning-host "-framework AVFoundation" "-framework Cocoa"
    UI WEBVIEW FORMATS AU VST3)
set_source_files_properties(mac_audio.mm mac_editor.mm ../host/mac_tuning_editor.mm ../host/mac_controls.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
foreach(format au vst3)
    target_compile_features(ChronoBent-${format} PRIVATE cxx_std_17)
    target_compile_definitions(ChronoBent-${format} PRIVATE CHRONOBENT_TUNE_OBJC_PREFIX=ChronoBentInstrument_${format})
    add_custom_command(TARGET ChronoBent-${format} POST_BUILD
        COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:ChronoBent-${format}>"
        COMMENT "Ad-hoc sign the complete local plugin bundle" VERBATIM)
endforeach()
add_subdirectory(fx)

if(CHRONOBENT_BUILD_TESTS)
    add_executable(chronobent-tuning-plugin-test ../tests/test_tuning_plugin.mm)
    target_compile_features(chronobent-tuning-plugin-test PRIVATE cxx_std_17)
    target_compile_options(chronobent-tuning-plugin-test PRIVATE -fobjc-arc)
    target_link_libraries(chronobent-tuning-plugin-test PRIVATE "-framework Cocoa" "-framework AudioUnit" "-framework AudioToolbox" "-framework WebKit")
    if(CHRONOBENT_SANITIZE)
        target_compile_options(chronobent-tuning-plugin-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(chronobent-tuning-plugin-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_dependencies(chronobent-tuning-plugin-test ChronoBent-au ChronoBentFX-au)
    add_test(NAME chronobent-tuning-plugins COMMAND chronobent-tuning-plugin-test $<TARGET_BUNDLE_DIR:ChronoBent-au> $<TARGET_BUNDLE_DIR:ChronoBentFX-au>)
    add_executable(chronobent-plugin-audio-test ../tests/test_plugin_audio.mm mac_audio.mm)
    target_compile_features(chronobent-plugin-audio-test PRIVATE cxx_std_17)
    target_compile_options(chronobent-plugin-audio-test PRIVATE -fobjc-arc)
    target_link_libraries(chronobent-plugin-audio-test PRIVATE chronobent-instrument "-framework Cocoa" "-framework AVFoundation")
    add_test(NAME chronobent-plugin-import COMMAND chronobent-plugin-audio-test)
    add_executable(chronobent-plugin-host-test ../tests/test_plugin_host.mm)
    target_compile_features(chronobent-plugin-host-test PRIVATE cxx_std_17)
    target_compile_options(chronobent-plugin-host-test PRIVATE -fobjc-arc)
    target_link_libraries(chronobent-plugin-host-test PRIVATE "-framework Cocoa" "-framework AudioUnit" "-framework AudioToolbox" "-framework WebKit")
    if(CHRONOBENT_SANITIZE)
        # The host loads the instrument dynamically, so its sanitizer runtime
        # must be linked before dlopen rather than inherited through the DSO.
        target_compile_options(chronobent-plugin-host-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(chronobent-plugin-host-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_dependencies(chronobent-plugin-host-test ChronoBent-au)
    add_test(NAME chronobent-plugin-host COMMAND chronobent-plugin-host-test $<TARGET_BUNDLE_DIR:ChronoBent-au>)
endif()
