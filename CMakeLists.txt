cmake_minimum_required(VERSION 3.8)

project(test_restlibs CXX)

find_path(LIBUV_INCLUDE_DIR uv.h)
find_library(LIBUV_LIBRARIES NAMES libuv uv)
if (WIN32)
  list(APPEND LIBUV_LIBRARIES "Ws2_32.lib")
endif()

find_path(HTTP_PARSER_INCLUDE_DIR http_parser.h)
find_library(HTTP_PARSER_LIBRARIES http_parser)

add_executable(test_libuv main.cpp)
target_include_directories(test_libuv PRIVATE
	"${LIBUV_INCLUDE_DIR}"
	"${HTTP_PARSER_INCLUDE_DIR}")
target_link_libraries(test_libuv
	"${LIBUV_LIBRARIES}"
	"${HTTP_PARSER_LIBRARIES}")
