/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "opencl/source/mem_obj/image.h"
#include "opencl/source/sharings/unified/unified_sharing_types.h"

namespace NEO {

bool Image::validateHandleType(MemoryProperties &memoryProperties, UnifiedSharingMemoryDescription &extMem) {
    if (memoryProperties.handleType == static_cast<uint64_t>(UnifiedSharingHandleType::Win32Nt)) {
        extMem.type = UnifiedSharingHandleType::Win32Nt;
        return true;
    }
    return false;
}

} // namespace NEO
