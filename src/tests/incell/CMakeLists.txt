set(INCLUDE_ROOT ${CMAKE_SOURCE_DIR}/src)
set(TEST_NAME slideio_incell_tests)
set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_executable(${TEST_NAME} test_incell_driver.cpp)
target_include_directories(${TEST_NAME} PRIVATE ${INCLUDE_ROOT})

find_package(GTest)
find_package(OpenCV)

target_link_libraries(${TEST_NAME}
   gtest::gtest
   opencv::opencv
   ${CORE_LIB_NAME}
   ${INCELL_DRIVER_NAME}
   ${IMAGETOOLS_LIB_NAME}
   ${TEST_LIB_NAME}
   ${SLIDEIO_LIB_NAME}
)

if(WIN32)
   set_target_properties(${TEST_NAME} PROPERTIES LINK_FLAGS "/ignore:4099")
   target_compile_definitions(${TEST_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

slideio_add_test(${TEST_NAME} ${TEST_DIR}/${TEST_NAME})
FIX_MACOS_RPATH(${TEST_NAME})
