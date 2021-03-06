# ----------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Andreas-Joachim Peters - CERN
# ----------------------------------------------------------------------

# ************************************************************************
# * EOS - the CERN Disk Storage System                                   *
# * Copyright (C) 2011 CERN/Switzerland                                  *
# *                                                                      *
# * This program is free software: you can redistribute it and/or modify *
# * it under the terms of the GNU General Public License as published by *
# * the Free Software Foundation, either version 3 of the License, or    *
# * (at your option) any later version.                                  *
# *                                                                      *
# * This program is distributed in the hope that it will be useful,      *
# * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
# * GNU General Public License for more details.                         *
# *                                                                      *
# * You should have received a copy of the GNU General Public License    *
# * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
# ************************************************************************

#-------------------------------------------------------------------------------
# Main section
#-------------------------------------------------------------------------------
cmake_minimum_required (VERSION 2.6)
project (eos)

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

if("${CMAKE_BUILD_TYPE}" STREQUAL "")
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Set "/usr" installation prefix if not specified otherwise
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Default install prefix: /usr" FORCE)
endif()

# Include some generic functions and GNU path settings
include(EosUtils)
include(GNUInstallDirs)

# Force /usr/ installation prefix
set(CMAKE_INSTALL_SYSCONFDIR /etc)
set(CMAKE_INSTALL_SBINDIR /sbin)
set(CMAKE_INSTALL_FULL_SBINDIR /usr/sbin)

EOS_GetVersion("${VERSION_MAJOR}" "${VERSION_MINOR}" "${VERSION_PATCH}" "${RELEASE}")
EOS_DefineOperatingSystem()

#-------------------------------------------------------------------------------
# Avoid CMake warnings
#-------------------------------------------------------------------------------
if(NOT (CMAKE_VERSION VERSION_LESS "2.8.12"))
  cmake_policy(SET CMP0022 NEW)
endif()

#-------------------------------------------------------------------------------
# Platform Specific
#-------------------------------------------------------------------------------
if(MacOSX)
  # On MAC we don't link static objects at all
  set(FUSE_LIBRARY /usr/local/lib/libosxfuse_i64.dylib)
  set(CMAKE_MACOSX_RPATH ON)
  set(CMAKE_SKIP_BUILD_RPATH FALSE)
  set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
  set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
  set(LIBRARY_PATH_PREFIX lib)
  List(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_PREFIX}/lib" isSystemDir)

  if("${isSystemDir}" STREQUAL "-1")
    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
  endif()
else()
  # On Linux we use static objects
  set(LIB_INSTALL_DIR lib64)
  set(RT_LIBS rt)
endif()

