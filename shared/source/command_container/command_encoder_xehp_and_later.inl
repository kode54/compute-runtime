/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/command_container/command_encoder.h"
#include "shared/source/command_container/implicit_scaling.h"
#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/command_stream/linear_stream.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/command_stream/stream_properties.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/gmm_helper/client_context/gmm_client_context.h"
#include "shared/source/helpers/basic_math.h"
#include "shared/source/helpers/cache_policy.h"
#include "shared/source/helpers/constants.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/hw_walk_order.h"
#include "shared/source/helpers/pause_on_gpu_properties.h"
#include "shared/source/helpers/pipe_control_args.h"
#include "shared/source/helpers/ray_tracing_helper.h"
#include "shared/source/helpers/simd_helper.h"
#include "shared/source/helpers/state_base_address.h"
#include "shared/source/kernel/dispatch_kernel_encoder_interface.h"
#include "shared/source/kernel/implicit_args.h"
#include "shared/source/kernel/kernel_descriptor.h"
#include "shared/source/os_interface/product_helper.h"

#include <algorithm>

namespace NEO {
constexpr size_t TimestampDestinationAddressAlignment = 16;

template <typename Family>
void EncodeDispatchKernel<Family>::setGrfInfo(INTERFACE_DESCRIPTOR_DATA *pInterfaceDescriptor, uint32_t numGrf,
                                              const size_t &sizeCrossThreadData, const size_t &sizePerThreadData,
                                              const HardwareInfo &hwInfo) {
}

template <typename Family>
void EncodeDispatchKernel<Family>::encode(CommandContainer &container, EncodeDispatchKernelArgs &args, LogicalStateHelper *logicalStateHelper) {
    using SHARED_LOCAL_MEMORY_SIZE = typename Family::INTERFACE_DESCRIPTOR_DATA::SHARED_LOCAL_MEMORY_SIZE;
    using STATE_BASE_ADDRESS = typename Family::STATE_BASE_ADDRESS;
    using INLINE_DATA = typename Family::INLINE_DATA;

    const HardwareInfo &hwInfo = args.device->getHardwareInfo();
    auto &rootDeviceEnvironment = args.device->getRootDeviceEnvironment();

    const auto &kernelDescriptor = args.dispatchInterface->getKernelDescriptor();
    auto sizeCrossThreadData = args.dispatchInterface->getCrossThreadDataSize();
    auto sizePerThreadData = args.dispatchInterface->getPerThreadDataSize();
    auto sizePerThreadDataForWholeGroup = args.dispatchInterface->getPerThreadDataSizeForWholeThreadGroup();
    auto pImplicitArgs = args.dispatchInterface->getImplicitArgs();

    LinearStream *listCmdBufferStream = container.getCommandStream();

    auto threadDims = static_cast<const uint32_t *>(args.threadGroupDimensions);
    const Vec3<size_t> threadStartVec{0, 0, 0};
    Vec3<size_t> threadDimsVec{0, 0, 0};
    if (!args.isIndirect) {
        threadDimsVec = {threadDims[0], threadDims[1], threadDims[2]};
    }

    bool systolicModeRequired = kernelDescriptor.kernelAttributes.flags.usesSystolicPipelineSelectMode;
    if (container.systolicModeSupportRef() && (container.lastPipelineSelectModeRequiredRef() != systolicModeRequired)) {
        container.lastPipelineSelectModeRequiredRef() = systolicModeRequired;
        EncodeComputeMode<Family>::adjustPipelineSelect(container, kernelDescriptor);
    }

    WALKER_TYPE walkerCmd = Family::cmdInitGpgpuWalker;
    auto &idd = walkerCmd.getInterfaceDescriptor();

    EncodeDispatchKernel<Family>::setGrfInfo(&idd, kernelDescriptor.kernelAttributes.numGrfRequired, sizeCrossThreadData,
                                             sizePerThreadData, hwInfo);
    auto &productHelper = args.device->getProductHelper();
    productHelper.updateIddCommand(&idd, kernelDescriptor.kernelAttributes.numGrfRequired,
                                   kernelDescriptor.kernelAttributes.threadArbitrationPolicy);

    bool localIdsGenerationByRuntime = args.dispatchInterface->requiresGenerationOfLocalIdsByRuntime();
    auto requiredWorkgroupOrder = args.dispatchInterface->getRequiredWorkgroupOrder();
    bool inlineDataProgramming = EncodeDispatchKernel<Family>::inlineDataProgrammingRequired(kernelDescriptor);
    {
        auto alloc = args.dispatchInterface->getIsaAllocation();
        UNRECOVERABLE_IF(nullptr == alloc);
        auto offset = alloc->getGpuAddressToPatch();
        if (!localIdsGenerationByRuntime) {
            offset += kernelDescriptor.entryPoints.skipPerThreadDataLoad;
        }
        idd.setKernelStartPointer(offset);
    }

    auto threadsPerThreadGroup = args.dispatchInterface->getNumThreadsPerThreadGroup();
    idd.setNumberOfThreadsInGpgpuThreadGroup(threadsPerThreadGroup);
    idd.setDenormMode(INTERFACE_DESCRIPTOR_DATA::DENORM_MODE_SETBYKERNEL);

    EncodeDispatchKernel<Family>::programBarrierEnable(idd,
                                                       kernelDescriptor.kernelAttributes.barrierCount,
                                                       hwInfo);

    auto &gfxCoreHelper = args.device->getGfxCoreHelper();
    auto slmSize = static_cast<SHARED_LOCAL_MEMORY_SIZE>(
        gfxCoreHelper.computeSlmValues(hwInfo, args.dispatchInterface->getSlmTotalSize()));

    if (DebugManager.flags.OverrideSlmAllocationSize.get() != -1) {
        slmSize = static_cast<SHARED_LOCAL_MEMORY_SIZE>(DebugManager.flags.OverrideSlmAllocationSize.get());
    }
    idd.setSharedLocalMemorySize(slmSize);

    auto bindingTableStateCount = kernelDescriptor.payloadMappings.bindingTable.numEntries;
    uint32_t bindingTablePointer = 0u;
    if ((kernelDescriptor.kernelAttributes.bufferAddressingMode == KernelDescriptor::BindfulAndStateless) ||
        kernelDescriptor.kernelAttributes.flags.usesImages) {
        container.prepareBindfulSsh();
        if (bindingTableStateCount > 0u) {
            auto ssh = args.surfaceStateHeap;
            if (ssh == nullptr) {
                ssh = container.getHeapWithRequiredSizeAndAlignment(HeapType::SURFACE_STATE, args.dispatchInterface->getSurfaceStateHeapDataSize(), BINDING_TABLE_STATE::SURFACESTATEPOINTER_ALIGN_SIZE);
            }
            bindingTablePointer = static_cast<uint32_t>(EncodeSurfaceState<Family>::pushBindingTableAndSurfaceStates(
                *ssh,
                args.dispatchInterface->getSurfaceStateHeapData(),
                args.dispatchInterface->getSurfaceStateHeapDataSize(), bindingTableStateCount,
                kernelDescriptor.payloadMappings.bindingTable.tableOffset));
        }
    }
    idd.setBindingTablePointer(bindingTablePointer);

    PreemptionHelper::programInterfaceDescriptorDataPreemption<Family>(&idd, args.preemptionMode);

    uint32_t samplerCount = 0;

    if constexpr (Family::supportsSampler) {
        if (args.device->getDeviceInfo().imageSupport) {

            uint32_t samplerStateOffset = 0;

            if (kernelDescriptor.payloadMappings.samplerTable.numSamplers > 0) {
                auto dsHeap = args.dynamicStateHeap;
                if (dsHeap == nullptr) {
                    dsHeap = ApiSpecificConfig::getBindlessConfiguration() ? args.device->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::GLOBAL_DSH) : container.getIndirectHeap(HeapType::DYNAMIC_STATE);
                }
                UNRECOVERABLE_IF(!dsHeap);

                samplerCount = kernelDescriptor.payloadMappings.samplerTable.numSamplers;
                samplerStateOffset = EncodeStates<Family>::copySamplerState(
                    dsHeap, kernelDescriptor.payloadMappings.samplerTable.tableOffset,
                    kernelDescriptor.payloadMappings.samplerTable.numSamplers, kernelDescriptor.payloadMappings.samplerTable.borderColor,
                    args.dispatchInterface->getDynamicStateHeapData(),
                    args.device->getBindlessHeapsHelper(), rootDeviceEnvironment);
                if (ApiSpecificConfig::getBindlessConfiguration()) {
                    container.getResidencyContainer().push_back(args.device->getBindlessHeapsHelper()->getHeap(NEO::BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH)->getGraphicsAllocation());
                }
            }

            idd.setSamplerStatePointer(samplerStateOffset);
        }
    }

