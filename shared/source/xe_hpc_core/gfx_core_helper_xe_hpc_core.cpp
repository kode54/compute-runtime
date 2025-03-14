/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/xe_hpc_core/aub_mapper.h"
#include "shared/source/xe_hpc_core/hw_cmds_xe_hpc_core_base.h"

using Family = NEO::XeHpcCoreFamily;

#include "shared/source/command_container/command_encoder.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/flat_batch_buffer_helper_hw.inl"
#include "shared/source/helpers/gfx_core_helper_base.inl"
#include "shared/source/helpers/gfx_core_helper_dg2_and_later.inl"
#include "shared/source/helpers/gfx_core_helper_pvc_and_later.inl"
#include "shared/source/helpers/gfx_core_helper_tgllp_and_later.inl"
#include "shared/source/helpers/gfx_core_helper_xehp_and_later.inl"
#include "shared/source/helpers/logical_state_helper.inl"

namespace NEO {

template <>
const AuxTranslationMode GfxCoreHelperHw<Family>::defaultAuxTranslationMode = AuxTranslationMode::Blit;

template <>
uint8_t GfxCoreHelperHw<Family>::getBarriersCountFromHasBarriers(uint8_t hasBarriers) const {
    static constexpr uint8_t possibleBarriersCounts[] = {
        0u,  // 0
        1u,  // 1
        2u,  // 2
        4u,  // 3
        8u,  // 4
        16u, // 5
        24u, // 6
        32u, // 7
    };
    return possibleBarriersCounts[hasBarriers];
}

template <>
const EngineInstancesContainer GfxCoreHelperHw<Family>::getGpgpuEngineInstances(const RootDeviceEnvironment &rootDeviceEnvironment) const {
    auto &hwInfo = *rootDeviceEnvironment.getHardwareInfo();
    auto defaultEngine = getChosenEngineType(hwInfo);
    auto &productHelper = rootDeviceEnvironment.getHelper<ProductHelper>();

    EngineInstancesContainer engines;

    if (hwInfo.featureTable.flags.ftrCCSNode) {
        for (uint32_t i = 0; i < hwInfo.gtSystemInfo.CCSInfo.NumberOfCCSEnabled; i++) {
            engines.push_back({static_cast<aub_stream::EngineType>(i + aub_stream::ENGINE_CCS), EngineUsage::Regular});
            if (productHelper.isCooperativeEngineSupported(hwInfo)) {
                engines.push_back({static_cast<aub_stream::EngineType>(i + aub_stream::ENGINE_CCS), EngineUsage::Cooperative});
            }
        }
    }

    if ((DebugManager.flags.NodeOrdinal.get() == static_cast<int32_t>(aub_stream::EngineType::ENGINE_CCCS)) ||
        hwInfo.featureTable.flags.ftrRcsNode) {
        engines.push_back({aub_stream::ENGINE_CCCS, EngineUsage::Regular});
    }

    engines.push_back({defaultEngine, EngineUsage::LowPriority});
    engines.push_back({defaultEngine, EngineUsage::Internal});

    if (hwInfo.capabilityTable.blitterOperationsSupported) {
        if (hwInfo.featureTable.ftrBcsInfo.test(0)) {
            engines.push_back({aub_stream::EngineType::ENGINE_BCS, EngineUsage::Regular});  // Main copy engine
            engines.push_back({aub_stream::EngineType::ENGINE_BCS, EngineUsage::Internal}); // Internal usage
        }

        for (uint32_t i = 1; i < hwInfo.featureTable.ftrBcsInfo.size(); i++) {
            if (hwInfo.featureTable.ftrBcsInfo.test(i)) {
                auto engineType = static_cast<aub_stream::EngineType>((i - 1) + aub_stream::ENGINE_BCS1); // Link copy engine
                engines.push_back({engineType, EngineUsage::Regular});
                uint32_t internalIndex = 3;
                if (DebugManager.flags.ForceBCSForInternalCopyEngine.get() != -1) {
                    internalIndex = DebugManager.flags.ForceBCSForInternalCopyEngine.get();
                }
                if (i == internalIndex) {
                    engines.push_back({engineType, EngineUsage::Internal}); // BCS3 for internal usage
                }
            }
        }
    }

    return engines;
};

template <>
EngineGroupType GfxCoreHelperHw<Family>::getEngineGroupType(aub_stream::EngineType engineType, EngineUsage engineUsage, const HardwareInfo &hwInfo) const {
    if (engineType == aub_stream::ENGINE_CCCS) {
        return EngineGroupType::RenderCompute;
    }
    if (engineType >= aub_stream::ENGINE_CCS && engineType < (aub_stream::ENGINE_CCS + hwInfo.gtSystemInfo.CCSInfo.NumberOfCCSEnabled)) {
        if (engineUsage == EngineUsage::Cooperative) {
            return EngineGroupType::CooperativeCompute;
        }
        return EngineGroupType::Compute;
    }
    if (engineType == aub_stream::ENGINE_BCS) {
        return EngineGroupType::Copy;
    }
    if (engineType >= aub_stream::ENGINE_BCS1 && engineType < aub_stream::ENGINE_BCS1 + hwInfo.featureTable.ftrBcsInfo.size() - 1) {
        return EngineGroupType::LinkedCopy;
    }
    UNRECOVERABLE_IF(true);
}

template <>
void GfxCoreHelperHw<Family>::adjustDefaultEngineType(HardwareInfo *pHwInfo, const ProductHelper &productHelper) {
    if (!pHwInfo->featureTable.flags.ftrCCSNode) {
        pHwInfo->capabilityTable.defaultEngineType = aub_stream::EngineType::ENGINE_CCCS;
    }
}

template <>
uint32_t GfxCoreHelperHw<Family>::getMetricsLibraryGenId() const {
    return static_cast<uint32_t>(MetricsLibraryApi::ClientGen::XeHPC);
}

template <>
uint32_t GfxCoreHelperHw<Family>::getMinimalSIMDSize() const {
    return 16u;
}

template <>
uint32_t GfxCoreHelperHw<Family>::getMocsIndex(const GmmHelper &gmmHelper, bool l3enabled, bool l1enabled) const {
    if (l3enabled) {
        return gmmHelper.getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1;
    }
    return gmmHelper.getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER_CACHELINE_MISALIGNED) >> 1;
}

