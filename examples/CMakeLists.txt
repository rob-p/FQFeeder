set(TEST_SRCS TestReader.cpp)

include_directories(
${TOP_LEVEL_DIR}/include
${TOP_LEVEL_DIR}/external/include
../include)

link_directories(${TOP_LEVEL_DIR}/lib 
$ENV{HOME}/lib)

add_executable(test_parser ${TEST_SRCS}) 
add_executable(test_kseq TestKseq.cpp)
add_executable(test_parser_triplet TestTriplet.cpp)

target_link_libraries(test_parser pthread fqfeeder zlib-ng::zlib-ng)
target_link_libraries(test_kseq pthread zlib-ng::zlib-ng)