    EncodeDispatchKernel<Family>::adjustBindingTablePrefetch(idd, samplerCount, bindingTableStateCount);

    uint64_t offsetThreadData = 0u;
    const uint32_t inlineDataSize = sizeof(INLINE_DATA);
    auto crossThreadData = args.dispatchInterface->getCrossThreadData();

    uint32_t inlineDataProgrammingOffset = 0u;

    if (inlineDataProgramming) {
        inlineDataProgrammingOffset = std::min(inlineDataSize, sizeCrossThreadData);
        auto dest = reinterpret_cast<char *>(walkerCmd.getInlineDataPointer());
        memcpy_s(dest, inlineDataProgrammingOffset, crossThreadData, inlineDataProgrammingOffset);
        sizeCrossThreadData -= inlineDataProgrammingOffset;
        crossThreadData = ptrOffset(crossThreadData, inlineDataProgrammingOffset);
        inlineDataProgramming = inlineDataProgrammingOffset != 0;
    }

    uint32_t sizeThreadData = sizePerThreadDataForWholeGroup + sizeCrossThreadData;
    uint32_t sizeForImplicitArgsPatching = NEO::ImplicitArgsHelper::getSizeForImplicitArgsPatching(pImplicitArgs, kernelDescriptor);
    uint32_t iohRequiredSize = sizeThreadData + sizeForImplicitArgsPatching;
    {
        auto heap = container.getIndirectHeap(HeapType::INDIRECT_OBJECT);
        UNRECOVERABLE_IF(!heap);
        heap->align(WALKER_TYPE::INDIRECTDATASTARTADDRESS_ALIGN_SIZE);
        void *ptr = nullptr;
        if (args.isKernelDispatchedFromImmediateCmdList) {
            ptr = container.getHeapWithRequiredSizeAndAlignment(HeapType::INDIRECT_OBJECT, iohRequiredSize, WALKER_TYPE::INDIRECTDATASTARTADDRESS_ALIGN_SIZE)->getSpace(iohRequiredSize);
        } else {
            ptr = container.getHeapSpaceAllowGrow(HeapType::INDIRECT_OBJECT, iohRequiredSize);
        }
        UNRECOVERABLE_IF(!ptr);
        offsetThreadData = (is64bit ? heap->getHeapGpuStartOffset() : heap->getHeapGpuBase()) + static_cast<uint64_t>(heap->getUsed() - sizeThreadData);

        if (pImplicitArgs) {
            offsetThreadData -= sizeof(ImplicitArgs);
            pImplicitArgs->localIdTablePtr = heap->getGraphicsAllocation()->getGpuAddress() + heap->getUsed() - iohRequiredSize;
            ptr = NEO::ImplicitArgsHelper::patchImplicitArgs(ptr, *pImplicitArgs, kernelDescriptor, std::make_pair(localIdsGenerationByRuntime, requiredWorkgroupOrder));
        }

        if (sizeCrossThreadData > 0) {
            memcpy_s(ptr, sizeCrossThreadData,
                     crossThreadData, sizeCrossThreadData);
        }
        if (args.isIndirect) {
            auto gpuPtr = heap->getGraphicsAllocation()->getGpuAddress() + static_cast<uint64_t>(heap->getUsed() - sizeThreadData - inlineDataProgrammingOffset);
            uint64_t implicitArgsGpuPtr = 0u;
            if (pImplicitArgs) {
                implicitArgsGpuPtr = gpuPtr + inlineDataProgrammingOffset - sizeof(ImplicitArgs);
            }
            EncodeIndirectParams<Family>::encode(container, gpuPtr, args.dispatchInterface, implicitArgsGpuPtr);
        }

        auto perThreadDataPtr = args.dispatchInterface->getPerThreadData();
        if (perThreadDataPtr != nullptr) {
            ptr = ptrOffset(ptr, sizeCrossThreadData);
            memcpy_s(ptr, sizePerThreadDataForWholeGroup,
                     perThreadDataPtr, sizePerThreadDataForWholeGroup);
        }
    }

