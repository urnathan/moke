# Moke	-*- mode:cmake -*-
# Copyright (C) 2021 Nathan Sidwell, nathan@acm.org
# License: Affero GPL v3.0

# Don't actually know the minimum version this'll work with, 3.18 is what I have
cmake_minimum_required (VERSION 3.18)

list (PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if (NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  message (FATAL_ERROR "Not intended as a subcomponent")
endif ()
project (Moke VERSION 1.0 LANGUAGES C)
set (PROJECT_URL "https://github.com/urnathan/moke")

include (CheckLibraryExists)
include (CheckSymbolExists)

message (NOTICE "Build type:${CMAKE_BUILD_TYPE}")

# -g3 means we get macros
set (CMAKE_CXX_FLAGS_DEBUG -g3)
set (CMAKE_CXX_FLAGS_RELWITHDEBINFO
  "${CMAKE_CXX_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS_DEBUG}")

add_compile_options ("-xc++;-std=gnu++17")
add_compile_options ("-fdiagnostics-color=always;-fno-diagnostics-show-option")
add_compile_options ("-W;-Wall")
add_compile_options ("-fno-exceptions;-fno-rtti")

configure_file (gdbinit.in .gdbinit ESCAPE_QUOTES)

configure_file (mokecfg.h.in mokecfg.h ESCAPE_QUOTES)
include_directories ("${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

set (PERMISSIONS
  OWNER_WRITE OWNER_READ OWNER_EXECUTE
  GROUP_READ GROUP_EXECUTE
  WORLD_READ WORLD_EXECUTE
  SETUID)

add_executable (moke moke.c)

install (TARGETS moke DESTINATION bin PERMISSIONS ${PERMISSIONS})