template <>
const StackVec<size_t, 3> GfxCoreHelperHw<Family>::getDeviceSubGroupSizes() const {
    return {16, 32};
}

template <>
const StackVec<uint32_t, 6> GfxCoreHelperHw<Family>::getThreadsPerEUConfigs() const {
    return {4, 8};
}

template <>
size_t MemorySynchronizationCommands<Family>::getSizeForSingleAdditionalSynchronization(const RootDeviceEnvironment &rootDeviceEnvironment) {
    const auto &productHelper = rootDeviceEnvironment.getHelper<ProductHelper>();
    auto &hwInfo = *rootDeviceEnvironment.getHardwareInfo();
    auto programGlobalFenceAsMiMemFenceCommandInCommandStream = productHelper.isGlobalFenceInCommandStreamRequired(hwInfo);
    if (DebugManager.flags.ProgramGlobalFenceAsMiMemFenceCommandInCommandStream.get() != -1) {
        programGlobalFenceAsMiMemFenceCommandInCommandStream = !!DebugManager.flags.ProgramGlobalFenceAsMiMemFenceCommandInCommandStream.get();
    }

    if (programGlobalFenceAsMiMemFenceCommandInCommandStream) {
        return sizeof(Family::MI_MEM_FENCE);
    } else {
        return EncodeSemaphore<Family>::getSizeMiSemaphoreWait();
    }
}

template <>
void MemorySynchronizationCommands<Family>::setAdditionalSynchronization(void *&commandsBuffer, uint64_t gpuAddress, bool acquire, const RootDeviceEnvironment &rootDeviceEnvironment) {
    using MI_MEM_FENCE = typename Family::MI_MEM_FENCE;
    using MI_SEMAPHORE_WAIT = typename Family::MI_SEMAPHORE_WAIT;

    const auto &productHelper = rootDeviceEnvironment.getHelper<ProductHelper>();
    auto &hwInfo = *rootDeviceEnvironment.getHardwareInfo();
    auto programGlobalFenceAsMiMemFenceCommandInCommandStream = productHelper.isGlobalFenceInCommandStreamRequired(hwInfo);
    if (DebugManager.flags.ProgramGlobalFenceAsMiMemFenceCommandInCommandStream.get() != -1) {
        programGlobalFenceAsMiMemFenceCommandInCommandStream = !!DebugManager.flags.ProgramGlobalFenceAsMiMemFenceCommandInCommandStream.get();
    }
    if (programGlobalFenceAsMiMemFenceCommandInCommandStream) {
        MI_MEM_FENCE miMemFence = Family::cmdInitMemFence;
        if (acquire) {
            miMemFence.setFenceType(Family::MI_MEM_FENCE::FENCE_TYPE::FENCE_TYPE_ACQUIRE);
        } else {
            miMemFence.setFenceType(Family::MI_MEM_FENCE::FENCE_TYPE::FENCE_TYPE_RELEASE);
        }
        *reinterpret_cast<MI_MEM_FENCE *>(commandsBuffer) = miMemFence;
        commandsBuffer = ptrOffset(commandsBuffer, sizeof(MI_MEM_FENCE));
    } else {
        EncodeSemaphore<Family>::programMiSemaphoreWait(reinterpret_cast<MI_SEMAPHORE_WAIT *>(commandsBuffer),
                                                        gpuAddress,
                                                        EncodeSemaphore<Family>::invalidHardwareTag,
                                                        MI_SEMAPHORE_WAIT::COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD,
                                                        false,
                                                        true);
        commandsBuffer = ptrOffset(commandsBuffer, EncodeSemaphore<Family>::getSizeMiSemaphoreWait());
    }
}