    if (container.isAnyHeapDirty() ||
        args.requiresUncachedMocs) {

        PipeControlArgs syncArgs;
        syncArgs.dcFlushEnable = args.dcFlushEnable;
        MemorySynchronizationCommands<Family>::addSingleBarrier(*container.getCommandStream(), syncArgs);
        STATE_BASE_ADDRESS sbaCmd;
        auto gmmHelper = container.getDevice()->getGmmHelper();
        uint32_t statelessMocsIndex =
            args.requiresUncachedMocs ? (gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER_CACHELINE_MISALIGNED) >> 1) : (gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1);
        auto l1CachePolicy = container.l1CachePolicyDataRef()->getL1CacheValue(false);
        auto l1CachePolicyDebuggerActive = container.l1CachePolicyDataRef()->getL1CacheValue(true);

        EncodeStateBaseAddressArgs<Family> encodeStateBaseAddressArgs = {
            &container,                  // container
            sbaCmd,                      // sbaCmd
            nullptr,                     // sbaProperties
            statelessMocsIndex,          // statelessMocsIndex
            l1CachePolicy,               // l1CachePolicy
            l1CachePolicyDebuggerActive, // l1CachePolicyDebuggerActive
            args.useGlobalAtomics,       // useGlobalAtomics
            args.partitionCount > 1,     // multiOsContextCapable
            args.isRcs,                  // isRcs
            container.doubleSbaWaRef()}; // doubleSbaWa
        EncodeStateBaseAddress<Family>::encode(encodeStateBaseAddressArgs);
        container.setDirtyStateForAllHeaps(false);
    }

    if (NEO::PauseOnGpuProperties::pauseModeAllowed(NEO::DebugManager.flags.PauseOnEnqueue.get(), args.device->debugExecutionCounter.load(), NEO::PauseOnGpuProperties::PauseMode::BeforeWorkload)) {
        void *commandBuffer = listCmdBufferStream->getSpace(MemorySynchronizationCommands<Family>::getSizeForBarrierWithPostSyncOperation(args.device->getRootDeviceEnvironment(), false));
        args.additionalCommands->push_back(commandBuffer);

        EncodeSemaphore<Family>::applyMiSemaphoreWaitCommand(*listCmdBufferStream, *args.additionalCommands);
    }

    walkerCmd.setIndirectDataStartAddress(static_cast<uint32_t>(offsetThreadData));
    walkerCmd.setIndirectDataLength(sizeThreadData);

    EncodeDispatchKernel<Family>::encodeThreadData(walkerCmd,
                                                   nullptr,
                                                   threadDims,
                                                   args.dispatchInterface->getGroupSize(),
                                                   kernelDescriptor.kernelAttributes.simdSize,
                                                   kernelDescriptor.kernelAttributes.numLocalIdChannels,
                                                   args.dispatchInterface->getNumThreadsPerThreadGroup(),
                                                   args.dispatchInterface->getThreadExecutionMask(),
                                                   localIdsGenerationByRuntime,
                                                   inlineDataProgramming,
                                                   args.isIndirect,
                                                   requiredWorkgroupOrder,
                                                   rootDeviceEnvironment);

    using POSTSYNC_DATA = typename Family::POSTSYNC_DATA;
    auto &postSync = walkerCmd.getPostSync();
    if (args.eventAddress != 0) {
        postSync.setDataportPipelineFlush(true);
        if (args.isTimestampEvent) {
            postSync.setOperation(POSTSYNC_DATA::OPERATION_WRITE_TIMESTAMP);
        } else {
            postSync.setOperation(POSTSYNC_DATA::OPERATION_WRITE_IMMEDIATE_DATA);
            postSync.setImmediateData(args.postSyncImmValue);
        }
        UNRECOVERABLE_IF(!(isAligned<TimestampDestinationAddressAlignment>(args.eventAddress)));
        postSync.setDestinationAddress(args.eventAddress);

        EncodeDispatchKernel<Family>::setupPostSyncMocs(walkerCmd, rootDeviceEnvironment, args.dcFlushEnable);
        EncodeDispatchKernel<Family>::adjustTimestampPacket(walkerCmd, hwInfo);
    }

    if (DebugManager.flags.ForceComputeWalkerPostSyncFlush.get() == 1) {
        postSync.setDataportPipelineFlush(true);
        EncodeDispatchKernel<Family>::adjustTimestampPacket(walkerCmd, hwInfo);
    }

    walkerCmd.setPredicateEnable(args.isPredicate);

    auto threadGroupCount = walkerCmd.getThreadGroupIdXDimension() * walkerCmd.getThreadGroupIdYDimension() * walkerCmd.getThreadGroupIdZDimension();
    EncodeDispatchKernel<Family>::adjustInterfaceDescriptorData(idd, *args.device, hwInfo, threadGroupCount, kernelDescriptor.kernelAttributes.numGrfRequired, walkerCmd);

    EncodeDispatchKernel<Family>::appendAdditionalIDDFields(&idd, rootDeviceEnvironment, threadsPerThreadGroup,
                                                            args.dispatchInterface->getSlmTotalSize(),
                                                            args.dispatchInterface->getSlmPolicy());

    EncodeWalkerArgs walkerArgs{
        args.isCooperative ? KernelExecutionType::Concurrent : KernelExecutionType::Default,
        args.isHostScopeSignalEvent && args.isKernelUsingSystemAllocation,
        kernelDescriptor};
    EncodeDispatchKernel<Family>::encodeAdditionalWalkerFields(rootDeviceEnvironment, walkerCmd, walkerArgs);

    PreemptionHelper::applyPreemptionWaCmdsBegin<Family>(listCmdBufferStream, *args.device);

    if (args.partitionCount > 1 && !args.isInternal) {
        const uint64_t workPartitionAllocationGpuVa = args.device->getDefaultEngine().commandStreamReceiver->getWorkPartitionAllocationGpuAddress();
        if (args.eventAddress != 0) {
            postSync.setOperation(POSTSYNC_DATA::OPERATION_WRITE_TIMESTAMP);
        }
        ImplicitScalingDispatch<Family>::dispatchCommands(*listCmdBufferStream,
                                                          walkerCmd,
                                                          args.device->getDeviceBitfield(),
                                                          args.partitionCount,
                                                          !(container.getFlushTaskUsedForImmediate() || container.isUsingPrimaryBuffer()),
                                                          !args.isKernelDispatchedFromImmediateCmdList,
                                                          false,
                                                          args.dcFlushEnable,
                                                          args.isCooperative,
                                                          workPartitionAllocationGpuVa,
                                                          hwInfo);
    } else {
        args.partitionCount = 1;
        auto buffer = listCmdBufferStream->getSpace(sizeof(walkerCmd));
        *(decltype(walkerCmd) *)buffer = walkerCmd;
    }

    PreemptionHelper::applyPreemptionWaCmdsEnd<Family>(listCmdBufferStream, *args.device);

    if (NEO::PauseOnGpuProperties::pauseModeAllowed(NEO::DebugManager.flags.PauseOnEnqueue.get(), args.device->debugExecutionCounter.load(), NEO::PauseOnGpuProperties::PauseMode::AfterWorkload)) {
        void *commandBuffer = listCmdBufferStream->getSpace(MemorySynchronizationCommands<Family>::getSizeForBarrierWithPostSyncOperation(rootDeviceEnvironment, false));
        args.additionalCommands->push_back(commandBuffer);

        EncodeSemaphore<Family>::applyMiSemaphoreWaitCommand(*listCmdBufferStream, *args.additionalCommands);
    }
}

