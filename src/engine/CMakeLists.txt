add_library(lb_engine STATIC
  core/clock.h
  core/no_mfc_guard.h
  engine.h
  engine.cpp
)

target_include_directories(lb_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lb_engine
  PUBLIC  ws2_32 mswsock
  PRIVATE nlohmann_json::nlohmann_json)
lb_set_project_options(lb_engine)
target_compile_definitions(lb_engine PRIVATE WIN32_LEAN_AND_MEAN LB_VERSION="${PROJECT_VERSION}")

# Every engine translation unit starts with the MFC guard (plan II.6).
target_compile_options(lb_engine PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/core/no_mfc_guard.h")
