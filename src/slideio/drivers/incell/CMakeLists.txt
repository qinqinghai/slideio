set(LIBRARY_NAME ${INCELL_DRIVER_NAME})

set(SOURCE_FILES
   ${CMAKE_CURRENT_SOURCE_DIR}/incell_api_def.hpp
   ${CMAKE_CURRENT_SOURCE_DIR}/incellimagedriver.hpp
   ${CMAKE_CURRENT_SOURCE_DIR}/incellimagedriver.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/incellscene.hpp
   ${CMAKE_CURRENT_SOURCE_DIR}/incellscene.cpp
)

add_library(${LIBRARY_NAME} SHARED ${SOURCE_FILES})
HIDE_THIRD_PARTY_SYMBOLS(${LIBRARY_NAME})
set_target_properties(${LIBRARY_NAME} PROPERTIES OUTPUT_NAME "${LIBRARY_NAME}${LIB_NAME_SUFFIX}")

SOURCE_GROUP_BY_FOLDER(SOURCE_FILES)

find_package(OpenCV)
find_package(tinyxml2)
find_package(nlohmann_json REQUIRED)

target_link_libraries(${LIBRARY_NAME}
  opencv::opencv
  tinyxml2::tinyxml2
  nlohmann_json::nlohmann_json
)

target_link_libraries(${LIBRARY_NAME} ${CORE_LIB_NAME} ${IMAGETOOLS_LIB_NAME})

if(WIN32)
   target_link_libraries(${LIBRARY_NAME} Shlwapi)
   set_target_properties(${LIBRARY_NAME} PROPERTIES LINK_FLAGS "/ignore:4099")
   target_compile_definitions(${LIBRARY_NAME} PRIVATE SLIDEIO_INCELL_API _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(${LIBRARY_NAME} PROPERTIES LINKER_LANGUAGE CXX)