template <typename Family>
inline void EncodeDispatchKernel<Family>::setupPostSyncMocs(WALKER_TYPE &walkerCmd, const RootDeviceEnvironment &rootDeviceEnvironment, bool dcFlush) {
    auto &postSyncData = walkerCmd.getPostSync();
    auto gmmHelper = rootDeviceEnvironment.getGmmHelper();

    if (dcFlush) {
        postSyncData.setMocs(gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER_CACHELINE_MISALIGNED));
    } else {
        postSyncData.setMocs(gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER));
    }

    if (DebugManager.flags.OverridePostSyncMocs.get() != -1) {
        postSyncData.setMocs(DebugManager.flags.OverridePostSyncMocs.get());
    }
}

template <typename Family>
inline void EncodeDispatchKernel<Family>::encodeAdditionalWalkerFields(const RootDeviceEnvironment &rootDeviceEnvironment, WALKER_TYPE &walkerCmd, const EncodeWalkerArgs &walkerArgs) {
}

template <typename Family>
bool EncodeDispatchKernel<Family>::isRuntimeLocalIdsGenerationRequired(uint32_t activeChannels,
                                                                       const size_t *lws,
                                                                       std::array<uint8_t, 3> walkOrder,
                                                                       bool requireInputWalkOrder,
                                                                       uint32_t &requiredWalkOrder,
                                                                       uint32_t simd) {
    if (simd == 1) {
        return true;
    }
    bool hwGenerationOfLocalIdsEnabled = true;
    if (DebugManager.flags.EnableHwGenerationLocalIds.get() != -1) {
        hwGenerationOfLocalIdsEnabled = !!DebugManager.flags.EnableHwGenerationLocalIds.get();
    }
    if (hwGenerationOfLocalIdsEnabled) {
        if (activeChannels == 0) {
            return false;
        }

        size_t totalLwsSize = 1u;
        for (auto dimension = 0u; dimension < activeChannels; dimension++) {
            totalLwsSize *= lws[dimension];
        }

        if (totalLwsSize > 1024u) {
            return true;
        }

        // check if we need to follow kernel requirements
        if (requireInputWalkOrder) {
            for (uint32_t dimension = 0; dimension < activeChannels - 1; dimension++) {
                if (!Math::isPow2<size_t>(lws[walkOrder[dimension]])) {
                    return true;
                }
            }

            auto index = 0u;
            while (index < HwWalkOrderHelper::walkOrderPossibilties) {
                if (walkOrder[0] == HwWalkOrderHelper::compatibleDimensionOrders[index][0] &&
                    walkOrder[1] == HwWalkOrderHelper::compatibleDimensionOrders[index][1]) {
                    break;
                };
                index++;
            }
            DEBUG_BREAK_IF(index >= HwWalkOrderHelper::walkOrderPossibilties);

            requiredWalkOrder = index;
            return false;
        }

        // kernel doesn't specify any walk order requirements, check if we have any compatible
        for (uint32_t walkOrder = 0; walkOrder < HwWalkOrderHelper::walkOrderPossibilties; walkOrder++) {
            bool allDimensionsCompatible = true;
            for (uint32_t dimension = 0; dimension < activeChannels - 1; dimension++) {
                if (!Math::isPow2<size_t>(lws[HwWalkOrderHelper::compatibleDimensionOrders[walkOrder][dimension]])) {
                    allDimensionsCompatible = false;
                    break;
                }
            }
            if (allDimensionsCompatible) {
                requiredWalkOrder = walkOrder;
                return false;
            }
        }
    }
    return true;
}

