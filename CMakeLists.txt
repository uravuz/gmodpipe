cmake_minimum_required(VERSION 3.19)
project(gmodpipe CXX)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
    gmod_headers
    GIT_REPOSITORY https://github.com/Facepunch/gmod-module-base.git
    GIT_TAG        development
    SOURCE_SUBDIR  include
)
FetchContent_MakeAvailable(gmod_headers)

add_library(gmodpipe SHARED)
target_sources(gmodpipe PRIVATE gmodpipe.cpp)

target_link_libraries(gmodpipe gmod-module-base)

SET_TARGET_PROPERTIES(gmodpipe PROPERTIES PREFIX "gmcl_")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        SET_TARGET_PROPERTIES(gmodpipe PROPERTIES SUFFIX "_osx.dll")
    else()
        SET_TARGET_PROPERTIES(gmodpipe PROPERTIES SUFFIX "_osx64.dll")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        SET_TARGET_PROPERTIES(gmodpipe PROPERTIES SUFFIX "_linux.dll")
    else()
        SET_TARGET_PROPERTIES(gmodpipe PROPERTIES SUFFIX "_linux64.dll")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        SET_TARGET_PROPERTIES(gmodpipe PROPERTIES SUFFIX "_win32.dll")
    else()
        SET_TARGET_PROPERTIES(gmodpipe PROPERTIES SUFFIX "_win64.dll")
    endif()
endif()
