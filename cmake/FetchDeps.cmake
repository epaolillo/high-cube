include(FetchContent)

FetchContent_Declare(
  sqlitecpp
  GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
  GIT_TAG 3.3.2
  GIT_SHALLOW TRUE
)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INTERNAL_SQLITE OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sqlitecpp)

FetchContent_Declare(
  jwt_cpp
  GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
  GIT_TAG v0.7.0
  GIT_SHALLOW TRUE
)
set(JWT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(jwt_cpp)
