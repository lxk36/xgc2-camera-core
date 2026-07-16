cmake_minimum_required(VERSION 3.10)
project(xgc2_camera VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(XGC2_CAMERA_BUILD_TESTING "Build xgc2_camera tests" ON)
option(XGC2_CAMERA_BUILD_TOOLS "Build xgc2_camera command-line tools" ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "xgc2_camera currently requires Linux V4L2")
endif()

find_package(Threads REQUIRED)

add_library(xgc2_camera SHARED
  src/camera.cpp
  src/synthetic_camera.cpp
  src/v4l2_camera.cpp
)
add_library(xgc2::camera ALIAS xgc2_camera)
set_target_properties(xgc2_camera PROPERTIES
  OUTPUT_NAME xgc2_camera
  EXPORT_NAME camera
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
)
target_compile_features(xgc2_camera PUBLIC cxx_std_14)
target_compile_options(xgc2_camera PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(xgc2_camera
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(xgc2_camera PRIVATE Threads::Threads)

if(XGC2_CAMERA_BUILD_TOOLS)
  add_executable(xgc2-camera-inspect tools/xgc2-camera-inspect.cpp)
  target_compile_features(xgc2-camera-inspect PRIVATE cxx_std_14)
  target_compile_options(xgc2-camera-inspect PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(xgc2-camera-inspect PRIVATE xgc2::camera)
endif()

install(TARGETS xgc2_camera
  EXPORT xgc2_cameraTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(XGC2_CAMERA_BUILD_TOOLS)
  install(TARGETS xgc2-camera-inspect
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
  cmake/xgc2_cameraConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/xgc2_cameraConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_camera"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/xgc2_cameraConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/xgc2_cameraConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/xgc2_cameraConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_camera"
)
install(EXPORT xgc2_cameraTargets
  NAMESPACE xgc2::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_camera"
)

configure_file(pkgconfig/xgc2-camera.pc.in
  "${CMAKE_CURRENT_BINARY_DIR}/xgc2-camera.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/xgc2-camera.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

install(FILES README.md LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/libxgc2-camera-dev")

if(XGC2_CAMERA_BUILD_TESTING)
  include(CTest)
  if(BUILD_TESTING)
    add_executable(xgc2_camera_test test/camera_test.cpp)
    target_compile_features(xgc2_camera_test PRIVATE cxx_std_14)
    target_compile_options(xgc2_camera_test PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(xgc2_camera_test PRIVATE src)
    target_link_libraries(xgc2_camera_test PRIVATE xgc2::camera)
    add_test(NAME xgc2_camera_test COMMAND xgc2_camera_test)
  endif()
endif()