template <typename Family>
void EncodeDispatchKernel<Family>::encodeThreadData(WALKER_TYPE &walkerCmd,
                                                    const uint32_t *startWorkGroup,
                                                    const uint32_t *numWorkGroups,
                                                    const uint32_t *workGroupSizes,
                                                    uint32_t simd,
                                                    uint32_t localIdDimensions,
                                                    uint32_t threadsPerThreadGroup,
                                                    uint32_t threadExecutionMask,
                                                    bool localIdsGenerationByRuntime,
                                                    bool inlineDataProgrammingRequired,
                                                    bool isIndirect,
                                                    uint32_t requiredWorkGroupOrder,
                                                    const RootDeviceEnvironment &rootDeviceEnvironment) {

    if (isIndirect) {
        walkerCmd.setIndirectParameterEnable(true);
    } else {
        walkerCmd.setThreadGroupIdXDimension(static_cast<uint32_t>(numWorkGroups[0]));
        walkerCmd.setThreadGroupIdYDimension(static_cast<uint32_t>(numWorkGroups[1]));
        walkerCmd.setThreadGroupIdZDimension(static_cast<uint32_t>(numWorkGroups[2]));
    }

    if (startWorkGroup) {
        walkerCmd.setThreadGroupIdStartingX(static_cast<uint32_t>(startWorkGroup[0]));
        walkerCmd.setThreadGroupIdStartingY(static_cast<uint32_t>(startWorkGroup[1]));
        walkerCmd.setThreadGroupIdStartingZ(static_cast<uint32_t>(startWorkGroup[2]));
    }

    uint64_t executionMask = threadExecutionMask;
    if (executionMask == 0) {
        auto workGroupSize = workGroupSizes[0] * workGroupSizes[1] * workGroupSizes[2];
        auto remainderSimdLanes = workGroupSize & (simd - 1);
        executionMask = maxNBitValue(remainderSimdLanes);
        if (!executionMask) {
            executionMask = maxNBitValue((simd == 1) ? 32 : simd);
        }
    }

    walkerCmd.setExecutionMask(static_cast<uint32_t>(executionMask));
    walkerCmd.setSimdSize(getSimdConfig<WALKER_TYPE>(simd));

    walkerCmd.setMessageSimd(walkerCmd.getSimdSize());

    if (DebugManager.flags.ForceSimdMessageSizeInWalker.get() != -1) {
        walkerCmd.setMessageSimd(DebugManager.flags.ForceSimdMessageSizeInWalker.get());
    }

    // 1) cross-thread inline data will be put into R1, but if kernel uses local ids, then cross-thread should be put further back
    // so whenever local ids are driver or hw generated, reserve space by setting right values for emitLocalIds
    // 2) Auto-generation of local ids should be possible, when in fact local ids are used
    if (!localIdsGenerationByRuntime && localIdDimensions > 0) {
        UNRECOVERABLE_IF(localIdDimensions != 3);
        uint32_t emitLocalIdsForDim = (1 << 0) | (1 << 1) | (1 << 2);
        walkerCmd.setEmitLocalId(emitLocalIdsForDim);

        walkerCmd.setLocalXMaximum(static_cast<uint32_t>(workGroupSizes[0] - 1));
        walkerCmd.setLocalYMaximum(static_cast<uint32_t>(workGroupSizes[1] - 1));
        walkerCmd.setLocalZMaximum(static_cast<uint32_t>(workGroupSizes[2] - 1));

        walkerCmd.setGenerateLocalId(1);
        walkerCmd.setWalkOrder(requiredWorkGroupOrder);
    }

    adjustWalkOrder(walkerCmd, requiredWorkGroupOrder, rootDeviceEnvironment);
    if (inlineDataProgrammingRequired == true) {
        walkerCmd.setEmitInlineParameter(1);
    }
}

template <typename Family>
inline bool EncodeDispatchKernel<Family>::isDshNeeded(const DeviceInfo &deviceInfo) {
    if constexpr (Family::supportsSampler) {
        return deviceInfo.imageSupport;
    }
    return false;
}

