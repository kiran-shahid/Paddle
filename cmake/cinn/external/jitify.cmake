if(NOT WITH_GPU)
  set(JITIFY_FOUND OFF)
  return()
endif()

include(ExternalProject)

set(JITIFY_SOURCE_PATH ${CINN_THIRD_PARTY_PATH}/install/jitify)

ExternalProject_Add(
  external_jitify
  ${EXTERNAL_PROJECT_LOG_ARGS}
  GIT_REPOSITORY "https://github.com/NVIDIA/jitify.git"
  GIT_TAG 57de649139c866eb83acacfe50c92ad7c6278776
  PREFIX ${CINN_THIRD_PARTY_PATH}/jitify
  SOURCE_DIR ${JITIFY_SOURCE_PATH}
  CONFIGURE_COMMAND ""
  PATCH_COMMAND ""
  BUILD_COMMAND ""
  UPDATE_COMMAND ""
  INSTALL_COMMAND "")

include_directories(${JITIFY_SOURCE_PATH})

add_library(extern_jitify INTERFACE)
add_dependencies(extern_jitify external_jitify)
set(jitify_deps extern_jitify)
