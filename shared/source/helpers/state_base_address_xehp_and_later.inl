/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/gmm_helper/cache_settings_helper.h"
#include "shared/source/helpers/cache_policy.h"
#include "shared/source/helpers/state_base_address_base.inl"

namespace NEO {

template <typename GfxFamily>
void setSbaStatelessCompressionParams(typename GfxFamily::STATE_BASE_ADDRESS *stateBaseAddress, MemoryCompressionState memoryCompressionState) {
    using STATE_BASE_ADDRESS = typename GfxFamily::STATE_BASE_ADDRESS;

    if (memoryCompressionState == MemoryCompressionState::Enabled) {
        stateBaseAddress->setEnableMemoryCompressionForAllStatelessAccesses(STATE_BASE_ADDRESS::ENABLE_MEMORY_COMPRESSION_FOR_ALL_STATELESS_ACCESSES_ENABLED);
    } else {
        stateBaseAddress->setEnableMemoryCompressionForAllStatelessAccesses(STATE_BASE_ADDRESS::ENABLE_MEMORY_COMPRESSION_FOR_ALL_STATELESS_ACCESSES_DISABLED);
    }
}

template <typename GfxFamily>
void StateBaseAddressHelper<GfxFamily>::appendStateBaseAddressParameters(
    StateBaseAddressHelperArgs<GfxFamily> &args) {
    using RENDER_SURFACE_STATE = typename GfxFamily::RENDER_SURFACE_STATE;
    using STATE_BASE_ADDRESS = typename GfxFamily::STATE_BASE_ADDRESS;

    if (args.sbaProperties) {
        if (args.sbaProperties->indirectObjectBaseAddress.value != StreamProperty64::initValue) {
            auto baseAddress = static_cast<uint64_t>(args.sbaProperties->indirectObjectBaseAddress.value);
            args.stateBaseAddressCmd->setGeneralStateBaseAddress(args.gmmHelper->decanonize(baseAddress));
            args.stateBaseAddressCmd->setGeneralStateBaseAddressModifyEnable(true);
            args.stateBaseAddressCmd->setGeneralStateBufferSizeModifyEnable(true);
            args.stateBaseAddressCmd->setGeneralStateBufferSize(0xfffff);
        }
        if (args.sbaProperties->surfaceStateBaseAddress.value != StreamProperty64::initValue) {
            args.stateBaseAddressCmd->setBindlessSurfaceStateBaseAddressModifyEnable(true);
            args.stateBaseAddressCmd->setBindlessSurfaceStateBaseAddress(static_cast<uint64_t>(args.sbaProperties->surfaceStateBaseAddress.value));
            args.stateBaseAddressCmd->setBindlessSurfaceStateSize(static_cast<uint32_t>(args.sbaProperties->surfaceStateSize.value));
        }
        if (args.sbaProperties->globalAtomics.value != StreamProperty::initValue) {
            args.useGlobalAtomics = !!args.sbaProperties->globalAtomics.value;
        }
    }
    if (args.setGeneralStateBaseAddress && is64bit) {
        args.stateBaseAddressCmd->setGeneralStateBaseAddress(args.gmmHelper->decanonize(args.indirectObjectHeapBaseAddress));
    }

    if (!args.useGlobalHeapsBaseAddress && args.ssh) {
        args.stateBaseAddressCmd->setBindlessSurfaceStateBaseAddress(args.ssh->getHeapGpuBase());
        args.stateBaseAddressCmd->setBindlessSurfaceStateBaseAddressModifyEnable(true);
        const auto surfaceStateCount = args.ssh->getMaxAvailableSpace() / sizeof(RENDER_SURFACE_STATE);
        args.stateBaseAddressCmd->setBindlessSurfaceStateSize(static_cast<uint32_t>(surfaceStateCount - 1));
    }

    args.stateBaseAddressCmd->setBindlessSamplerStateBaseAddressModifyEnable(true);

    auto &productHelper = args.gmmHelper->getRootDeviceEnvironment().template getHelper<ProductHelper>();

    auto heapResourceUsage = CacheSettingsHelper::getGmmUsageType(AllocationType::INTERNAL_HEAP, DebugManager.flags.DisableCachingForHeaps.get(), productHelper);
    auto heapMocsValue = args.gmmHelper->getMOCS(heapResourceUsage);

    args.stateBaseAddressCmd->setSurfaceStateMemoryObjectControlState(heapMocsValue);
    args.stateBaseAddressCmd->setDynamicStateMemoryObjectControlState(heapMocsValue);
    args.stateBaseAddressCmd->setGeneralStateMemoryObjectControlState(heapMocsValue);
    args.stateBaseAddressCmd->setBindlessSurfaceStateMemoryObjectControlState(heapMocsValue);
    args.stateBaseAddressCmd->setBindlessSamplerStateMemoryObjectControlState(heapMocsValue);

    bool enableMultiGpuAtomics = args.isMultiOsContextCapable;
    if (DebugManager.flags.EnableMultiGpuAtomicsOptimization.get()) {
        enableMultiGpuAtomics = args.useGlobalAtomics && (args.isMultiOsContextCapable || args.areMultipleSubDevicesInContext);
    }
    args.stateBaseAddressCmd->setDisableSupportForMultiGpuAtomicsForStatelessAccesses(!enableMultiGpuAtomics);

    args.stateBaseAddressCmd->setDisableSupportForMultiGpuPartialWritesForStatelessMessages(!args.isMultiOsContextCapable);

    if (DebugManager.flags.ForceMultiGpuAtomics.get() != -1) {
        args.stateBaseAddressCmd->setDisableSupportForMultiGpuAtomicsForStatelessAccesses(!!DebugManager.flags.ForceMultiGpuAtomics.get());
    }

    if (DebugManager.flags.ForceMultiGpuPartialWrites.get() != -1) {
        args.stateBaseAddressCmd->setDisableSupportForMultiGpuPartialWritesForStatelessMessages(!!DebugManager.flags.ForceMultiGpuPartialWrites.get());
    }

    if (args.memoryCompressionState != MemoryCompressionState::NotApplicable) {
        setSbaStatelessCompressionParams<GfxFamily>(args.stateBaseAddressCmd, args.memoryCompressionState);
    }

    bool l3MocsEnabled = (args.stateBaseAddressCmd->getStatelessDataPortAccessMemoryObjectControlState() >> 1) == (args.gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1);
    bool constMocsAllowed = (l3MocsEnabled && (DebugManager.flags.ForceL1Caching.get() != 0));

    if (constMocsAllowed) {
        auto constMocsIndex = args.gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER_CONST);
        GmmHelper::applyMocsEncryptionBit(constMocsIndex);

        args.stateBaseAddressCmd->setStatelessDataPortAccessMemoryObjectControlState(constMocsIndex);
    }