template <typename Family>
void EncodeStateBaseAddress<Family>::setSbaAddressesForDebugger(NEO::Debugger::SbaAddresses &sbaAddress, const STATE_BASE_ADDRESS &sbaCmd) {
    sbaAddress.bindlessSurfaceStateBaseAddress = sbaCmd.getBindlessSurfaceStateBaseAddress();
    sbaAddress.dynamicStateBaseAddress = sbaCmd.getDynamicStateBaseAddress();
    sbaAddress.generalStateBaseAddress = sbaCmd.getGeneralStateBaseAddress();
    sbaAddress.instructionBaseAddress = sbaCmd.getInstructionBaseAddress();
    sbaAddress.surfaceStateBaseAddress = sbaCmd.getSurfaceStateBaseAddress();
    sbaAddress.indirectObjectBaseAddress = 0;
}

template <typename Family>
void EncodeStateBaseAddress<Family>::encode(EncodeStateBaseAddressArgs<Family> &args) {
    auto &device = *args.container->getDevice();
    auto gmmHelper = device.getRootDeviceEnvironment().getGmmHelper();

    auto dsh = args.container->isHeapDirty(HeapType::DYNAMIC_STATE) ? args.container->getIndirectHeap(HeapType::DYNAMIC_STATE) : nullptr;
    auto ioh = args.container->isHeapDirty(HeapType::INDIRECT_OBJECT) ? args.container->getIndirectHeap(HeapType::INDIRECT_OBJECT) : nullptr;
    auto ssh = args.container->isHeapDirty(HeapType::SURFACE_STATE) ? args.container->getIndirectHeap(HeapType::SURFACE_STATE) : nullptr;
    auto isDebuggerActive = device.isDebuggerActive() || device.getDebugger() != nullptr;
    bool setGeneralStateBaseAddress = args.sbaProperties ? false : true;

    StateBaseAddressHelperArgs<Family> stateBaseAddressHelperArgs = {
        0,                                                  // generalStateBaseAddress
        args.container->getIndirectObjectHeapBaseAddress(), // indirectObjectHeapBaseAddress
        args.container->getInstructionHeapBaseAddress(),    // instructionHeapBaseAddress
        0,                                                  // globalHeapsBaseAddress
        0,                                                  // surfaceStateBaseAddress
        &args.sbaCmd,                                       // stateBaseAddressCmd
        args.sbaProperties,                                 // sbaProperties
        dsh,                                                // dsh
        ioh,                                                // ioh
        ssh,                                                // ssh
        gmmHelper,                                          // gmmHelper
        args.statelessMocsIndex,                            // statelessMocsIndex
        args.l1CachePolicy,                                 // l1CachePolicy
        args.l1CachePolicyDebuggerActive,                   // l1CachePolicyDebuggerActive
        NEO::MemoryCompressionState::NotApplicable,         // memoryCompressionState
        true,                                               // setInstructionStateBaseAddress
        setGeneralStateBaseAddress,                         // setGeneralStateBaseAddress
        false,                                              // useGlobalHeapsBaseAddress
        args.multiOsContextCapable,                         // isMultiOsContextCapable
        args.useGlobalAtomics,                              // useGlobalAtomics
        false,                                              // areMultipleSubDevicesInContext
        false,                                              // overrideSurfaceStateBaseAddress
        isDebuggerActive,                                   // isDebuggerActive
        args.doubleSbaWa                                    // doubleSbaWa
    };

    StateBaseAddressHelper<Family>::programStateBaseAddressIntoCommandStream(stateBaseAddressHelperArgs,
                                                                             *args.container->getCommandStream());

    if (args.sbaProperties) {
        if (args.sbaProperties->bindingTablePoolBaseAddress.value != StreamProperty64::initValue) {
            StateBaseAddressHelper<Family>::programBindingTableBaseAddress(*args.container->getCommandStream(),
                                                                           static_cast<uint64_t>(args.sbaProperties->bindingTablePoolBaseAddress.value),
                                                                           static_cast<uint32_t>(args.sbaProperties->bindingTablePoolSize.value),
                                                                           gmmHelper);
        }
    } else if (args.container->isHeapDirty(HeapType::SURFACE_STATE) && ssh != nullptr) {
        auto heap = args.container->getIndirectHeap(HeapType::SURFACE_STATE);
        StateBaseAddressHelper<Family>::programBindingTableBaseAddress(*args.container->getCommandStream(),
                                                                       *heap,
                                                                       gmmHelper);
    }
}

template <typename Family>
size_t EncodeStateBaseAddress<Family>::getRequiredSizeForStateBaseAddress(Device &device, CommandContainer &container, bool isRcs) {
    auto &hwInfo = device.getHardwareInfo();
    auto &productHelper = device.getProductHelper();

    size_t size = sizeof(typename Family::STATE_BASE_ADDRESS);
    if (productHelper.isAdditionalStateBaseAddressWARequired(hwInfo)) {
        size += sizeof(typename Family::STATE_BASE_ADDRESS);
    }

    if (container.isHeapDirty(HeapType::SURFACE_STATE)) {
        size += sizeof(typename Family::_3DSTATE_BINDING_TABLE_POOL_ALLOC);
    }

    return size;
}

