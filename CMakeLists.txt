zephyr_include_directories(include)

# Core: the nanopb-generated proto, the shared per-operation dispatch, and the
# setting type-hint registry. Needed by BOTH the central Studio RPC handler and
# the split peripheral relay path (which runs the same dispatch against the
# peripheral's own store). The peripheral image has no ZMK_STUDIO, so this must
# NOT be gated on ZMK_SETTING_EXPOSE alone.
if(CONFIG_ZMK_SETTING_EXPOSE OR CONFIG_ZMK_SETTING_EXPOSE_SPLIT)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/setting_expose_dispatch.c)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/zmk_known_settings.c)
    zephyr_linker_sources(SECTIONS include/linker/zmk-setting-expose-entries.ld)

    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
    set(NANOPB_GENERATE_CPP_STANDALONE OFF)

    # NOTE: adding to app target directly causes build issues, so we create a library instead
    zephyr_library()
    file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

    nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
    target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

    # app should depend on the generated proto files
    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
    add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
endif()

# Central-side Studio RPC handler.
if(CONFIG_ZMK_SETTING_EXPOSE)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/setting_expose_handler.c)

    if(CONFIG_ZMK_SETTING_EXPOSE_UNIT_TEST)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/setting_expose_test.c)
    endif()
endif()

# Split event-relay glue (compiled on both split roles; relay macros are
# self-role-gating).
if(CONFIG_ZMK_SETTING_EXPOSE_SPLIT)
    file(GLOB_RECURSE SPLIT_C_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/split/*.c)
    target_sources(app PRIVATE ${SPLIT_C_FILES})
endif()
