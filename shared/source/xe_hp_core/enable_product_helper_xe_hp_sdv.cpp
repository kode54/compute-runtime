/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/product_helper.h"
#include "shared/source/os_interface/product_helper_hw.h"
#include "shared/source/xe_hp_core/hw_cmds.h"

namespace NEO {

static EnableProductHelper<IGFX_XE_HP_SDV> enableXEHP;

} // namespace NEO