template <>
bool MemorySynchronizationCommands<Family>::isBarrierWaRequired(const RootDeviceEnvironment &rootDeviceEnvironment) {
    if (DebugManager.flags.DisablePipeControlPrecedingPostSyncCommand.get() == 1) {
        return true;
    }
    return false;
}

template <>
size_t MemorySynchronizationCommands<Family>::getSizeForAdditonalSynchronization(const RootDeviceEnvironment &rootDeviceEnvironment) {
    return (DebugManager.flags.DisablePipeControlPrecedingPostSyncCommand.get() == 1 ? 2 : 1) * getSizeForSingleAdditionalSynchronization(rootDeviceEnvironment);
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
void GfxCoreHelperHw<Family>::setExtraAllocationData(AllocationData &allocationData, const AllocationProperties &properties, const RootDeviceEnvironment &rootDeviceEnvironment) const {
    if (properties.allocationType == AllocationType::TIMESTAMP_PACKET_TAG_BUFFER || properties.allocationType == AllocationType::COMMAND_BUFFER) {
        allocationData.flags.useSystemMemory = false;
    }

    bool forceLocalMemoryForDirectSubmission = true;
    switch (DebugManager.flags.DirectSubmissionForceLocalMemoryStorageMode.get()) {
    case 0:
        forceLocalMemoryForDirectSubmission = false;
        break;
    case 1:
        forceLocalMemoryForDirectSubmission = properties.flags.multiOsContextCapable;
        break;
    default:
        break;
    }

    if (forceLocalMemoryForDirectSubmission) {
        if (properties.allocationType == AllocationType::COMMAND_BUFFER ||
            properties.allocationType == AllocationType::RING_BUFFER ||
            properties.allocationType == AllocationType::SEMAPHORE_BUFFER) {
            allocationData.flags.useSystemMemory = false;
            allocationData.flags.requiresCpuAccess = true;
        }
    }

    allocationData.cacheRegion = properties.cacheRegion;

    if (allocationData.flags.requiresCpuAccess && !allocationData.flags.useSystemMemory &&
        (allocationData.storageInfo.getMemoryBanks() > 1)) {

        auto &productHeler = rootDeviceEnvironment.getHelper<ProductHelper>();
        auto &hwInfo = *rootDeviceEnvironment.getHardwareInfo();
        bool applyWa = productHeler.isTilePlacementResourceWaRequired(hwInfo);

        if (applyWa) {
            allocationData.storageInfo.memoryBanks = 1; // force Tile0
        }
    }
}

template <>
uint32_t GfxCoreHelperHw<Family>::getNumCacheRegions() const {
    constexpr uint32_t numSharedCacheRegions = 1;
    constexpr uint32_t numReservedCacheRegions = 2;
    constexpr uint32_t numTotalCacheRegions = numSharedCacheRegions + numReservedCacheRegions;
    return numTotalCacheRegions;
}

template <>
uint32_t GfxCoreHelperHw<Family>::alignSlmSize(uint32_t slmSize) const {
    const uint32_t alignedSlmSizes[] = {
        0u,
        1u * KB,
        2u * KB,
        4u * KB,
        8u * KB,
        16u * KB,
        24u * KB,
        32u * KB,
        48u * KB,
        64u * KB,
        96u * KB,
        128u * KB,
    };

    for (auto &alignedSlmSize : alignedSlmSizes) {
        if (slmSize <= alignedSlmSize) {
            return alignedSlmSize;
        }
    }

    UNRECOVERABLE_IF(true);
    return 0;
}

template <>
uint32_t GfxCoreHelperHw<Family>::computeSlmValues(const HardwareInfo &hwInfo, uint32_t slmSize) const {
    using SHARED_LOCAL_MEMORY_SIZE = typename Family::INTERFACE_DESCRIPTOR_DATA::SHARED_LOCAL_MEMORY_SIZE;
    if (slmSize == 0u) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_0K;
    }

    UNRECOVERABLE_IF(slmSize > 128u * KB);

    if (slmSize > 96u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_128K;
    }
    if (slmSize > 64u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_96K;
    }
    if (slmSize > 48u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_64K;
    }
    if (slmSize > 32u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_48K;
    }
    if (slmSize > 24u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_32K;
    }
    if (slmSize > 16u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_24K;
    }
    if (slmSize > 8u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_16K;
    }
    if (slmSize > 4u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_8K;
    }
    if (slmSize > 2u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_4K;
    }
    if (slmSize > 1u * KB) {
        return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_2K;
    }
    return SHARED_LOCAL_MEMORY_SIZE::SHARED_LOCAL_MEMORY_SIZE_ENCODES_1K;
}

