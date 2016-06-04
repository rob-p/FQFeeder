set(TEST_SRCS TestReader.cpp)

set(FQFEEDER_LIB_SRCS FastxParser.cpp)

add_library(fqfeeder ${FQFEEDER_LIB_SRCS})

include_directories(
${TOP_LEVEL_DIR}/include
../include)

link_directories(${TOP_LEVEL_DIR}/lib 
$ENV{HOME}/lib)

add_executable(test_parser ${TEST_SRCS}) 
add_executable(test_kseq TestKseq.cpp)

target_link_libraries(test_parser pthread fqfeeder z)
target_link_libraries(test_kseq pthread z)
