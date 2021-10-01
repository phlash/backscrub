cmake_minimum_required(VERSION 3.16)
project(backscrub CXX)

# globally set C++17 as default for compilation
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# allow override of Tensorflow location
if(NOT DEFINED TENSORFLOW)
  set(TENSORFLOW tensorflow)
  set(TF_CHECKOUT true)
endif()

# find Git and use it to get backscrub version
find_package(Git)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --all --long --always --dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE DEEPSEG_VERSION)
else()
  set(DEEPSEG_VERSION v0.2.0-no-git)
endif()
message("Version: ${DEEPSEG_VERSION}")

# always build PIC everywhere
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)

# use local platform OpenCV libraries & Minizip (via PkgConfig)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs video videoio highgui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Minizip minizip REQUIRED IMPORTED_TARGET)

# generate Metadata schema with flatc
# - for which we provide a custom command and a custom target
# - NB: CMake will detect 'flatc' is a target elsewhere (in Tensorflow Lite) and ensure
#   it is built for us to use here.. voodoo!
set(GEN_HEADER_PATH ${CMAKE_BINARY_DIR})
set(GEN_HEADER_SRC ${CMAKE_CURRENT_LIST_DIR}/models)
add_custom_command(OUTPUT ${GEN_HEADER_PATH}/metadata_schema_generated.h
  COMMAND flatc -c --scoped-enums -o ${GEN_HEADER_PATH}/ ${GEN_HEADER_SRC}/metadata_schema.fbs
  COMMENT "Compiling schema ${GEN_HEADER_SRC}/metadata_schema.fbs"
  DEPENDS ${GEN_HEADER_SRC}/metadata_schema.fbs)
add_custom_target(gen-headers ALL
  DEPENDS ${GEN_HEADER_PATH}/metadata_schema_generated.h)

# use .gitmodules defined Tensorflow version unless a path was provided
if (TF_CHECKOUT)
  if (GIT_FOUND)
    message(STATUS "Updating Tensorflow source")
    execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive)
  else()
    message(FATAL_ERROR "Git not found. Unable to checkout required Tensorflow version")
  endif()
else()
  message(STATUS "Using specified Tensorflow: ${TENSORFLOW}")
endif(TF_CHECKOUT)

# force compilation of XNNPACK delegate (without this the too clever-by-half use
# of weak/strong symbol linking fails in a static library)
add_compile_definitions(TFLITE_BUILD_WITH_XNNPACK_DELEGATE)

# Stop broken xnnpack/FP16 cmake goop from downloading PSimd to our source folder!
# TODO: check once tensorflow is updated post 2.5.0
set(CONFU_DEPENDENCIES_SOURCE_DIR ${CMAKE_BINARY_DIR})
set(CONFU_DEPENDENCIES_BINARY_DIR ${CMAKE_BINARY_DIR}/_deps)

# pull in Tensorflow Lite source build
add_subdirectory(${TENSORFLOW}/tensorflow/lite
  "${CMAKE_CURRENT_BINARY_DIR}/tensorflow-lite" EXCLUDE_FROM_ALL)

# add dependency from tensorflow-lite library to generated headers custom target
add_dependencies(tensorflow-lite gen-headers)

# build backscrub code
add_compile_definitions(DEEPSEG_VERSION=${DEEPSEG_VERSION})
include_directories(BEFORE . ${GEN_HEADER_PATH})

add_library(backscrub
  lib/libbackscrub.cc
  lib/transpose_conv_bias.cc)

target_link_libraries(backscrub
  tensorflow-lite ${CMAKE_DL_LIBS}
  opencv_core
  opencv_imgproc
  PkgConfig::Minizip
)

# We don't build the Linux-specific wrapper application on Windows
if(NOT WIN32)
add_library(videoio
  videoio/loopback.cc)

target_link_libraries(videoio)

add_executable(deepseg
  app/deepseg.cc
)

set_target_properties(deepseg PROPERTIES OUTPUT_NAME backscrub)

target_link_libraries(deepseg
  backscrub
  videoio
  opencv_core
  opencv_video
  opencv_videoio
  opencv_imgproc
  opencv_imgcodecs
  opencv_highgui
)
endif()

# Export our library, and all transitive dependencies - sadly Tensorflow Lite's
# CMakeLists.txt does not export these for us
function(get_link_libraries target outlist)
  message(STATUS "get_link_libraries(${target} ${outlist})")
  # recursive dependency munger, feed with a name and a list to extend
  get_target_property(target_type ${target} TYPE)
  if(${target_type} STREQUAL "INTERFACE_LIBRARY")
    get_target_property(libs ${target} INTERFACE_LINK_LIBRARIES)
  else()
    get_target_property(libs ${target} LINK_LIBRARIES)
  endif()
  foreach(lib IN LISTS libs)
    if(NOT TARGET ${lib})
      continue()
    endif()
    get_target_property(unalias ${lib} ALIASED_TARGET)
	if("${unalias}" STREQUAL "unalias-NOTFOUND")
      set(unalias ${lib})
    endif()
	get_target_property(imp ${unalias} IMPORTED)
	if(${imp})
	  continue()
	endif()
    list(FIND ${outlist} ${unalias} exists)
    if(NOT exists EQUAL -1)
      continue()
    endif()
    list(APPEND ${outlist} ${unalias})
    get_link_libraries(${unalias} ${outlist})
    set(${outlist} ${${outlist}} PARENT_SCOPE)
  endforeach()
endfunction()

set(BACKSCRUB_DEPS "")
get_link_libraries(backscrub BACKSCRUB_DEPS)

export(TARGETS
  backscrub
  ${BACKSCRUB_DEPS}
  FILE BackscrubTargets.cmake)

# installation names for library, header and models
if(NOT WIN32)
install(TARGETS deepseg)
endif()
install(TARGETS backscrub)
install(FILES lib/libbackscrub.h DESTINATION include/backscrub)
install(DIRECTORY models
  DESTINATION ${CMAKE_INSTALL_PREFIX}/share/backscrub
  FILES_MATCHING PATTERN "*.tflite" PATTERN "*.md")