include_directories(/usr/include/xrootd/ /usr/include/)
link_directories(${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/)

#-------------------------------------------------------------------------------
# Search for dependencies
#-------------------------------------------------------------------------------
set(PACKAGEONLY CACHE BOOL false)
set(CLIENT CACHE BOOL false)

if (NOT PACKAGEONLY)
  find_package(PythonSitePkg REQUIRED)
  find_package(XRootD REQUIRED)
  find_package(fuse REQUIRED)
  find_package(Threads REQUIRED)
  find_package(z REQUIRED)
  find_package(readline REQUIRED)
  find_package(CURL REQUIRED)
  find_package(uuid REQUIRED)
  find_package(ProtocolBuffers REQUIRED)
  find_package(openssl REQUIRED)
  find_package(ncurses REQUIRED)
  find_package(leveldb REQUIRED)
  find_package(ZMQ REQUIRED)
  find_package(krb5 REQUIRED)
  find_package(Sphinx)

  if (Linux)
    find_package(glibc REQUIRED)
  endif()

  # The server build also requires
  if (NOT CLIENT)
    find_package(SparseHash REQUIRED)
    find_package(rt REQUIRED)
    find_package(ldap REQUIRED)
    find_package(xfs REQUIRED)
    find_package(attr REQUIRED)
    find_package(CPPUnit)
    find_package(microhttpd)
    find_package(httpd)
  endif()
else()
  message(STATUS "Runing CMake in package only mode")
  # Fake function for building the SRPMS in build system
  function(PROTOBUF_GENERATE_CPP SRCS HDRS)
    return()
  endfunction()
endif()

#-------------------------------------------------------------------------------
# Check for Gcc >=4.4
#-------------------------------------------------------------------------------
if(CMAKE_COMPILER_IS_GNUCXX)
  exec_program(${CMAKE_CXX_COMPILER} ARGS --version
      OUTPUT_VARIABLE _compiler_output)
  string(REGEX REPLACE ".* ([0-9]\\.[0-9]\\.[0-9]) .*" "\\1"
         gcc_compiler_version ${_compiler_output})
  message(STATUS "C++ compiler version: ${gcc_compiler_version} [${CMAKE_CXX_COMPILER}]")

  if(gcc_compiler_version MATCHES "4\\.[0-3]\\.[0-9]")
    message(FATAL_ERROR "Error: you need atleast gcc 4.4.x to compile EOS!")
  endif()
endif()

#-------------------------------------------------------------------------------
# Check for out of source build
#-------------------------------------------------------------------------------
EOS_CheckOutOfSourceBuild()

#-------------------------------------------------------------------------------
# Get daemon uid and gid
#-------------------------------------------------------------------------------
set( DAEMONUID "" )
set( DAEMONGID "" )
EOS_GetUidGid("daemon" "DAEMONUID" "DAEMONGID")
if (CLIENT)
  if (MacOSX)
    set(CMAKE_CXX_FLAGS "-O2 -g -Wall -Wno-deprecated-declarations -Wno-c++11-narrowing -msse4.2 -std=c++11 -stdlib=libc++ -DHAVE_ATOMICS=1 -DVERSION=\\\"${VERSION}\\\" -DRELEASE=\\\"${RELEASE}\\\" -DCLIENT_ONLY=1 -DDAEMONUID=${DAEMONUID} -DDAEMONGID=${DAEMONGID}" )
  else ()
  set(CMAKE_CXX_FLAGS "-O2 -g -Wall -Wno-deprecated-declarations -msse4.2 -std=gnu++0x -DHAVE_ATOMICS=1 -DVERSION=\\\"${VERSION}\\\" -DRELEASE=\\\"${RELEASE}\\\" -DCLIENT_ONLY=1 -DDAEMONUID=${DAEMONUID} -DDAEMONGID=${DAEMONGID}" )
  endif()
else (CLIENT)
  set(CMAKE_CXX_FLAGS "-O2 -g -Wall -Wno-deprecated-declarations -msse4.2 -std=gnu++0x -DHAVE_ATOMICS=1 -DVERSION=\\\"${VERSION}\\\" -DRELEASE=\\\"${RELEASE}\\\" -DDAEMONUID=${DAEMONUID} -DDAEMONGID=${DAEMONGID}" )
endif (CLIENT)

if(NOT MICROHTTPD_FOUND)
 message("  Warning: MicroHttpd not found, no httpd access available")
else()
 set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DEOS_MICRO_HTTPD=1")
endif()

#-------------------------------------------------------------------------------
# Generate documentation
#-------------------------------------------------------------------------------
if(PYTHONINTERP_FOUND AND SPHINX_FOUND)
  add_custom_target(doc
    COMMAND ${PYTHON_EXECUTABLE} generate_docs.py
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/doc"
    COMMENT "Build HTML documentation with Sphinx ...")
endif()

#-------------------------------------------------------------------------------
# Install targets
#-------------------------------------------------------------------------------
if (CLIENT)
  add_subdirectory (authz)
  add_subdirectory (fst)
  add_subdirectory (fuse)
  add_subdirectory (console)
  add_subdirectory (man)
  add_subdirectory (mq)
  add_subdirectory (common)
else()
  add_subdirectory (mq)
  add_subdirectory (common)
  add_subdirectory (authz)
  add_subdirectory (fst)
  add_subdirectory (mgm)
  add_subdirectory (console)
  add_subdirectory (namespace)
  add_subdirectory (etc)
  add_subdirectory (sync)
  add_subdirectory (fuse)
  add_subdirectory (srm)
  add_subdirectory (var)
  add_subdirectory (sbin)
  add_subdirectory (man)
  add_subdirectory (test)
  add_subdirectory (utils)
  add_subdirectory (archive)
  add_subdirectory(auth_plugin)
endif()

#-------------------------------------------------------------------------------
# Uninstall target
#-------------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cmake/cmake_uninstall.cmake"
  IMMEDIATE @ONLY)

add_custom_target(
   uninstall
  "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/cmake_uninstall.cmake")

#-------------------------------------------------------------------------------
# Packaging
#-------------------------------------------------------------------------------
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_PACKAGE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${VERSION_PATCH}")
set(CPACK_PACKAGE_RELEASE "${RELEASE}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}")
set(CPACK_SOURCE_IGNORE_FILES "xrootd-dsi/;/nginx/;/dsi/;/microhttpd/;/build/;/.deps/;/.bzr/;~$;'.'o$;/CMakeFiles/;Makefile;/_CPack_Packages/;.tar.gz$;.tar.bz2;CMakeCache.txt;ApMon;build;install_manifest.txt;/lib/;/.git/;${CPACK_SOURCE_IGNORE_FILES};")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/config_spec.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/cmake/config_spec.cmake" @ONLY IMMEDIATE)

add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/eos.spec"
                   COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/config_spec.cmake"
                   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/cmake/config_spec.cmake"
                           "${CMAKE_CURRENT_SOURCE_DIR}/eos.spec.in")

add_custom_target(dist COMMAND ${CMAKE_MAKE_PROGRAM} package_source
                  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/eos.spec")

add_custom_command(TARGET dist POST_BUILD
                   COMMAND rm ARGS -rf "${CMAKE_CURRENT_SOURCE_DIR}/eos.spec"
                   COMMENT "clean generated spec file")

include(CPack)

add_custom_target(gitdist
    COMMAND git archive --prefix=${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}/ HEAD
    | gzip > ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}.tar.gz
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

#-------------------------------------------------------------------------------
# Source and binary rpms
#-------------------------------------------------------------------------------
set (RPM_DEFINE --define "_source_filedigest_algorithm md5" --define "_binary_filedigest_algorithm md5")

add_custom_target(
  srpm
  COMMAND rpmbuild ${RPM_DEFINE} -ts ${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}.tar.gz)


add_custom_target(
  rpm
  COMMAND rpmbuild -tb ${CMAKE_PROJECT_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}.tar.gz)

add_dependencies(rpm dist)
add_dependencies(srpm dist)

#-------------------------------------------------------------------------------
# Print project summary
#-------------------------------------------------------------------------------
include(EosSummary)
