cmake_minimum_required (VERSION 3.12)

enable_testing()

project (FQFeeder)

set (CPACK_PACKAGE_VERSION "1.0.0")
set (CPACK_PACKAGE_VERSION_MAJOR "1")
set (CPACK_PACKAGE_VERSION_MAJOR "0")
set (CPACK_PACKAGE_VERSION_MAJOR "0")
set (CPACK_PACKAGE_GENERATOR "TGZ")
set (CPACK_SOURCE_GENERATOR "TGZ")
set (CPACK_PACKAGE_VENDOR "Rob Patro")
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Multithreaded Fasta Parser")
set (CPACK_PACKAGE_NAME
     "${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
set (CPACK_SOURCE_PACKAGE_FILE_NAME
     "${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}-Source")
 
set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

# it's time now — require >= C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# set the required set the required compile flags
set (CMAKE_CXX_FLAGS "-fPIC -pthread -O3 -DHAVE_ANSI_TERM -DHAVE_SSTREAM -Wall -pedantic")

##
# OSX is strange (some might say, stupid in this regard). Deal with it's quirkines here.
##
if ("${CMAKE_SYSTEM_NAME}" MATCHES "Darwin")
    # To allow ourselves to build a dynamic library, we have to tell the compiler
    # that, yes, the symbols will be around at runtime.
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -undefined dynamic_lookup")
    set (LINKER_FLAGS "-all_load")
    # In order to "think different", we also have to use non-standard suffixes
    # for our shared libraries
    set(SHARED_LIB_EXTENSION "dylib")
else()
    # We're in sane linux world
    set (SHARED_LIB_EXTENSION "so")
    set (LINKER_FLAGS "")
endif()

##
# Compiler-specific C++11 activation.
# http://stackoverflow.com/questions/10984442/how-to-detect-c11-support-of-a-compiler-with-cmake
##
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
    set (BOOST_CONFIGURE_TOOLSET "")
    set (BOOST_EXTRA_FLAGS "")
elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif ()

set (TOP_LEVEL_DIR ${CMAKE_CURRENT_SORUCE_DIR})
set (CMAKE_VERBOSE_MAKEFILE true)

set (CPACK_SOURCE_IGNORE_FILES
".git/")

message("CPACK_SOURCE_IGNORE_FILES = ${CPACK_SOURCE_IGNORE_FILES}")

# Recurse into source directory
add_subdirectory (src)

# build a CPack driven installer package
include (CPack)
