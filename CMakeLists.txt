cmake_minimum_required(VERSION 2.8)
project(lci)

ENABLE_TESTING()
SET(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake )

SET(PERFORM_MEM_TESTS FALSE CACHE BOOL "Whether or not to enable memory testing")
MARK_AS_ADVANCED(PERFORM_MEM_TESTS)

IF(${PERFORM_MEM_TESTS})
  find_program(VALGRIND valgrind)
  IF(NOT VALGRIND)
    message(FATAL_ERROR 
"
  Error: You've enabled memory testing but we can't find valgrind
  Try one of the following: 
  1: Make sure valgrind is in your PATH 
  2: Install valgrind if you already haven't 
  3: Disable memory testing
")
  ENDIF(NOT VALGRIND)
ENDIF(${PERFORM_MEM_TESTS})

SET(HDRS 
  interpreter.h
  lexer.h
  parser.h
  tokenizer.h
  unicode.h
  error.h
  binding.h
  inet.h
  intern.h
  vm.h
  jit.h
)

SET(SRCS
  interpreter.c
  lexer.c
  main.c
  parser.c
  tokenizer.c
  unicode.c
  error.c
  binding.c
  inet.c
  intern.c
  vm.c
  jit.c
)

add_executable(lci ${SRCS} ${HDRS})

# Link time optimisation lets the machine inline across its own source files.
include(CheckCCompilerFlag)
check_c_compiler_flag("-flto" HAVE_FLTO)
if(HAVE_FLTO)
  target_compile_options(lci PRIVATE $<$<CONFIG:Release>:-flto>)
  target_link_options(lci PRIVATE $<$<CONFIG:Release>:-flto>)
endif()
target_link_libraries(lci m ncurses readline)
add_subdirectory(test)
install(
  TARGETS lci
  RUNTIME DESTINATION bin
)

find_package(Doxygen)
if(DOXYGEN_FOUND)
  add_custom_target(docs
  ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMENT "Generating API documentation with Doxygen" VERBATIM
  )
else(DOXYGEN_FOUND)
  message(WARNING 
    "Couldn't find doxygen. You won't be able to generate documentation now")
endif(DOXYGEN_FOUND)
