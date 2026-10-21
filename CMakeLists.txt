cmake_minimum_required(VERSION 3.20)
project(high-cube VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchDeps)

find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)

# Protobuf generation
set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/highcube.proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
set(PROTO_SRCS "${GENERATED_DIR}/highcube.pb.cc")
set(PROTO_HDRS "${GENERATED_DIR}/highcube.pb.h")

add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
  COMMAND protobuf::protoc
          --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
          --cpp_out=${GENERATED_DIR}
          ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  COMMENT "Generating Protobuf sources"
  VERBATIM
)

add_library(hc_proto STATIC ${PROTO_SRCS})
target_include_directories(hc_proto PUBLIC ${GENERATED_DIR})
target_link_libraries(hc_proto PUBLIC protobuf::libprotobuf)

add_library(hc_common INTERFACE)
target_include_directories(hc_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_common INTERFACE spdlog::spdlog nlohmann_json::nlohmann_json)

# ---------- hc-admin ----------
set(ADMIN_SOURCES
  src/admin/main.cpp
  src/admin/config/admin_config.cpp
  src/admin/db/database.cpp
  src/admin/db/migration_runner.cpp
  src/admin/db/repositories.cpp
  src/admin/auth/password.cpp
  src/admin/auth/jwt_service.cpp
  src/admin/auth/auth_middleware.cpp
  src/admin/api/http_server.cpp
  src/admin/api/router.cpp
  src/admin/api/handlers.cpp
  src/admin/services/json_helpers.cpp
  src/admin/services/user_service.cpp
  src/admin/services/activity_service.cpp
  src/admin/services/settings_service.cpp
  src/admin/services/server_service.cpp
  src/admin/services/state_service.cpp
  src/admin/services/application_service.cpp
  src/admin/services/db_admin_service.cpp
  src/admin/services/deployment_service.cpp
  src/admin/services/placement_service.cpp
  src/admin/services/metrics_retention_service.cpp
  src/admin/services/autoscaler_service.cpp
  src/admin/services/supervisor_service.cpp
  src/admin/services/github_app_service.cpp
  src/admin/github/github_http_client.cpp
  src/admin/github/github_app_auth.cpp
  src/admin/agents/agent_hub.cpp
  src/admin/presence/presence_hub.cpp
  src/admin/agents/session.cpp
  src/admin/agents/host_terminal.cpp
  src/admin/agents/terminal_polling.cpp
  src/admin/proxy/nginx_proxy.cpp
  src/admin/certificates/certbot_service.cpp
  src/admin/bootstrap/bootstrap.cpp
)

add_executable(hc-admin ${ADMIN_SOURCES})
target_include_directories(hc-admin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/admin
)
target_link_libraries(hc-admin PRIVATE
  hc_common
  hc_proto
  SQLiteCpp
  jwt-cpp::jwt-cpp
  Boost::system
  Boost::filesystem
  OpenSSL::SSL
  OpenSSL::Crypto
  crypt
  pthread
)
target_compile_definitions(hc-admin PRIVATE
  HIGH_CUBE_MIGRATIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/migrations"
)

# ---------- hc-agent ----------
set(AGENT_SOURCES
  src/agent/main.cpp
  src/agent/config/agent_config.cpp
  src/agent/connection/ws_client.cpp
  src/agent/protocol/codec.cpp
  src/agent/runtime/docker_runtime.cpp
  src/agent/runtime/db_manage.cpp
  src/agent/runtime/git_checkout.cpp
  src/agent/metrics/host_metrics.cpp
  src/agent/metrics/container_metrics.cpp
  src/agent/logs/log_streamer.cpp
  src/agent/terminal/terminal_session.cpp
  src/agent/bootstrap/bootstrap.cpp
)

add_executable(hc-agent ${AGENT_SOURCES})
target_include_directories(hc-agent PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/agent
)
target_link_libraries(hc-agent PRIVATE
  hc_common
  hc_proto
  Boost::system
  Boost::filesystem
  OpenSSL::SSL
  OpenSSL::Crypto
  pthread
  util
)

install(TARGETS hc-admin hc-agent RUNTIME DESTINATION bin)