template <typename Family>
void EncodeComputeMode<Family>::programComputeModeCommand(LinearStream &csr, StateComputeModeProperties &properties, const RootDeviceEnvironment &rootDeviceEnvironment, LogicalStateHelper *logicalStateHelper) {
    using STATE_COMPUTE_MODE = typename Family::STATE_COMPUTE_MODE;
    using FORCE_NON_COHERENT = typename STATE_COMPUTE_MODE::FORCE_NON_COHERENT;

    STATE_COMPUTE_MODE stateComputeMode = Family::cmdInitStateComputeMode;
    auto maskBits = stateComputeMode.getMaskBits();

    FORCE_NON_COHERENT coherencyValue = (properties.isCoherencyRequired.value == 1) ? FORCE_NON_COHERENT::FORCE_NON_COHERENT_FORCE_DISABLED
                                                                                    : FORCE_NON_COHERENT::FORCE_NON_COHERENT_FORCE_GPU_NON_COHERENT;
    stateComputeMode.setForceNonCoherent(coherencyValue);
    maskBits |= Family::stateComputeModeForceNonCoherentMask;

    stateComputeMode.setLargeGrfMode(properties.largeGrfMode.value == 1);
    maskBits |= Family::stateComputeModeLargeGrfModeMask;

    if (DebugManager.flags.ForceMultiGpuAtomics.get() != -1) {
        stateComputeMode.setForceDisableSupportForMultiGpuAtomics(!!DebugManager.flags.ForceMultiGpuAtomics.get());
        maskBits |= Family::stateComputeModeForceDisableSupportMultiGpuAtomics;
    }

    if (DebugManager.flags.ForceMultiGpuPartialWrites.get() != -1) {
        stateComputeMode.setForceDisableSupportForMultiGpuPartialWrites(!!DebugManager.flags.ForceMultiGpuPartialWrites.get());
        maskBits |= Family::stateComputeModeForceDisableSupportMultiGpuPartialWrites;
    }

    stateComputeMode.setMaskBits(maskBits);

    auto buffer = csr.getSpaceForCmd<STATE_COMPUTE_MODE>();
    *buffer = stateComputeMode;
}

template <typename Family>
void EncodeComputeMode<Family>::adjustPipelineSelect(CommandContainer &container, const NEO::KernelDescriptor &kernelDescriptor) {

    PipelineSelectArgs pipelineSelectArgs;
    pipelineSelectArgs.systolicPipelineSelectMode = kernelDescriptor.kernelAttributes.flags.usesSystolicPipelineSelectMode;
    pipelineSelectArgs.systolicPipelineSelectSupport = container.systolicModeSupportRef();

    PreambleHelper<Family>::programPipelineSelect(container.getCommandStream(),
                                                  pipelineSelectArgs,
                                                  container.getDevice()->getRootDeviceEnvironment());
}

template <typename Family>
inline void EncodeMediaInterfaceDescriptorLoad<Family>::encode(CommandContainer &container, IndirectHeap *childDsh) {
}

template <typename Family>
void EncodeMiFlushDW<Family>::adjust(MI_FLUSH_DW *miFlushDwCmd, const ProductHelper &productHelper) {
    miFlushDwCmd->setFlushCcs(1);
    miFlushDwCmd->setFlushLlc(1);
}

template <typename Family>
bool EncodeSurfaceState<Family>::isBindingTablePrefetchPreferred() {
    return false;
}

template <typename Family>
void EncodeSurfaceState<Family>::encodeExtraBufferParams(EncodeSurfaceStateArgs &args) {
    auto surfaceState = reinterpret_cast<R_SURFACE_STATE *>(args.outMemory);
    Gmm *gmm = args.allocation ? args.allocation->getDefaultGmm() : nullptr;
    uint32_t compressionFormat = 0;

    bool setConstCachePolicy = false;
    if (args.allocation && args.allocation->getAllocationType() == AllocationType::CONSTANT_SURFACE) {
        setConstCachePolicy = true;
    }

    if (surfaceState->getMemoryObjectControlState() == args.gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) &&
        DebugManager.flags.ForceL1Caching.get() != 0) {
        setConstCachePolicy = true;
    }

    if (setConstCachePolicy == true) {
        surfaceState->setMemoryObjectControlState(args.gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER_CONST));
    }

    encodeExtraCacheSettings(surfaceState, args);

    encodeImplicitScalingParams(args);

    if (EncodeSurfaceState<Family>::isAuxModeEnabled(surfaceState, gmm)) {
        auto resourceFormat = gmm->gmmResourceInfo->getResourceFormat();
        compressionFormat = args.gmmHelper->getClientContext()->getSurfaceStateCompressionFormat(resourceFormat);

        if (DebugManager.flags.ForceBufferCompressionFormat.get() != -1) {
            compressionFormat = DebugManager.flags.ForceBufferCompressionFormat.get();
        }
    }

    if (DebugManager.flags.EnableStatelessCompressionWithUnifiedMemory.get()) {
        if (args.allocation && !MemoryPoolHelper::isSystemMemoryPool(args.allocation->getMemoryPool())) {
            setCoherencyType(surfaceState, R_SURFACE_STATE::COHERENCY_TYPE_GPU_COHERENT);
            setBufferAuxParamsForCCS(surfaceState);
            compressionFormat = DebugManager.flags.FormatForStatelessCompressionWithUnifiedMemory.get();
        }
    }

    surfaceState->setCompressionFormat(compressionFormat);
}

template <typename Family>
inline void EncodeSurfaceState<Family>::setCoherencyType(R_SURFACE_STATE *surfaceState, COHERENCY_TYPE coherencyType) {
    surfaceState->setCoherencyType(R_SURFACE_STATE::COHERENCY_TYPE_GPU_COHERENT);
}

