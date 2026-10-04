# Headless proxy host for load tests and scripted scenarios (the MFC app is the operator UI).
add_executable(lb_console main.cpp)
target_link_libraries(lb_console PRIVATE lb_engine nlohmann_json::nlohmann_json)
lb_set_project_options(lb_console)
target_compile_definitions(lb_console PRIVATE WIN32_LEAN_AND_MEAN)
