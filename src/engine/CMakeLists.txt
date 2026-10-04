add_library(lb_engine STATIC
  core/clock.h
  core/no_mfc_guard.h
  core/debug_assert.h
  core/timer_service.h
  core/timer_service.cpp
  backend/backend_types.h
  backend/connection_pool.h
  backend/connection_pool.cpp
  backend/backend_runtime.h
  backend/backend_runtime.cpp
  backend/registry.h
  backend/registry.cpp
  balance/group_balancer.h
  balance/group_balancer.cpp
  metrics/metrics_types.h
  metrics/histogram.h
  metrics/metrics.h
  metrics/metrics.cpp
  log/event_types.h
  log/event_log.h
  log/event_log.cpp
  health/hysteresis.h
  health/health_checker.h
  health/health_checker.cpp
  config/config.h
  config/config_loader.h
  config/config_loader.cpp
  config/config_store.h
  http/http_message.h
  http/http_parser.h
  http/http_parser.cpp
  net/winsock.h
  net/winsock.cpp
  net/iocp.h
  net/iocp.cpp
  net/listener.h
  net/listener.cpp
  proxy/trace.h
  proxy/forwarding.h
  proxy/forwarding.cpp
  proxy/http_writer.h
  proxy/http_writer.cpp
  proxy/session_context.h
  proxy/session_context.cpp
  proxy/client_session.h
  proxy/client_session.cpp
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
