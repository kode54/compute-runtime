#
# Copyright (C) 2020-2023 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

set(L0_SRCS_SYSMAN_FREQUENCY_WINDOWS
    ${CMAKE_CURRENT_SOURCE_DIR}/os_frequency_imp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/os_frequency_imp.cpp
)

if(WIN32)
  target_sources(${L0_STATIC_LIB_NAME}
                 PRIVATE
                 ${L0_SRCS_SYSMAN_FREQUENCY_WINDOWS}
  )
endif()

# Make our source files visible to parent
set_property(GLOBAL PROPERTY L0_SRCS_SYSMAN_FREQUENCY_WINDOWS ${L0_SRCS_SYSMAN_FREQUENCY_WINDOWS})
