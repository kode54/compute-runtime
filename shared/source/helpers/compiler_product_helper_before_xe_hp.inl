/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/helpers/compiler_product_helper.h"

namespace NEO {
template <PRODUCT_FAMILY gfxProduct>
uint32_t CompilerProductHelperHw<gfxProduct>::getNumThreadsPerEu() const {
    return 7u;
}

template <PRODUCT_FAMILY gfxProduct>
bool CompilerProductHelperHw<gfxProduct>::isMatrixMultiplyAccumulateSupported(const ReleaseHelper *releaseHelper) const {
    return false;
}

template <PRODUCT_FAMILY gfxProduct>
bool CompilerProductHelperHw<gfxProduct>::isBFloat16ConversionSupported(const HardwareInfo &hwInfo) const {
    return false;
}

template <PRODUCT_FAMILY gfxProduct>
bool CompilerProductHelperHw<gfxProduct>::isDotAccumulateSupported() const {
    return false;
}

template <PRODUCT_FAMILY gfxProduct>
bool CompilerProductHelperHw<gfxProduct>::isCreateBufferWithPropertiesSupported() const {
    return false;
}

} // namespace NEO
