cmake_minimum_required(VERSION 3.20)
project(fixgen LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---------------------------------------------------------------- QuickFIX (session layer)
# Pinned release. For offline builds point FETCHCONTENT_SOURCE_DIR_QUICKFIX at a checkout.
include(FetchContent)
set(QUICKFIX_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QUICKFIX_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUICKFIX_TESTS OFF CACHE BOOL "" FORCE)
set(QUICKFIX_LIB_OUTPUT_DIR ${CMAKE_BINARY_DIR}/quickfix CACHE PATH "" FORCE)
FetchContent_Declare(quickfix
  GIT_REPOSITORY https://github.com/quickfix/quickfix.git
  GIT_TAG v1.16.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(quickfix)

set(FIXGEN_WARNINGS -Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------- core (no QuickFIX dependency)
add_library(fixcore
  core/src/message.cpp core/src/codec.cpp core/src/order.cpp core/src/dictionary.cpp
  core/src/spec_adapter.cpp core/src/net.cpp core/src/json.cpp)
target_include_directories(fixcore PUBLIC core/include)
target_compile_options(fixcore PRIVATE ${FIXGEN_WARNINGS})

# ---------------------------------------------------------------- generated venues
# Every spec/venues/*.yaml becomes a venue adapter + QuickFIX dictionaries.
file(GLOB VENUE_SPECS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/spec/venues/*.yaml)
file(GLOB CODEGEN_DEPS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/tools/fixgen/*.py ${CMAKE_SOURCE_DIR}/tools/fixgen/templates/*.j2
  ${CMAKE_SOURCE_DIR}/spec/schema/*.json)
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(GEN_SOURCES ${GEN_DIR}/registry.cpp)
foreach(spec ${VENUE_SPECS})
  get_filename_component(venue ${spec} NAME_WE)
  list(APPEND GEN_SOURCES ${GEN_DIR}/${venue}.cpp)
  list(APPEND GEN_OTHER ${GEN_DIR}/fixgen/venues/${venue}.hpp ${GEN_DIR}/dict/${venue}.xml)
endforeach()
add_custom_command(
  OUTPUT ${GEN_SOURCES} ${GEN_OTHER}
  COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_SOURCE_DIR}/tools
          ${Python3_EXECUTABLE} -m fixgen.cli codegen --out ${GEN_DIR} ${VENUE_SPECS}
  DEPENDS ${VENUE_SPECS} ${CODEGEN_DEPS}
  COMMENT "Generating venue adapters and QuickFIX dictionaries from spec/venues"
  VERBATIM)
add_library(fixgen_venues ${GEN_SOURCES})
target_include_directories(fixgen_venues PUBLIC ${GEN_DIR})
target_link_libraries(fixgen_venues PUBLIC fixcore)
target_compile_options(fixgen_venues PRIVATE ${FIXGEN_WARNINGS})

# ---------------------------------------------------------------- QuickFIX bridge
add_library(fixgen_qf core/qf/bridge.cpp)
target_include_directories(fixgen_qf PUBLIC core/qf)
target_link_libraries(fixgen_qf PUBLIC fixgen_venues quickfix)
add_dependencies(fixgen_qf QUICKFIX_HEADERS_COPY)  # QuickFIX copies its public headers in a separate target
target_compile_definitions(fixgen_qf PUBLIC FIXGEN_DEFAULT_DICT_DIR="${GEN_DIR}/dict")
target_compile_options(fixgen_qf PRIVATE ${FIXGEN_WARNINGS})

# ---------------------------------------------------------------- apps
add_executable(fixgen-gateway gateway/src/main.cpp gateway/src/http.cpp)
target_link_libraries(fixgen-gateway PRIVATE fixgen_qf)
target_compile_options(fixgen-gateway PRIVATE ${FIXGEN_WARNINGS})

add_executable(fixgen-sim sim/src/main.cpp)
target_link_libraries(fixgen-sim PRIVATE fixgen_qf)
target_compile_options(fixgen-sim PRIVATE ${FIXGEN_WARNINGS})

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(fixcore_tests core/tests/test_main.cpp core/tests/test_codec.cpp
  core/tests/test_adapters.cpp core/tests/test_json.cpp core/tests/test_bridge.cpp)
target_link_libraries(fixcore_tests PRIVATE fixgen_qf)
target_compile_options(fixcore_tests PRIVATE ${FIXGEN_WARNINGS})
add_test(NAME fixcore_tests COMMAND fixcore_tests)
