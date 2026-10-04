include(GoogleTest)

# Unit tests: engine logic only, no sockets to real servers (plan IX "Unit").
add_executable(lb_unit_tests
  unit/engine_smoke_test.cpp
  unit/config_loader_test.cpp
  unit/config_store_test.cpp
  unit/http_test_util.h
  unit/http_parser_test.cpp
  unit/http_corpus_test.cpp
  unit/connection_pool_test.cpp
  unit/timer_service_test.cpp
  unit/forwarding_test.cpp
  unit/group_balancer_test.cpp
)
target_link_libraries(lb_unit_tests PRIVATE lb_engine nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)
lb_set_project_options(lb_unit_tests)
target_compile_definitions(lb_unit_tests PRIVATE
  LB_EXPECTED_VERSION="${PROJECT_VERSION}"
  LB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gtest_discover_tests(lb_unit_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES LABELS unit)

# Integration tests: engine + in-process mock backends on ephemeral ports (plan IX "Integration").
add_executable(lb_integration_tests
  integration/test_http_client.h
  integration/test_process.h
  integration/test_config.h
  integration/scripted_backend.h
  integration/proxy_test_fixture.h
  integration/mock_backend_test.cpp
  integration/proxy_forwarding_test.cpp
  integration/proxy_lifecycle_test.cpp
  integration/proxy_pool_test.cpp
  integration/proxy_headers_timeouts_test.cpp
  integration/proxy_balancing_test.cpp
)
target_link_libraries(lb_integration_tests PRIVATE
  lb_engine mock_backend_lib nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)
lb_set_project_options(lb_integration_tests)
target_compile_definitions(lb_integration_tests PRIVATE
  WIN32_LEAN_AND_MEAN
  LB_MOCK_BACKEND_EXE=L"$<TARGET_FILE:mock_backend>")
add_dependencies(lb_integration_tests mock_backend)
gtest_discover_tests(lb_integration_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES LABELS integration)

# Parser fuzz target (plan IX). Run with: tools\build.cmd fuzz all [seconds]
if(LB_BUILD_FUZZ)
  add_executable(lb_parser_fuzz fuzz/parser_fuzz.cpp)
  target_link_libraries(lb_parser_fuzz PRIVATE lb_engine)
  lb_set_project_options(lb_parser_fuzz)
  target_compile_options(lb_parser_fuzz PRIVATE /fsanitize=fuzzer)
endif()
