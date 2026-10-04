include(GoogleTest)

# Unit tests: engine logic only, no sockets to real servers (plan IX "Unit").
add_executable(lb_unit_tests
  unit/engine_smoke_test.cpp
)
target_link_libraries(lb_unit_tests PRIVATE lb_engine GTest::gtest GTest::gtest_main)
lb_set_project_options(lb_unit_tests)
target_compile_definitions(lb_unit_tests PRIVATE LB_EXPECTED_VERSION="${PROJECT_VERSION}")
gtest_discover_tests(lb_unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS unit)

# Integration tests: engine + in-process mock backends on ephemeral ports (plan IX "Integration").
add_executable(lb_integration_tests
  integration/mock_backend_smoke_test.cpp
)
target_link_libraries(lb_integration_tests PRIVATE lb_engine mock_backend_lib GTest::gtest GTest::gtest_main)
lb_set_project_options(lb_integration_tests)
target_compile_definitions(lb_integration_tests PRIVATE WIN32_LEAN_AND_MEAN)
gtest_discover_tests(lb_integration_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS integration)
