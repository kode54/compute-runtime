#
# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

if(UNIX)
  target_sources(${L0_STATIC_LIB_NAME}
                 PRIVATE
                 ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
  )

  if(NEO_ENABLE_i915_PRELIM_DETECTION)
    target_sources(${L0_STATIC_LIB_NAME}
                   PRIVATE
                   ${CMAKE_CURRENT_SOURCE_DIR}/os_power_imp_prelim.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/os_power_imp_prelim.h

    )
  else()
    target_sources(${L0_STATIC_LIB_NAME}
                   PRIVATE
                   ${CMAKE_CURRENT_SOURCE_DIR}/os_power_imp.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/os_power_imp.h
    )
  endif()
endif()
