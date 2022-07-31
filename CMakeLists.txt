# DSD Unpack - https://github.com/michaelburton/dsdunpack
#
# Copyright (c) 2014 by Michael Burton.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

cmake_minimum_required(VERSION 2.6)
project(dsdunpack C)

# Macros we'll need
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckTypeSize)
include(FindThreads)

# Include directory paths
include_directories(${dsdunpack_SOURCE_DIR})

include_directories("./src/lib/libdstdec")

# Extra flags for GCC
if (CMAKE_COMPILER_IS_GNUCC)
  add_definitions(
      -pipe
      -Wall -Wextra -Wcast-align -Wpointer-arith
      -Wno-unused-parameter
      -mno-ms-bitfields # breaks __attribute__((packed)) on Windows
      -msse2)
endif (CMAKE_COMPILER_IS_GNUCC)

if (MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MDd")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MD /GL /arch:SSE2 /D PTW32_STATIC_LIB")
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
    add_definitions(-D_CRT_NONSTDC_NO_WARNINGS)
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} pthreadVC2.lib")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} pthreadVC2_static.lib")
    
    # MSVC doesn't come with a getopt implementation
    include_directories("./src/lib/getopt")
    file(GLOB getopt_headers ./src/lib/getopt/*.h)
    file(GLOB getopt_sources ./src/lib/getopt/*.c)
    source_group(getopt FILES ${getopt_headers} ${getopt_sources})
else()
    add_definitions(-D_FILE_OFFSET_BITS=64)
    set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIRARIES} -lpthread")
endif()

file(GLOB libdstdec_headers ./src/lib/libdstdec/*.h)
file(GLOB libdstdec_sources ./src/lib/libdstdec/*.c)
source_group(libdstdec FILES ${libdstdec_headers} ${libdstdec_sources})

file(GLOB main_headers ./src/*.h)
file(GLOB main_sources ./src/*.c)
source_group(main FILES ${main_headers} ${main_sources})

add_executable(dsdunpack 
    ${main_headers} ${main_sources}
    ${libdstdec_headers} ${libdstdec_sources}
	${getopt_headers} ${getopt_sources}
    )

set(CMAKE_BUILD_TYPE Release)
