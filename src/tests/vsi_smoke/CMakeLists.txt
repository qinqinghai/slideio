set(SMOKE_NAME slideio_vsi_smoke)

add_executable(${SMOKE_NAME} vsi_smoke.cpp)
target_include_directories(${SMOKE_NAME} PRIVATE ${INCLUDE_ROOT})
target_link_libraries(${SMOKE_NAME} PRIVATE
    ${SLIDEIO_LIB_NAME}
    ${IMAGETOOLS_LIB_NAME}
    ${CORE_LIB_NAME}
    ${VSI_DRIVER_NAME}
    opencv::opencv
    openjp2
    nlohmann_json::nlohmann_json
)

# Derive the smoke VSI from SLIDEIO_IMAGES_PATH (stc-cellflex-data fixture).
set(_vsi_smoke_sample "")
if(SLIDEIO_IMAGES_PATH)
    set(_vsi_smoke_candidate
        "${SLIDEIO_IMAGES_PATH}/vsi/vsi-multifile/vsi-ets-test-jpg2k.vsi")
    if(EXISTS "${_vsi_smoke_candidate}")
        set(_vsi_smoke_sample "${_vsi_smoke_candidate}")
    endif()
endif()
if(_vsi_smoke_sample)
    target_compile_definitions(${SMOKE_NAME} PRIVATE
        SLIDEIO_VSI_SAMPLE="${_vsi_smoke_sample}"
    )
else()
    message(STATUS
        "slideio_vsi_smoke: VSI sample skipped "
        "(need SLIDEIO_IMAGES_PATH/vsi/vsi-multifile/vsi-ets-test-jpg2k.vsi)")
endif()

add_test(NAME ${SMOKE_NAME} COMMAND ${SMOKE_NAME})
