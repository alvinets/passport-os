# ESP-IDF 6.x replaces the "json" component with the managed "cjson" component.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(PASSPORT_JSON_COMPONENT cjson)
else()
    set(PASSPORT_JSON_COMPONENT json)
endif()
idf_component_register(
    SRCS "passport_workbuddy.cc" "config_store.c" "text.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES passport_muse bsp esp-wifi-connect ${PASSPORT_JSON_COMPONENT} nvs_flash
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
