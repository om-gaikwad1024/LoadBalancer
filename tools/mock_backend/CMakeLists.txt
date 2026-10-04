# Mock HTTP backend (plan IX). Deliberately independent of lb_engine: it has its own
# minimal HTTP parsing so a parser bug cannot hide on both sides of the proxy.
add_library(mock_backend_lib STATIC
  mock_server.h
  mock_server.cpp
)
target_include_directories(mock_backend_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mock_backend_lib PUBLIC ws2_32)
lb_set_project_options(mock_backend_lib)
target_compile_definitions(mock_backend_lib PRIVATE WIN32_LEAN_AND_MEAN)

add_executable(mock_backend main.cpp)
target_link_libraries(mock_backend PRIVATE mock_backend_lib)
lb_set_project_options(mock_backend)
target_compile_definitions(mock_backend PRIVATE WIN32_LEAN_AND_MEAN)