    appendExtraCacheSettings(args);
}

template <typename GfxFamily>
void StateBaseAddressHelper<GfxFamily>::programBindingTableBaseAddress(LinearStream &commandStream, uint64_t baseAddress, uint32_t sizeInPages, GmmHelper *gmmHelper) {
    using _3DSTATE_BINDING_TABLE_POOL_ALLOC = typename GfxFamily::_3DSTATE_BINDING_TABLE_POOL_ALLOC;

    auto bindingTablePoolAlloc = commandStream.getSpaceForCmd<_3DSTATE_BINDING_TABLE_POOL_ALLOC>();
    _3DSTATE_BINDING_TABLE_POOL_ALLOC cmd = GfxFamily::cmdInitStateBindingTablePoolAlloc;
    cmd.setBindingTablePoolBaseAddress(baseAddress);
    cmd.setBindingTablePoolBufferSize(sizeInPages);
    cmd.setSurfaceObjectControlStateIndexToMocsTables(gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_STATE_HEAP_BUFFER));
    if (DebugManager.flags.DisableCachingForHeaps.get()) {
        cmd.setSurfaceObjectControlStateIndexToMocsTables(gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_SYSTEM_MEMORY_BUFFER_CACHELINE_MISALIGNED));
    }

    *bindingTablePoolAlloc = cmd;
}

template <typename GfxFamily>
uint32_t StateBaseAddressHelper<GfxFamily>::getMaxBindlessSurfaceStates() {
    return std::numeric_limits<uint32_t>::max();
}

template <typename GfxFamily>
void StateBaseAddressHelper<GfxFamily>::appendExtraCacheSettings(StateBaseAddressHelperArgs<GfxFamily> &args) {
    auto cachePolicy = args.isDebuggerActive ? args.l1CachePolicyDebuggerActive : args.l1CachePolicy;
    args.stateBaseAddressCmd->setL1CachePolicyL1CacheControl(static_cast<typename STATE_BASE_ADDRESS::L1_CACHE_POLICY>(cachePolicy));

    if (DebugManager.flags.ForceStatelessL1CachingPolicy.get() != -1 &&
        DebugManager.flags.ForceAllResourcesUncached.get() == false) {
        args.stateBaseAddressCmd->setL1CachePolicyL1CacheControl(static_cast<typename STATE_BASE_ADDRESS::L1_CACHE_POLICY>(DebugManager.flags.ForceStatelessL1CachingPolicy.get()));
    }
}

template <typename GfxFamily>
void StateBaseAddressHelper<GfxFamily>::appendIohParameters(StateBaseAddressHelperArgs<GfxFamily> &args) {
}

} // namespace NEO
