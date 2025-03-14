#
# Copyright (C) 2019-2023 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

target_sources(neo_shared_tests PRIVATE
               ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
               ${CMAKE_CURRENT_SOURCE_DIR}/compiler_cache_tests.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/compiler_interface_tests.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/compiler_options_tests.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/external_functions_tests.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/intermediate_representations_tests.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/linker_tests.cpp
)

if(WIN32)
  target_sources(neo_shared_tests PRIVATE
                 ${CMAKE_CURRENT_SOURCE_DIR}/windows/compiler_cache_tests_windows.cpp
  )
else()
  target_sources(neo_shared_tests PRIVATE
                 ${CMAKE_CURRENT_SOURCE_DIR}/linux/compiler_cache_tests_linux.cpp
  )
endif()