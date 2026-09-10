# SPDX-License-Identifier: MIT
# The pinned framework sets this file property in its first calling directory.
# Repeat only the build property for this second target directory, not its source.
set_source_files_properties(../mac_editor.mm ../mac_audio.mm ../../host/mac_tuning_editor.mm ../../host/mac_controls.mm
    "${IPLUG2_DIR}/IPlug/Extras/WebView/IPlugWebView_mac.mm"
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
iplug_add_plugin(ChronoBentFX
    SOURCES ChronoBentFX.cpp ChronoBentFX.h config.h ../host.hpp ../mac_editor.mm ../mac_audio.mm ../mac_audio.hpp ../tuning_state.cpp ../tuning_state.hpp
        ../../host/mac_tuning_editor.mm ../../host/mac_controls.mm
    RESOURCES ../../LICENSE ../THIRD_PARTY_NOTICES.md
    WEB_RESOURCES resources/web/index.html resources/web/script.js resources/web/fx.css
        ../resources/web/style.css ../resources/web/controls.js
    LINK chronobent-effect chronobent-tuning-host "-framework AVFoundation" "-framework Cocoa"
    UI WEBVIEW FORMATS AU VST3)
foreach(format au vst3)
    target_compile_features(ChronoBentFX-${format} PRIVATE cxx_std_17)
    target_compile_definitions(ChronoBentFX-${format} PRIVATE CHRONOBENT_TUNE_OBJC_PREFIX=ChronoBentEffect_${format})
    add_custom_command(TARGET ChronoBentFX-${format} POST_BUILD
        COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:ChronoBentFX-${format}>"
        COMMENT "Ad-hoc sign the complete local effect bundle" VERBATIM)
endforeach()
if(CHRONOBENT_BUILD_TESTS)
    add_executable(chronobent-effect-host-test ../../tests/test_effect_host.mm)
    target_compile_features(chronobent-effect-host-test PRIVATE cxx_std_17)
    target_compile_options(chronobent-effect-host-test PRIVATE -fobjc-arc)
    target_link_libraries(chronobent-effect-host-test PRIVATE "-framework Cocoa" "-framework AudioUnit" "-framework AudioToolbox" "-framework WebKit")
    if(CHRONOBENT_SANITIZE)
        target_compile_options(chronobent-effect-host-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(chronobent-effect-host-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_dependencies(chronobent-effect-host-test ChronoBentFX-au)
    add_test(NAME chronobent-effect-host COMMAND chronobent-effect-host-test $<TARGET_BUNDLE_DIR:ChronoBentFX-au>)
    set(CHRONOBENT_VST3_SDK "${IPLUG2_DIR}/Dependencies/IPlug/VST3_SDK")
    add_executable(chronobent-effect-vst3-test ../../tests/test_effect_vst3.mm
        "${CHRONOBENT_VST3_SDK}/pluginterfaces/base/funknown.cpp"
        "${CHRONOBENT_VST3_SDK}/pluginterfaces/base/coreiids.cpp"
        "${CHRONOBENT_VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp")
    target_compile_features(chronobent-effect-vst3-test PRIVATE cxx_std_17)
    target_compile_options(chronobent-effect-vst3-test PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_compile_definitions(chronobent-effect-vst3-test PRIVATE RELEASE=1)
    target_include_directories(chronobent-effect-vst3-test PRIVATE "${CHRONOBENT_VST3_SDK}")
    target_link_libraries(chronobent-effect-vst3-test PRIVATE "-framework Cocoa")
    if(CHRONOBENT_SANITIZE)
        target_compile_options(chronobent-effect-vst3-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(chronobent-effect-vst3-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_dependencies(chronobent-effect-vst3-test ChronoBentFX-vst3)
    add_test(NAME chronobent-effect-vst3 COMMAND chronobent-effect-vst3-test $<TARGET_BUNDLE_DIR:ChronoBentFX-vst3>)
endif()
