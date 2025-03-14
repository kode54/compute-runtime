/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/memory_manager/compression_selector.h"
#include "shared/source/xe_hpg_core/aub_mapper.h"
#include "shared/source/xe_hpg_core/hw_cmds_xe_hpg_core_base.h"

using Family = NEO::XeHpgCoreFamily;

#include "shared/source/helpers/compiler_product_helper.h"
#include "shared/source/helpers/constants.h"
#include "shared/source/helpers/extra_allocation_data_xehp_and_later.inl"
#include "shared/source/helpers/flat_batch_buffer_helper_hw.inl"
#include "shared/source/helpers/gfx_core_helper_base.inl"
#include "shared/source/helpers/gfx_core_helper_bdw_to_dg2.inl"
#include "shared/source/helpers/gfx_core_helper_dg2_and_later.inl"
#include "shared/source/helpers/gfx_core_helper_tgllp_and_later.inl"
#include "shared/source/helpers/gfx_core_helper_xehp_and_later.inl"
#include "shared/source/helpers/logical_state_helper.inl"

namespace NEO {
template <>
const AuxTranslationMode GfxCoreHelperHw<Family>::defaultAuxTranslationMode = AuxTranslationMode::Blit;

template <>
inline bool GfxCoreHelperHw<Family>::isFusedEuDispatchEnabled(const HardwareInfo &hwInfo, bool disableEUFusionForKernel) const {
    auto fusedEuDispatchEnabled = !hwInfo.workaroundTable.flags.waDisableFusedThreadScheduling;
    fusedEuDispatchEnabled &= hwInfo.capabilityTable.fusedEuEnabled;

    if (disableEUFusionForKernel)
        fusedEuDispatchEnabled = false;

    if (DebugManager.flags.CFEFusedEUDispatch.get() != -1) {
        fusedEuDispatchEnabled = (DebugManager.flags.CFEFusedEUDispatch.get() == 0);
    }
    return fusedEuDispatchEnabled;
}

template <>
uint32_t GfxCoreHelperHw<Family>::getMetricsLibraryGenId() const {
    return static_cast<uint32_t>(MetricsLibraryApi::ClientGen::XeHPG);
}

template <>
void GfxCoreHelperHw<Family>::adjustDefaultEngineType(HardwareInfo *pHwInfo, const ProductHelper &productHelper) {
    if (!pHwInfo->featureTable.flags.ftrCCSNode) {
        pHwInfo->capabilityTable.defaultEngineType = aub_stream::ENGINE_RCS;
    }
    if (productHelper.isDefaultEngineTypeAdjustmentRequired(*pHwInfo)) {
        pHwInfo->capabilityTable.defaultEngineType = aub_stream::ENGINE_RCS;
    }
}

template <>
bool GfxCoreHelperHw<Family>::is1MbAlignmentSupported(const HardwareInfo &hwInfo, bool isCompressionEnabled) const {
    return !hwInfo.workaroundTable.flags.waAuxTable64KGranular && isCompressionEnabled;
}

template <>
void GfxCoreHelperHw<Family>::setL1CachePolicy(bool useL1Cache, typename Family::RENDER_SURFACE_STATE *surfaceState, const HardwareInfo *hwInfo) const {
    if (useL1Cache) {
        surfaceState->setL1CachePolicyL1CacheControl(Family::RENDER_SURFACE_STATE::L1_CACHE_POLICY_WB);
        if (DebugManager.flags.OverrideL1CacheControlInSurfaceStateForScratchSpace.get() != -1) {
            surfaceState->setL1CachePolicyL1CacheControl(static_cast<typename Family::RENDER_SURFACE_STATE::L1_CACHE_POLICY>(DebugManager.flags.OverrideL1CacheControlInSurfaceStateForScratchSpace.get()));
        }
    }
}

template <>
bool GfxCoreHelperHw<Family>::isBankOverrideRequired(const HardwareInfo &hwInfo, const ProductHelper &productHelper) const {

    bool forceOverrideMemoryBankIndex = false;

    if (DebugManager.flags.ForceMemoryBankIndexOverride.get() != -1) {
        forceOverrideMemoryBankIndex = static_cast<bool>(DebugManager.flags.ForceMemoryBankIndexOverride.get());
    }
    return forceOverrideMemoryBankIndex;
}

template <>
size_t MemorySynchronizationCommands<Family>::getSizeForSingleAdditionalSynchronizationForDirectSubmission(const RootDeviceEnvironment &rootDeviceEnvironment) {
    return EncodeSemaphore<Family>::getSizeMiSemaphoreWait();
}

template <>
void MemorySynchronizationCommands<Family>::addAdditionalSynchronizationForDirectSubmission(LinearStream &commandStream, uint64_t gpuAddress, bool acquire, const RootDeviceEnvironment &rootDeviceEnvironment) {
    using COMPARE_OPERATION = typename Family::MI_SEMAPHORE_WAIT::COMPARE_OPERATION;

    EncodeSemaphore<Family>::addMiSemaphoreWaitCommand(commandStream, gpuAddress, EncodeSemaphore<Family>::invalidHardwareTag, COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD);
}

template <>
const StackVec<uint32_t, 6> GfxCoreHelperHw<Family>::getThreadsPerEUConfigs() const {
    return {4, 8};
}

template <>
bool GfxCoreHelperHw<Family>::isBufferSizeSuitableForCompression(const size_t size) const {
    if (DebugManager.flags.OverrideBufferSuitableForRenderCompression.get() != -1) {
        return !!DebugManager.flags.OverrideBufferSuitableForRenderCompression.get();
    }

    if (CompressionSelector::allowStatelessCompression()) {
        return true;
    } else {
        return false;
    }
}

template <>
uint32_t GfxCoreHelperHw<Family>::computeSlmValues(const HardwareInfo &hwInfo, uint32_t slmSize) const {
    using SHARED_LOCAL_MEMORY_SIZE = typename Family::INTERFACE_DESCRIPTOR_DATA::SHARED_LOCAL_MEMORY_SIZE;

    auto slmValue = std::max(slmSize, 1024u);
    slmValue = Math::nextPowerOfTwo(slmValue);
    slmValue = Math::getMinLsbSet(slmValue);
    slmValue = slmValue - 9;
    DEBUG_BREAK_IF(slmValue > 7);
    slmValue *= !!slmSize;
    return slmValue;
}

template <>
bool GfxCoreHelperHw<Family>::disableL3CacheForDebug(const HardwareInfo &hwInfo, const ProductHelper &productHelper) const {
    return GfxCoreHelper::isWorkaroundRequired(REVISION_A0, REVISION_B, hwInfo, productHelper);
}

template <>
bool GfxCoreHelperHw<Family>::copyThroughLockedPtrEnabled(const HardwareInfo &hwInfo, const ProductHelper &productHelper) const {
    if (DebugManager.flags.ExperimentalCopyThroughLock.get() != -1) {
        return DebugManager.flags.ExperimentalCopyThroughLock.get() == 1;
    }

    return this->isLocalMemoryEnabled(hwInfo) && !productHelper.isUnlockingLockedPtrNecessary(hwInfo);
}
template <>
uint32_t GfxCoreHelperHw<Family>::calculateAvailableThreadCount(const HardwareInfo &hwInfo, uint32_t grfCount) const {
    if (grfCount > GrfConfig::DefaultGrfNumber) {
        return hwInfo.gtSystemInfo.ThreadCount / 2u;
    }
    return hwInfo.gtSystemInfo.ThreadCount;
}

template class GfxCoreHelperHw<Family>;
template class FlatBatchBufferHelperHw<Family>;
template struct MemorySynchronizationCommands<Family>;
template struct LriHelper<Family>;

template LogicalStateHelper *LogicalStateHelper::create<Family>();
} // namespace NEO
