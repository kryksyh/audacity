if (OS_IS_MAC AND MUSE_COMPILE_ASAN)
    # Allow the UI test runner to preload this fixture for its process lifetime.
    # Apple Clang ASan leaves poisoned global redzones after dlclose, which can
    # otherwise flag unrelated allocations reusing the unloaded image's address.
    # A Mach-O bundle cannot be preloaded; retain the manifest's .so filename.
    add_library(native_lv2_fixture SHARED instrumentfixture.cpp instrumentui.cpp)
    set_target_properties(native_lv2_fixture PROPERTIES SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}")
else()
    add_library(native_lv2_fixture MODULE instrumentfixture.cpp instrumentui.cpp)
endif()
target_link_libraries(native_lv2_fixture PRIVATE lv2sdk Qt::Widgets)
set_target_properties(native_lv2_fixture PROPERTIES
    PREFIX "" UNITY_BUILD OFF AUTOMOC OFF AUTOUIC OFF
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native-instrument.lv2"
)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-instrument.lv2/manifest.ttl" CONTENT
"@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
<urn:audacity:test:native-instrument> a lv2:Plugin ; lv2:binary <$<TARGET_FILE_NAME:native_lv2_fixture>> ; rdfs:seeAlso <instrument.ttl> .
<urn:audacity:test:fixed-instrument> a lv2:Plugin ; lv2:binary <$<TARGET_FILE_NAME:native_lv2_fixture>> ; rdfs:seeAlso <instrument.ttl> .
<urn:audacity:test:delayed-instrument> a lv2:Plugin ; lv2:binary <$<TARGET_FILE_NAME:native_lv2_fixture>> ; rdfs:seeAlso <instrument.ttl> .
<urn:audacity:test:fixed-audio-effect> a lv2:Plugin ; lv2:binary <$<TARGET_FILE_NAME:native_lv2_fixture>> ; rdfs:seeAlso <instrument.ttl> .
<urn:audacity:test:audio-effect> a lv2:Plugin ; lv2:binary <$<TARGET_FILE_NAME:native_lv2_fixture>> ; rdfs:seeAlso <instrument.ttl> .
")
configure_file(instrument.ttl "${CMAKE_CURRENT_BINARY_DIR}/native-instrument.lv2/instrument.ttl" @ONLY)
