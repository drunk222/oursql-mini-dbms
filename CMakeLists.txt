cmake_minimum_required(VERSION 3.20)

project(OurSQL VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 在中文 Windows 上默认用 GBK(代码页 936) 读源码，会导致 UTF-8 源文件中的
# 中文注释/字符串被误解析。该配置必须放在所有 target 之前。
if(MSVC)
  add_compile_options(/utf-8 /W4 /EHsc)
endif()

add_library(oursql_core
  src/common/status.cpp
  src/common/value.cpp
  src/common/schema.cpp
  src/catalog/catalog.cpp
  src/parser/parser.cpp
  src/planner/planner.cpp
  src/optimizer/optimizer.cpp
  src/optimizer/expression_optimizer.cpp
  src/execution/executor.cpp
  src/execution/database_engine.cpp
  src/parser/lexer.cpp
  src/storage/row_codec.cpp
  src/storage/heap_table.cpp
  src/storage/storage.cpp
  src/index/b_plus_tree_page.cpp
  src/index/b_plus_tree.cpp
  src/index/index_manager.cpp
)

target_include_directories(oursql_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(oursql_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(oursql src/app/main.cpp)
target_link_libraries(oursql PRIVATE oursql_core)
if (MINGW)
  target_link_options(oursql PRIVATE -municode)
endif()

add_executable(oursql_tests tests/oursql_tests.cpp)
target_link_libraries(oursql_tests PRIVATE oursql_core)

add_executable(oursql_storage_tests tests/storage_disk_manager_tests.cpp)
target_link_libraries(oursql_storage_tests PRIVATE oursql_core)

add_executable(oursql_slotted_page_tests tests/slotted_page_tests.cpp)
target_link_libraries(oursql_slotted_page_tests PRIVATE oursql_core)

add_executable(oursql_frontend_tests tests/frontend_tests.cpp)
target_link_libraries(oursql_frontend_tests PRIVATE oursql_core)

add_executable(oursql_optimizer_tests tests/optimizer_tests.cpp)
target_link_libraries(oursql_optimizer_tests PRIVATE oursql_core)

add_executable(oursql_catalog_heap_tests tests/catalog_heap_tests.cpp)
target_link_libraries(oursql_catalog_heap_tests PRIVATE oursql_core)

add_executable(oursql_execution_tests tests/execution_tests.cpp)
target_link_libraries(oursql_execution_tests PRIVATE oursql_core)

add_executable(oursql_edge_case_tests tests/edge_case_tests.cpp)
target_link_libraries(oursql_edge_case_tests PRIVATE oursql_core)

add_executable(oursql_b_plus_tree_tests tests/b_plus_tree_tests.cpp)
target_link_libraries(oursql_b_plus_tree_tests PRIVATE oursql_core)

add_executable(oursql_benchmark benchmarks/storage_benchmark.cpp)
target_link_libraries(oursql_benchmark PRIVATE oursql_core)

enable_testing()
add_test(NAME oursql_tests COMMAND oursql_tests)
add_test(NAME oursql_storage_tests COMMAND oursql_storage_tests)
add_test(NAME oursql_slotted_page_tests COMMAND oursql_slotted_page_tests)
add_test(NAME oursql_frontend_tests COMMAND oursql_frontend_tests)
add_test(NAME oursql_optimizer_tests COMMAND oursql_optimizer_tests)
add_test(NAME oursql_catalog_heap_tests COMMAND oursql_catalog_heap_tests)
add_test(NAME oursql_execution_tests COMMAND oursql_execution_tests)
add_test(NAME oursql_edge_case_tests COMMAND oursql_edge_case_tests)
add_test(NAME oursql_b_plus_tree_tests COMMAND oursql_b_plus_tree_tests)
