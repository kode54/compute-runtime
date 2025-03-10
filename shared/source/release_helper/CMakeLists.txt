#
# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

set(NEO_CORE_RELEASE_HELPER
    ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/release_helper.h
    ${CMAKE_CURRENT_SOURCE_DIR}/release_helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/release_helper_base.inl
    ${CMAKE_CURRENT_SOURCE_DIR}/definitions${BRANCH_DIR_SUFFIX}release_definitions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/release_helper.cpp
)

if(SUPPORT_XE_HPG_CORE)
  list(APPEND NEO_CORE_RELEASE_HELPER
       ${CMAKE_CURRENT_SOURCE_DIR}/release_helper_common_xe_lpg.inl
       ${CMAKE_CURRENT_SOURCE_DIR}/release_helper_1255.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/release_helper_1256.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/release_helper_1257.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/release_helper_1270.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/release_helper_1271.cpp
  )
  set_property(GLOBAL APPEND PROPERTY NEO_CORE_SRCS_LINK ${CMAKE_CURRENT_SOURCE_DIR}${BRANCH_DIR_SUFFIX}enable_release_helper_xe.cpp)
endif()

set_property(GLOBAL APPEND PROPERTY NEO_CORE_RELEASE_HELPER ${NEO_CORE_RELEASE_HELPER})

add_subdirectories()
