/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/gen8/hw_cmds_bdw.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/os_interface/product_helper.inl"
#include "shared/source/os_interface/product_helper_bdw_and_later.inl"

#include "platforms.h"

constexpr static auto gfxProduct = IGFX_BROADWELL;

#include "shared/source/gen8/bdw/os_agnostic_product_helper_bdw.inl"

namespace NEO {

template <>
int ProductHelperHw<gfxProduct>::configureHardwareCustom(HardwareInfo *hwInfo, OSInterface *osIface) const {
    if (nullptr == osIface) {
        return 0;
    }
    GT_SYSTEM_INFO *gtSystemInfo = &hwInfo->gtSystemInfo;

    // There is no interface to read total slice count from drm/i915, so we
    // derive this from the number of EUs and subslices.
    // otherwise there is one slice.
    if (gtSystemInfo->SubSliceCount > 3) {
        gtSystemInfo->SliceCount = 2;
    } else {
        gtSystemInfo->SliceCount = 1;
    }

    if (hwInfo->platform.usDeviceID == IBDW_GT3_HALO_MOBL_DEVICE_F0_ID ||
        hwInfo->platform.usDeviceID == IBDW_GT3_SERV_DEVICE_F0_ID) {
        gtSystemInfo->EdramSizeInKb = 128 * 1024;
    }
    return 0;
}

template class ProductHelperHw<gfxProduct>;

} // namespace NEO
