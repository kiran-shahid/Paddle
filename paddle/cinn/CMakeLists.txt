if(WITH_TESTING)
  cinn_cc_library(cinn_gtest_main SRCS gtest_main.cc DEPS gtest gflags)
endif()

add_subdirectory(auto_schedule)
add_subdirectory(common)
add_subdirectory(utils)
add_subdirectory(poly)
add_subdirectory(runtime)
add_subdirectory(ir)
add_subdirectory(backends)
add_subdirectory(lang)
add_subdirectory(optim)
add_subdirectory(hlir)
if(CINN_ONLY)
  add_subdirectory(pybind)
endif()
add_subdirectory(frontend)

# Download a model
download_and_uncompress("${DOWNLOAD_MODEL_DIR}" "${PADDLE_RESOURCE_URL}"
                        "lite_naive_model.tar.gz")

core_gather_headers()