template <>
int32_t GfxCoreHelperHw<Family>::getDefaultThreadArbitrationPolicy() const {
    return ThreadArbitrationPolicy::RoundRobinAfterDependency;
}

template <>
bool GfxCoreHelperHw<Family>::isSubDeviceEngineSupported(const RootDeviceEnvironment &rootDeviceEnvironment, const DeviceBitfield &deviceBitfield, aub_stream::EngineType engineType) const {
    constexpr uint64_t tile1Bitfield = 0b10;

    bool affectedEngine = (deviceBitfield.to_ulong() == tile1Bitfield) &&
                          (aub_stream::ENGINE_BCS == engineType ||
                           aub_stream::ENGINE_BCS1 == engineType ||
                           aub_stream::ENGINE_BCS3 == engineType);

    auto &productHelper = rootDeviceEnvironment.template getHelper<ProductHelper>();
    auto &hwInfo = *rootDeviceEnvironment.getHardwareInfo();
    return affectedEngine ? !productHelper.isBcsReportWaRequired(hwInfo) : true;
}

template <>
uint32_t GfxCoreHelperHw<Family>::getComputeUnitsUsedForScratch(const RootDeviceEnvironment &rootDeviceEnvironment) const {
    if (DebugManager.flags.OverrideNumComputeUnitsForScratch.get() != -1) {
        return static_cast<uint32_t>(DebugManager.flags.OverrideNumComputeUnitsForScratch.get());
    }

    auto &helper = rootDeviceEnvironment.getHelper<ProductHelper>();
    auto hwInfo = rootDeviceEnvironment.getHardwareInfo();
    uint32_t threadEuRatio = helper.getThreadEuRatioForScratch(*hwInfo);

    return hwInfo->gtSystemInfo.MaxSubSlicesSupported * hwInfo->gtSystemInfo.MaxEuPerSubSlice * threadEuRatio;
}

template <>
bool GfxCoreHelperHw<Family>::isRevisionSpecificBinaryBuiltinRequired() const {
    return true;
}

template <>
size_t GfxCoreHelperHw<Family>::getSipKernelMaxDbgSurfaceSize(const HardwareInfo &hwInfo) const {
    return 40 * MB;
}

template <>
bool GfxCoreHelperHw<Family>::isTimestampWaitSupportedForQueues() const {
    return true;
}

template <>
uint64_t GfxCoreHelperHw<Family>::getPatIndex(CacheRegion cacheRegion, CachePolicy cachePolicy) const {
    /*
    PAT Index  CLOS   MemType
    SHARED
    0          0      UC (00)
    1          0      WC (01)
    2          0      WT (10)
    3          0      WB (11)
    RESERVED 1
    4          1      WT (10)
    5          1      WB (11)
    RESERVED 2
    6          2      WT (10)
    7          2      WB (11)
    */

    if ((DebugManager.flags.ForceAllResourcesUncached.get() == true)) {
        cacheRegion = CacheRegion::Default;
        cachePolicy = CachePolicy::Uncached;
    }

    UNRECOVERABLE_IF((cacheRegion > CacheRegion::Default) && (cachePolicy < CachePolicy::WriteThrough));
    return (static_cast<uint32_t>(cachePolicy) + (static_cast<uint16_t>(cacheRegion) * 2));
}

template <>
bool GfxCoreHelperHw<Family>::copyThroughLockedPtrEnabled(const HardwareInfo &hwInfo, const ProductHelper &productHelper) const {
    if (DebugManager.flags.ExperimentalCopyThroughLock.get() != -1) {
        return DebugManager.flags.ExperimentalCopyThroughLock.get() == 1;
    }
    return true;
}

template <>
uint32_t GfxCoreHelperHw<Family>::getAmountOfAllocationsToFill() const {
    if (DebugManager.flags.SetAmountOfReusableAllocations.get() != -1) {
        return DebugManager.flags.SetAmountOfReusableAllocations.get();
    }
    return 1u;
}

template <>
bool GfxCoreHelperHw<Family>::isRelaxedOrderingSupported() const {
    return true;
}

} // namespace NEO

namespace NEO {
template class GfxCoreHelperHw<Family>;
template class FlatBatchBufferHelperHw<Family>;
template struct MemorySynchronizationCommands<Family>;
template struct LriHelper<Family>;

template LogicalStateHelper *LogicalStateHelper::create<Family>();
} // namespace NEO
