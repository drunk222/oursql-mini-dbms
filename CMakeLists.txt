cmake_minimum_required(VERSION 3.20)

project(OurSQL VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(oursql_core
  src/common/status.cpp
  src/common/value.cpp
  src/common/schema.cpp
  src/catalog/catalog.cpp
  src/parser/parser.cpp
  src/planner/planner.cpp
  src/execution/executor.cpp
  src/parser/lexer.cpp
  src/storage/row_codec.cpp
  src/storage/heap_table.cpp
  src/storage/storage.cpp
)

target_include_directories(oursql_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(oursql_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(oursql src/app/main.cpp)
target_link_libraries(oursql PRIVATE oursql_core)

add_executable(oursql_tests tests/oursql_tests.cpp)
target_link_libraries(oursql_tests PRIVATE oursql_core)

add_executable(oursql_storage_tests tests/storage_disk_manager_tests.cpp)
target_link_libraries(oursql_storage_tests PRIVATE oursql_core)

add_executable(oursql_slotted_page_tests tests/slotted_page_tests.cpp)
target_link_libraries(oursql_slotted_page_tests PRIVATE oursql_core)

add_executable(oursql_frontend_tests tests/frontend_tests.cpp)
target_link_libraries(oursql_frontend_tests PRIVATE oursql_core)

add_executable(oursql_catalog_heap_tests tests/catalog_heap_tests.cpp)
target_link_libraries(oursql_catalog_heap_tests PRIVATE oursql_core)

enable_testing()
add_test(NAME oursql_tests COMMAND oursql_tests)
add_test(NAME oursql_storage_tests COMMAND oursql_storage_tests)
add_test(NAME oursql_slotted_page_tests COMMAND oursql_slotted_page_tests)
add_test(NAME oursql_frontend_tests COMMAND oursql_frontend_tests)
add_test(NAME oursql_catalog_heap_tests COMMAND oursql_catalog_heap_tests)