template <typename Family>
void EncodeSemaphore<Family>::programMiSemaphoreWait(MI_SEMAPHORE_WAIT *cmd,
                                                     uint64_t compareAddress,
                                                     uint32_t compareData,
                                                     COMPARE_OPERATION compareMode,
                                                     bool registerPollMode,
                                                     bool waitMode) {
    MI_SEMAPHORE_WAIT localCmd = Family::cmdInitMiSemaphoreWait;
    localCmd.setCompareOperation(compareMode);
    localCmd.setSemaphoreDataDword(compareData);
    localCmd.setSemaphoreGraphicsAddress(compareAddress);
    localCmd.setWaitMode(waitMode ? MI_SEMAPHORE_WAIT::WAIT_MODE::WAIT_MODE_POLLING_MODE : MI_SEMAPHORE_WAIT::WAIT_MODE::WAIT_MODE_SIGNAL_MODE);
    localCmd.setRegisterPollMode(registerPollMode ? MI_SEMAPHORE_WAIT::REGISTER_POLL_MODE::REGISTER_POLL_MODE_REGISTER_POLL : MI_SEMAPHORE_WAIT::REGISTER_POLL_MODE::REGISTER_POLL_MODE_MEMORY_POLL);

    *cmd = localCmd;
}

template <typename Family>
inline void EncodeWA<Family>::encodeAdditionalPipelineSelect(LinearStream &stream, const PipelineSelectArgs &args, bool is3DPipeline,
                                                             const RootDeviceEnvironment &rootDeviceEnvironment, bool isRcs) {}

template <typename Family>
inline size_t EncodeWA<Family>::getAdditionalPipelineSelectSize(Device &device, bool isRcs) {
    return 0u;
}
template <typename Family>
inline void EncodeWA<Family>::addPipeControlPriorToNonPipelinedStateCommand(LinearStream &commandStream, PipeControlArgs args,
                                                                            const RootDeviceEnvironment &rootDeviceEnvironment, bool isRcs) {

    auto &productHelper = rootDeviceEnvironment.getHelper<ProductHelper>();
    auto *releaseHelper = rootDeviceEnvironment.getReleaseHelper();
    auto &hwInfo = *rootDeviceEnvironment.getHardwareInfo();
    const auto &[isBasicWARequired, isExtendedWARequired] = productHelper.isPipeControlPriorToNonPipelinedStateCommandsWARequired(hwInfo, isRcs, releaseHelper);

    if (isExtendedWARequired) {
        args.textureCacheInvalidationEnable = true;
        args.hdcPipelineFlush = true;
        args.amfsFlushEnable = true;
        args.instructionCacheInvalidateEnable = true;
        args.constantCacheInvalidationEnable = true;
        args.stateCacheInvalidationEnable = true;

        args.dcFlushEnable = false;

        NEO::EncodeWA<Family>::setAdditionalPipeControlFlagsForNonPipelineStateCommand(args);
    } else if (isBasicWARequired) {
        args.hdcPipelineFlush = true;

        NEO::EncodeWA<Family>::setAdditionalPipeControlFlagsForNonPipelineStateCommand(args);
    }

    MemorySynchronizationCommands<Family>::addSingleBarrier(commandStream, args);
}

template <typename Family>
void EncodeWA<Family>::adjustCompressionFormatForPlanarImage(uint32_t &compressionFormat, int plane) {
    static_assert(sizeof(plane) == sizeof(GMM_YUV_PLANE_ENUM));
    if (plane == GMM_PLANE_Y) {
        compressionFormat &= 0xf;
    } else if ((plane == GMM_PLANE_U) || (plane == GMM_PLANE_V)) {
        compressionFormat |= 0x10;
    }
}

template <typename Family>
inline void EncodeStoreMemory<Family>::programStoreDataImm(MI_STORE_DATA_IMM *cmdBuffer,
                                                           uint64_t gpuAddress,
                                                           uint32_t dataDword0,
                                                           uint32_t dataDword1,
                                                           bool storeQword,
                                                           bool workloadPartitionOffset) {
    MI_STORE_DATA_IMM storeDataImmediate = Family::cmdInitStoreDataImm;
    storeDataImmediate.setAddress(gpuAddress);
    storeDataImmediate.setStoreQword(storeQword);
    storeDataImmediate.setDataDword0(dataDword0);
    if (storeQword) {
        storeDataImmediate.setDataDword1(dataDword1);
        storeDataImmediate.setDwordLength(MI_STORE_DATA_IMM::DWORD_LENGTH::DWORD_LENGTH_STORE_QWORD);
    } else {
        storeDataImmediate.setDwordLength(MI_STORE_DATA_IMM::DWORD_LENGTH::DWORD_LENGTH_STORE_DWORD);
    }
    storeDataImmediate.setWorkloadPartitionIdOffsetEnable(workloadPartitionOffset);
    *cmdBuffer = storeDataImmediate;
}

template <typename Family>
inline void EncodeStoreMMIO<Family>::appendFlags(MI_STORE_REGISTER_MEM *storeRegMem, bool workloadPartition) {
    storeRegMem->setMmioRemapEnable(true);
    storeRegMem->setWorkloadPartitionIdOffsetEnable(workloadPartition);
}

template <typename Family>
void EncodeDispatchKernel<Family>::adjustWalkOrder(WALKER_TYPE &walkerCmd, uint32_t requiredWorkGroupOrder, const RootDeviceEnvironment &rootDeviceEnvironment) {}

template <typename Family>
size_t EncodeDispatchKernel<Family>::additionalSizeRequiredDsh(uint32_t iddCount) {
    return 0u;
}

template <typename Family>
size_t EncodeStates<Family>::getSshHeapSize() {
    return 2 * MB;
}

} // namespace NEO
