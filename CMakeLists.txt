cmake_minimum_required(VERSION 3.20)
project(fixgen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---------------------------------------------------------------- core
add_library(fixcore
  core/src/message.cpp core/src/codec.cpp core/src/order.cpp core/src/dictionary.cpp
  core/src/spec_adapter.cpp core/src/session.cpp core/src/net.cpp core/src/json.cpp)
target_include_directories(fixcore PUBLIC core/include)

# ---------------------------------------------------------------- generated venues
# Every spec/venues/*.yaml becomes a venue adapter. Re-run cmake after adding a file.
file(GLOB VENUE_SPECS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/spec/venues/*.yaml)
file(GLOB CODEGEN_DEPS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/tools/fixgen/*.py ${CMAKE_SOURCE_DIR}/tools/fixgen/templates/*.j2
  ${CMAKE_SOURCE_DIR}/spec/schema/*.json)
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(GEN_SOURCES ${GEN_DIR}/registry.cpp)
foreach(spec ${VENUE_SPECS})
  get_filename_component(venue ${spec} NAME_WE)
  list(APPEND GEN_SOURCES ${GEN_DIR}/${venue}.cpp)
  list(APPEND GEN_HEADERS ${GEN_DIR}/fixgen/venues/${venue}.hpp)
endforeach()
add_custom_command(
  OUTPUT ${GEN_SOURCES} ${GEN_HEADERS}
  COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_SOURCE_DIR}/tools
          ${Python3_EXECUTABLE} -m fixgen.cli codegen --out ${GEN_DIR} ${VENUE_SPECS}
  DEPENDS ${VENUE_SPECS} ${CODEGEN_DEPS}
  COMMENT "Generating venue adapters from spec/venues"
  VERBATIM)
add_library(fixgen_venues ${GEN_SOURCES})
target_include_directories(fixgen_venues PUBLIC ${GEN_DIR})
target_link_libraries(fixgen_venues PUBLIC fixcore)

# ---------------------------------------------------------------- apps
add_executable(fixgen-gateway gateway/src/main.cpp gateway/src/http.cpp)
target_link_libraries(fixgen-gateway PRIVATE fixgen_venues)

add_executable(fixgen-sim sim/src/main.cpp)
target_link_libraries(fixgen-sim PRIVATE fixgen_venues)

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(fixcore_tests core/tests/test_main.cpp core/tests/test_codec.cpp
  core/tests/test_session.cpp core/tests/test_adapters.cpp core/tests/test_json.cpp)
target_link_libraries(fixcore_tests PRIVATE fixgen_venues)
add_test(NAME fixcore_tests COMMAND fixcore_tests)
