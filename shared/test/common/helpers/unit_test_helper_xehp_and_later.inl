/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_container/command_encoder.h"
#include "shared/source/command_stream/linear_stream.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/kernel/kernel_descriptor.h"
#include "shared/test/common/cmd_parse/hw_parse.h"
#include "shared/test/common/helpers/unit_test_helper.h"

namespace NEO {

template <typename GfxFamily>
const uint32_t UnitTestHelper<GfxFamily>::smallestTestableSimdSize = 16;

template <typename GfxFamily>
uint32_t UnitTestHelper<GfxFamily>::getDebugModeRegisterOffset() {
    return 0x20d8;
}

template <typename GfxFamily>
uint32_t UnitTestHelper<GfxFamily>::getDebugModeRegisterValue() {
    return (1u << 5) | (1u << 21);
}

template <typename GfxFamily>
uint32_t UnitTestHelper<GfxFamily>::getTdCtlRegisterValue() {
    return (1u << 7) | (1u << 4) | (1u << 2) | (1u << 0);
}

template <typename GfxFamily>
bool UnitTestHelper<GfxFamily>::isL3ConfigProgrammable() {
    return false;
};

template <typename GfxFamily>
bool UnitTestHelper<GfxFamily>::evaluateDshUsage(size_t sizeBeforeEnqueue, size_t sizeAfterEnqueue, const KernelDescriptor *kernelDescriptor, uint32_t rootDeviceIndex) {
    if (kernelDescriptor == nullptr) {
        if (sizeBeforeEnqueue == sizeAfterEnqueue) {
            return true;
        }
        return false;
    }

    auto samplerCount = kernelDescriptor->payloadMappings.samplerTable.numSamplers;
    if (samplerCount > 0) {
        if (sizeBeforeEnqueue != sizeAfterEnqueue) {
            return true;
        }
        return false;
    } else {
        if (sizeBeforeEnqueue == sizeAfterEnqueue) {
            return true;
        }
        return false;
    }
}

template <typename GfxFamily>
bool UnitTestHelper<GfxFamily>::isTimestampPacketWriteSupported() {
    return true;
};

template <typename GfxFamily>
bool UnitTestHelper<GfxFamily>::isExpectMemoryNotEqualSupported() {
    return true;
}

template <typename GfxFamily>
uint32_t UnitTestHelper<GfxFamily>::getDefaultSshUsage() {
    return (32 * 2 * 64);
}

template <typename GfxFamily>
bool UnitTestHelper<GfxFamily>::isAdditionalMiSemaphoreWait(const typename GfxFamily::MI_SEMAPHORE_WAIT &semaphoreWait) {
    return (semaphoreWait.getSemaphoreDataDword() == EncodeSemaphore<GfxFamily>::invalidHardwareTag);
}

template <typename GfxFamily>
bool UnitTestHelper<GfxFamily>::evaluateGshAddressForScratchSpace(uint64_t usedScratchGpuAddress, uint64_t retrievedGshAddress) {
    return 0llu == retrievedGshAddress;
}

template <typename GfxFamily>
auto UnitTestHelper<GfxFamily>::getCoherencyTypeSupported(COHERENCY_TYPE coherencyType) -> decltype(coherencyType) {
    return GfxFamily::RENDER_SURFACE_STATE::COHERENCY_TYPE_GPU_COHERENT;
}

template <typename GfxFamily>
inline void UnitTestHelper<GfxFamily>::adjustKernelDescriptorForImplicitArgs(KernelDescriptor &kernelDescriptor) {
    kernelDescriptor.kernelAttributes.flags.requiresImplicitArgs = true;
}

template <typename GfxFamily>
std::vector<bool> UnitTestHelper<GfxFamily>::getProgrammedLargeGrfValues(CommandStreamReceiver &csr, LinearStream &linearStream) {
    using STATE_COMPUTE_MODE = typename GfxFamily::STATE_COMPUTE_MODE;

    std::vector<bool> largeGrfValues;
    HardwareParse hwParser;
    hwParser.parseCommands<GfxFamily>(csr, linearStream);
    auto commands = hwParser.getCommandsList<STATE_COMPUTE_MODE>();
    for (auto &cmd : commands) {
        largeGrfValues.push_back(reinterpret_cast<STATE_COMPUTE_MODE *>(cmd)->getLargeGrfMode());
    }
    return largeGrfValues;
}

template <typename GfxFamily>
inline bool UnitTestHelper<GfxFamily>::getWorkloadPartitionForStoreRegisterMemCmd(typename GfxFamily::MI_STORE_REGISTER_MEM &storeRegisterMem) {
    return storeRegisterMem.getWorkloadPartitionIdOffsetEnable();
}

template <typename GfxFamily>
size_t UnitTestHelper<GfxFamily>::getAdditionalDshSize(uint32_t iddCount) {
    return 0;
}

} // namespace NEO
