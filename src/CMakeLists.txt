set(FQFEEDER_LIB_SRCS FastxParser.cpp)

include_directories(
${TOP_LEVEL_DIR}/include
../include)

link_directories(${TOP_LEVEL_DIR}/lib 
$ENV{HOME}/lib)

add_library(fqfeeder ${FQFEEDER_LIB_SRCS})

