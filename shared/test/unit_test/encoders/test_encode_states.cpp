/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_container/encode_surface_state.h"
#include "shared/source/command_stream/stream_properties.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/helpers/bindless_heaps_helper.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/ptr_math.h"
#include "shared/source/indirect_heap/indirect_heap.h"
#include "shared/test/common/cmd_parse/gen_cmd_parse.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/helpers/unit_test_helper.h"
#include "shared/test/common/libult/ult_command_stream_receiver.h"
#include "shared/test/common/mocks/mock_device.h"
#include "shared/test/common/mocks/ult_device_factory.h"
#include "shared/test/common/test_macros/hw_test.h"
#include "shared/test/unit_test/fixtures/command_container_fixture.h"
#include "shared/test/unit_test/fixtures/front_window_fixture.h"

#include "encode_surface_state_args.h"
#include "test_traits_common.h"

using namespace NEO;

using CommandEncodeStatesTest = Test<CommandEncodeStatesFixture>;

HWTEST_F(CommandEncodeStatesTest, GivenCommandStreamWhenEncodeCopySamplerStateThenIndirectStatePointerIsCorrect) {
    bool deviceUsesDsh = pDevice->getHardwareInfo().capabilityTable.supportsImages;
    if (!deviceUsesDsh) {
        GTEST_SKIP();
    }
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;
    uint32_t numSamplers = 1;
    SAMPLER_STATE samplerState{};

    auto dsh = cmdContainer->getIndirectHeap(HeapType::DYNAMIC_STATE);
    auto usedBefore = dsh->getUsed();
    auto samplerStateOffset = EncodeStates<FamilyType>::copySamplerState(dsh, 0, numSamplers, 0, &samplerState, nullptr, pDevice->getRootDeviceEnvironment());

    auto pSmplr = reinterpret_cast<SAMPLER_STATE *>(ptrOffset(dsh->getCpuBase(), samplerStateOffset));
    EXPECT_EQ(pSmplr->getIndirectStatePointer(), usedBefore);
}

HWTEST2_F(CommandEncodeStatesTest, givenDebugVariableSetWhenCopyingSamplerStateThenSetLowQualityFilterMode, IsAtLeastGen12lp) {
    bool deviceUsesDsh = pDevice->getHardwareInfo().capabilityTable.supportsImages;
    if (!deviceUsesDsh) {
        GTEST_SKIP();
    }
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;

    DebugManagerStateRestore restore;
    DebugManager.flags.ForceSamplerLowFilteringPrecision.set(true);

    uint32_t numSamplers = 1;
    SAMPLER_STATE samplerState;
    samplerState.init();

    EXPECT_EQ(samplerState.getLowQualityFilter(), SAMPLER_STATE::LOW_QUALITY_FILTER_DISABLE);

    auto dsh = cmdContainer->getIndirectHeap(HeapType::DYNAMIC_STATE);

    auto samplerStateOffset = EncodeStates<FamilyType>::copySamplerState(dsh, 0, numSamplers, 0, &samplerState, nullptr, pDevice->getRootDeviceEnvironment());

    auto pSamplerState = reinterpret_cast<SAMPLER_STATE *>(ptrOffset(dsh->getCpuBase(), samplerStateOffset));
    EXPECT_EQ(pSamplerState->getLowQualityFilter(), SAMPLER_STATE::LOW_QUALITY_FILTER_ENABLE);
}
using BindlessCommandEncodeStatesTest = Test<BindlessCommandEncodeStatesFixture>;

HWTEST_F(BindlessCommandEncodeStatesTest, GivenBindlessEnabledWhenBorderColorWithoutAlphaThenBorderColorPtrReturned) {
    using SAMPLER_BORDER_COLOR_STATE = typename FamilyType::SAMPLER_BORDER_COLOR_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseBindlessMode.set(1);
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;
    uint32_t numSamplers = 1;
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->createBindlessHeapsHelper(pDevice->getMemoryManager(),
                                                                                                                         pDevice->getNumGenericSubDevices() > 1,
                                                                                                                         pDevice->getRootDeviceIndex(),
                                                                                                                         pDevice->getDeviceBitfield());

    uint32_t borderColorSize = 0x40;
    SAMPLER_BORDER_COLOR_STATE samplerState;
    samplerState.init();
    auto dsh = pDevice->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
    EncodeStates<FamilyType>::copySamplerState(dsh, borderColorSize, numSamplers, 0, &samplerState, pDevice->getBindlessHeapsHelper(), pDevice->getRootDeviceEnvironment());
    auto expectedValue = pDevice->getBindlessHeapsHelper()->getDefaultBorderColorOffset();

    auto pSmplr = reinterpret_cast<SAMPLER_STATE *>(dsh->getGraphicsAllocation()->getUnderlyingBuffer());
    EXPECT_EQ(pSmplr->getIndirectStatePointer(), expectedValue);
}

HWTEST_F(BindlessCommandEncodeStatesTest, GivenBindlessEnabledWhenBorderColorWithAlphaThenBorderColorPtrOffseted) {
    using SAMPLER_BORDER_COLOR_STATE = typename FamilyType::SAMPLER_BORDER_COLOR_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseBindlessMode.set(1);
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;
    uint32_t numSamplers = 1;
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->createBindlessHeapsHelper(pDevice->getMemoryManager(),
                                                                                                                         pDevice->getNumGenericSubDevices() > 1,
                                                                                                                         pDevice->getRootDeviceIndex(),
                                                                                                                         pDevice->getDeviceBitfield());

    uint32_t borderColorSize = 0x40;
    SAMPLER_BORDER_COLOR_STATE samplerState;
    samplerState.init();
    samplerState.setBorderColorAlpha(1.0);
    auto dsh = pDevice->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
    EncodeStates<FamilyType>::copySamplerState(dsh, borderColorSize, numSamplers, 0, &samplerState, pDevice->getBindlessHeapsHelper(), pDevice->getRootDeviceEnvironment());
    auto expectedValue = pDevice->getBindlessHeapsHelper()->getAlphaBorderColorOffset();

    auto pSmplr = reinterpret_cast<SAMPLER_STATE *>(dsh->getGraphicsAllocation()->getUnderlyingBuffer());
    EXPECT_EQ(pSmplr->getIndirectStatePointer(), expectedValue);
}

HWTEST_F(BindlessCommandEncodeStatesTest, GivenBindlessEnabledWhenBorderColorsRedChanelIsNotZeroThenExceptionThrown) {
    using SAMPLER_BORDER_COLOR_STATE = typename FamilyType::SAMPLER_BORDER_COLOR_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseBindlessMode.set(1);
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;
    uint32_t numSamplers = 1;
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->createBindlessHeapsHelper(pDevice->getMemoryManager(),
                                                                                                                         pDevice->getNumGenericSubDevices() > 1,
                                                                                                                         pDevice->getRootDeviceIndex(),
                                                                                                                         pDevice->getDeviceBitfield());

    uint32_t borderColorSize = 0x40;
    SAMPLER_BORDER_COLOR_STATE samplerState;
    samplerState.init();
    samplerState.setBorderColorRed(0.5);
    auto dsh = pDevice->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
    EXPECT_THROW(EncodeStates<FamilyType>::copySamplerState(dsh, borderColorSize, numSamplers, 0, &samplerState, pDevice->getBindlessHeapsHelper(), pDevice->getRootDeviceEnvironment()), std::exception);
}

HWTEST_F(BindlessCommandEncodeStatesTest, GivenBindlessEnabledWhenBorderColorsGreenChanelIsNotZeroThenExceptionThrown) {
    using SAMPLER_BORDER_COLOR_STATE = typename FamilyType::SAMPLER_BORDER_COLOR_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseBindlessMode.set(1);
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;
    uint32_t numSamplers = 1;
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->createBindlessHeapsHelper(pDevice->getMemoryManager(),
                                                                                                                         pDevice->getNumGenericSubDevices() > 1,
                                                                                                                         pDevice->getRootDeviceIndex(),
                                                                                                                         pDevice->getDeviceBitfield());

    uint32_t borderColorSize = 0x40;
    SAMPLER_BORDER_COLOR_STATE samplerState;
    samplerState.init();
    samplerState.setBorderColorGreen(0.5);
    auto dsh = pDevice->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
    EXPECT_THROW(EncodeStates<FamilyType>::copySamplerState(dsh, borderColorSize, numSamplers, 0, &samplerState, pDevice->getBindlessHeapsHelper(), pDevice->getRootDeviceEnvironment()), std::exception);
}

HWTEST_F(BindlessCommandEncodeStatesTest, GivenBindlessEnabledWhenBorderColorsBlueChanelIsNotZeroThenExceptionThrown) {
    using SAMPLER_BORDER_COLOR_STATE = typename FamilyType::SAMPLER_BORDER_COLOR_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseBindlessMode.set(1);
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;
    uint32_t numSamplers = 1;
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->createBindlessHeapsHelper(pDevice->getMemoryManager(),
                                                                                                                         pDevice->getNumGenericSubDevices() > 1,
                                                                                                                         pDevice->getRootDeviceIndex(),
                                                                                                                         pDevice->getDeviceBitfield());
    uint32_t borderColorSize = 0x40;
    SAMPLER_BORDER_COLOR_STATE samplerState;
    samplerState.init();
    samplerState.setBorderColorBlue(0.5);
    auto dsh = pDevice->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
    EXPECT_THROW(EncodeStates<FamilyType>::copySamplerState(dsh, borderColorSize, numSamplers, 0, &samplerState, pDevice->getBindlessHeapsHelper(), pDevice->getRootDeviceEnvironment()), std::exception);
}

HWTEST_F(BindlessCommandEncodeStatesTest, GivenBindlessEnabledWhenBorderColorsAlphaChanelIsNotZeroOrOneThenExceptionThrown) {
    using SAMPLER_BORDER_COLOR_STATE = typename FamilyType::SAMPLER_BORDER_COLOR_STATE;
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseBindlessMode.set(1);
    using SAMPLER_STATE = typename FamilyType::SAMPLER_STATE;
    uint32_t numSamplers = 1;
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->createBindlessHeapsHelper(pDevice->getMemoryManager(),
                                                                                                                         pDevice->getNumGenericSubDevices() > 1,
                                                                                                                         pDevice->getRootDeviceIndex(),
                                                                                                                         pDevice->getDeviceBitfield());

    uint32_t borderColorSize = 0x40;
    SAMPLER_BORDER_COLOR_STATE samplerState;
    samplerState.init();
    samplerState.setBorderColorAlpha(0.5);
    auto dsh = pDevice->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
    EXPECT_THROW(EncodeStates<FamilyType>::copySamplerState(dsh, borderColorSize, numSamplers, 0, &samplerState, pDevice->getBindlessHeapsHelper(), pDevice->getRootDeviceEnvironment()), std::exception);
}

HWTEST_F(CommandEncodeStatesTest, givenCreatedSurfaceStateBufferWhenAllocationProvidedThenUseAllocationAsInput) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    using SURFACE_TYPE = typename RENDER_SURFACE_STATE::SURFACE_TYPE;
    using AUXILIARY_SURFACE_MODE = typename RENDER_SURFACE_STATE::AUXILIARY_SURFACE_MODE;

    void *stateBuffer = alignedMalloc(sizeof(RENDER_SURFACE_STATE), sizeof(RENDER_SURFACE_STATE));
    ASSERT_NE(nullptr, stateBuffer);
    RENDER_SURFACE_STATE *state = reinterpret_cast<RENDER_SURFACE_STATE *>(stateBuffer);

    memset(stateBuffer, 0, sizeof(RENDER_SURFACE_STATE));

    size_t size = 0x1000;
    SURFACE_STATE_BUFFER_LENGTH length;

    void *cpuAddr = reinterpret_cast<void *>(0x4000);
    uint64_t gpuAddr = 0x4000u;
    size_t allocSize = size;
    length.length = static_cast<uint32_t>(allocSize - 1);
    GraphicsAllocation allocation(0, AllocationType::UNKNOWN, cpuAddr, gpuAddr, 0u, allocSize, MemoryPool::MemoryNull, 1);

    NEO::EncodeSurfaceStateArgs args;
    args.outMemory = stateBuffer;
    args.graphicsAddress = gpuAddr;
    args.size = allocSize;
    args.mocs = 1;
    args.numAvailableDevices = 1;
    args.allocation = &allocation;
    args.gmmHelper = pDevice->getGmmHelper();
    args.areMultipleSubDevicesInContext = true;
    EncodeSurfaceState<FamilyType>::encodeBuffer(args);

    EXPECT_EQ(length.surfaceState.depth + 1u, state->getDepth());
    EXPECT_EQ(length.surfaceState.width + 1u, state->getWidth());
    EXPECT_EQ(length.surfaceState.height + 1u, state->getHeight());
    EXPECT_EQ(gpuAddr, state->getSurfaceBaseAddress());

    alignedFree(stateBuffer);
}

HWTEST_F(CommandEncodeStatesTest, givenCreatedSurfaceStateBufferWhenAllocationNotProvidedThenStateTypeIsNull) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;

    void *stateBuffer = alignedMalloc(sizeof(RENDER_SURFACE_STATE), sizeof(RENDER_SURFACE_STATE));
    ASSERT_NE(nullptr, stateBuffer);
    RENDER_SURFACE_STATE *state = reinterpret_cast<RENDER_SURFACE_STATE *>(stateBuffer);

    memset(stateBuffer, 0, sizeof(RENDER_SURFACE_STATE));

    size_t size = 0x1000;
    SURFACE_STATE_BUFFER_LENGTH length;

    uint64_t gpuAddr = 0;
    size_t allocSize = size;
    length.length = static_cast<uint32_t>(allocSize - 1);

    NEO::EncodeSurfaceStateArgs args;
    args.outMemory = stateBuffer;
    args.graphicsAddress = gpuAddr;
    args.size = allocSize;
    args.mocs = 1;
    args.cpuCoherent = true;
    args.numAvailableDevices = 1;
    args.gmmHelper = pDevice->getGmmHelper();
    args.areMultipleSubDevicesInContext = true;

    EncodeSurfaceState<FamilyType>::encodeBuffer(args);

    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_NULL, state->getSurfaceType());
    EXPECT_EQ(UnitTestHelper<FamilyType>::getCoherencyTypeSupported(RENDER_SURFACE_STATE::COHERENCY_TYPE_IA_COHERENT), state->getCoherencyType());

    alignedFree(stateBuffer);
}

HWTEST_F(CommandEncodeStatesTest, givenCreatedSurfaceStateBufferWhenGpuCoherencyProvidedThenCoherencyGpuIsSet) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;

    void *stateBuffer = alignedMalloc(sizeof(RENDER_SURFACE_STATE), sizeof(RENDER_SURFACE_STATE));
    ASSERT_NE(nullptr, stateBuffer);
    RENDER_SURFACE_STATE *state = reinterpret_cast<RENDER_SURFACE_STATE *>(stateBuffer);

    memset(stateBuffer, 0, sizeof(RENDER_SURFACE_STATE));

    size_t size = 0x1000;
    SURFACE_STATE_BUFFER_LENGTH length;

    uint64_t gpuAddr = 0;
    size_t allocSize = size;
    length.length = static_cast<uint32_t>(allocSize - 1);

    NEO::EncodeSurfaceStateArgs args;
    args.outMemory = stateBuffer;
    args.graphicsAddress = gpuAddr;
    args.size = allocSize;
    args.mocs = 1;
    args.numAvailableDevices = 1;
    args.gmmHelper = pDevice->getGmmHelper();
    args.areMultipleSubDevicesInContext = true;
    EncodeSurfaceState<FamilyType>::encodeBuffer(args);

    EXPECT_EQ(RENDER_SURFACE_STATE::COHERENCY_TYPE_GPU_COHERENT, state->getCoherencyType());

    alignedFree(stateBuffer);
}

HWTEST2_F(CommandEncodeStatesTest, givenCommandContainerWithDirtyHeapsWhenSetStateBaseAddressCalledThenStateBaseAddressAreNotSet, MatchAny) {
    using STATE_BASE_ADDRESS = typename FamilyType::STATE_BASE_ADDRESS;
    cmdContainer->dirtyHeaps = 0;

    cmdContainer->setHeapDirty(NEO::HeapType::DYNAMIC_STATE);
    cmdContainer->setHeapDirty(NEO::HeapType::INDIRECT_OBJECT);
    cmdContainer->setHeapDirty(NEO::HeapType::SURFACE_STATE);

    auto gmmHelper = cmdContainer->getDevice()->getRootDeviceEnvironment().getGmmHelper();
    uint32_t statelessMocsIndex = (gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1);

    STATE_BASE_ADDRESS sba;

    EncodeStateBaseAddressArgs<FamilyType> args = createDefaultEncodeStateBaseAddressArgs<FamilyType>(cmdContainer.get(), sba, statelessMocsIndex);

    EncodeStateBaseAddress<FamilyType>::encode(args);

    auto dsh = cmdContainer->getIndirectHeap(NEO::HeapType::DYNAMIC_STATE);
    auto ssh = cmdContainer->getIndirectHeap(NEO::HeapType::SURFACE_STATE);

    GenCmdList commands;
    CmdParse<FamilyType>::parseCommandBuffer(commands, ptrOffset(cmdContainer->getCommandStream()->getCpuBase(), 0), cmdContainer->getCommandStream()->getUsed());

    auto itorCmd = find<STATE_BASE_ADDRESS *>(commands.begin(), commands.end());
    auto pCmd = genCmdCast<STATE_BASE_ADDRESS *>(*itorCmd);

    if (pDevice->getDeviceInfo().imageSupport) {
        EXPECT_EQ(dsh->getHeapGpuBase(), pCmd->getDynamicStateBaseAddress());
    } else {
        EXPECT_EQ(dsh, nullptr);
    }
    EXPECT_EQ(ssh->getHeapGpuBase(), pCmd->getSurfaceStateBaseAddress());

    EXPECT_EQ(sba.getDynamicStateBaseAddress(), pCmd->getDynamicStateBaseAddress());
    EXPECT_EQ(sba.getSurfaceStateBaseAddress(), pCmd->getSurfaceStateBaseAddress());

    if constexpr (TestTraits<gfxCoreFamily>::iohInSbaSupported) {
        auto ioh = cmdContainer->getIndirectHeap(NEO::HeapType::INDIRECT_OBJECT);

        EXPECT_EQ(ioh->getHeapGpuBase(), pCmd->getIndirectObjectBaseAddress());
        EXPECT_EQ(sba.getIndirectObjectBaseAddress(), pCmd->getIndirectObjectBaseAddress());
    }
}

HWTEST_F(CommandEncodeStatesTest, givenCommandContainerWhenSetStateBaseAddressCalledThenStateBaseAddressIsSetCorrectly) {
    using STATE_BASE_ADDRESS = typename FamilyType::STATE_BASE_ADDRESS;
    cmdContainer->dirtyHeaps = 0;

    STATE_BASE_ADDRESS sba;
    auto gmmHelper = cmdContainer->getDevice()->getRootDeviceEnvironment().getGmmHelper();
    uint32_t statelessMocsIndex = (gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1);

    EncodeStateBaseAddressArgs<FamilyType> args = createDefaultEncodeStateBaseAddressArgs<FamilyType>(cmdContainer.get(), sba, statelessMocsIndex);

    EncodeStateBaseAddress<FamilyType>::encode(args);

    auto dsh = cmdContainer->getIndirectHeap(NEO::HeapType::DYNAMIC_STATE);
    auto ssh = cmdContainer->getIndirectHeap(NEO::HeapType::SURFACE_STATE);

    GenCmdList commands;
    CmdParse<FamilyType>::parseCommandBuffer(commands, ptrOffset(cmdContainer->getCommandStream()->getCpuBase(), 0), cmdContainer->getCommandStream()->getUsed());

    auto itorCmd = find<STATE_BASE_ADDRESS *>(commands.begin(), commands.end());
    ASSERT_NE(itorCmd, commands.end());

    auto cmd = genCmdCast<STATE_BASE_ADDRESS *>(*itorCmd);
    if (pDevice->getDeviceInfo().imageSupport) {
        EXPECT_NE(dsh->getHeapGpuBase(), cmd->getDynamicStateBaseAddress());
    } else {
        EXPECT_EQ(dsh, nullptr);
    }
    EXPECT_NE(ssh->getHeapGpuBase(), cmd->getSurfaceStateBaseAddress());
}

HWTEST_F(CommandEncodeStatesTest, givenAnAlignedDstPtrThenNoAlignmentNorOffsetNeeded) {
    uintptr_t ptr = NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignment() << 1;
    size_t offset = 0;
    NEO::EncodeSurfaceState<FamilyType>::getSshAlignedPointer(ptr, offset);
    EXPECT_TRUE((ptr & (NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignment() - 1)) == 0x0u);
    EXPECT_EQ(0u, offset);
}

HWTEST_F(CommandEncodeStatesTest, givenAnUnalignedDstPtrThenCorrectAlignedPtrAndOffsetAreCalculated) {
    uintptr_t ptr = NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignment() >> 1;
    size_t offset = 0;
    NEO::EncodeSurfaceState<FamilyType>::getSshAlignedPointer(ptr, offset);
    EXPECT_TRUE((ptr & (NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignment() - 1)) == 0x0u);
    EXPECT_NE(0u, offset);
}

HWCMDTEST_F(IGFX_GEN8_CORE, CommandEncodeStatesTest, whenAdjustPipelineSelectIsCalledThenNothingHappens) {
    using PIPELINE_SELECT = typename FamilyType::PIPELINE_SELECT;
    auto initialUsed = cmdContainer->getCommandStream()->getUsed();
    NEO::EncodeComputeMode<FamilyType>::adjustPipelineSelect(*cmdContainer, descriptor);
    EXPECT_EQ(initialUsed, cmdContainer->getCommandStream()->getUsed());
}

HWTEST2_F(CommandEncodeStatesTest, whenProgramComputeModeCommandModeIsCalledThenThreadArbitrationPolicyIsProgrammed, IsAtMostGen11) {
    using MI_LOAD_REGISTER_IMM = typename FamilyType::MI_LOAD_REGISTER_IMM;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;
    auto initialUsed = cmdContainer->getCommandStream()->getUsed();
    auto expectedSize = sizeof(MI_LOAD_REGISTER_IMM) + sizeof(PIPE_CONTROL);
    StreamProperties streamProperties{};
    streamProperties.stateComputeMode.threadArbitrationPolicy.value = ThreadArbitrationPolicy::AgeBased;
    streamProperties.stateComputeMode.threadArbitrationPolicy.isDirty = true;
    auto &rootDeviceEnvironment = pDevice->getRootDeviceEnvironment();
    NEO::EncodeComputeMode<FamilyType>::programComputeModeCommand(*cmdContainer->getCommandStream(),
                                                                  streamProperties.stateComputeMode, rootDeviceEnvironment, nullptr);

    if constexpr (TestTraits<gfxCoreFamily>::programComputeModeCommandProgramsThreadArbitrationPolicy) {
        GenCmdList commands;
        CmdParse<FamilyType>::parseCommandBuffer(commands, ptrOffset(cmdContainer->getCommandStream()->getCpuBase(), initialUsed), cmdContainer->getCommandStream()->getUsed());

        auto cmdCount = findAll<MI_LOAD_REGISTER_IMM *>(commands.begin(), commands.end()).size();
        EXPECT_EQ(1u, cmdCount);
        cmdCount = findAll<PIPE_CONTROL *>(commands.begin(), commands.end()).size();
        EXPECT_EQ(1u, cmdCount);
        EXPECT_EQ(initialUsed + expectedSize, cmdContainer->getCommandStream()->getUsed());
    } else {
        EXPECT_EQ(initialUsed, cmdContainer->getCommandStream()->getUsed());
    }
}

HWTEST2_F(CommandEncodeStatesTest, whenProgramComputeModeCommandModeIsCalledThenNonCoherentIsProgrammed, IsAtMostGen11) {
    using MI_LOAD_REGISTER_IMM = typename FamilyType::MI_LOAD_REGISTER_IMM;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;
    auto initialUsed = cmdContainer->getCommandStream()->getUsed();
    [[maybe_unused]] auto expectedSize = sizeof(MI_LOAD_REGISTER_IMM);
    StreamProperties streamProperties{};
    streamProperties.stateComputeMode.threadArbitrationPolicy.value = ThreadArbitrationPolicy::AgeBased;
    streamProperties.stateComputeMode.isCoherencyRequired.isDirty = true;
    auto &rootDeviceEnvironment = pDevice->getRootDeviceEnvironment();
    NEO::EncodeComputeMode<FamilyType>::programComputeModeCommand(*cmdContainer->getCommandStream(),
                                                                  streamProperties.stateComputeMode, rootDeviceEnvironment, nullptr);

    if constexpr (TestTraits<gfxCoreFamily>::programComputeModeCommandProgramsNonCoherent) {
        GenCmdList commands;
        CmdParse<FamilyType>::parseCommandBuffer(commands, ptrOffset(cmdContainer->getCommandStream()->getCpuBase(), initialUsed), cmdContainer->getCommandStream()->getUsed());

        auto cmdCount = findAll<MI_LOAD_REGISTER_IMM *>(commands.begin(), commands.end()).size();
        EXPECT_EQ(1u, cmdCount);
        EXPECT_EQ(initialUsed + expectedSize, cmdContainer->getCommandStream()->getUsed());
    } else {
        EXPECT_EQ(initialUsed, cmdContainer->getCommandStream()->getUsed());
    }
}

HWTEST2_F(CommandEncodeStatesTest, whenGetCmdSizeForComputeModeThenCorrectValueIsReturned, IsAtMostGen11) {
    using MI_LOAD_REGISTER_IMM = typename FamilyType::MI_LOAD_REGISTER_IMM;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;
    auto expectedScmSize = 0u;

    if constexpr (TestTraits<gfxCoreFamily>::programComputeModeCommandProgramsThreadArbitrationPolicy) {
        expectedScmSize += sizeof(MI_LOAD_REGISTER_IMM) + sizeof(PIPE_CONTROL);
    }
    if constexpr (TestTraits<gfxCoreFamily>::programComputeModeCommandProgramsNonCoherent) {
        expectedScmSize += sizeof(MI_LOAD_REGISTER_IMM);
    }
    EXPECT_EQ(expectedScmSize, EncodeComputeMode<FamilyType>::getCmdSizeForComputeMode(this->pDevice->getRootDeviceEnvironment(), false, false));

    UltDeviceFactory deviceFactory{1, 0};
    auto &csr = deviceFactory.rootDevices[0]->getUltCommandStreamReceiver<FamilyType>();
    csr.streamProperties.stateComputeMode.setPropertiesAll(false, 0, ThreadArbitrationPolicy::AgeBased, PreemptionMode::Disabled);
    EXPECT_EQ(expectedScmSize, csr.getCmdSizeForComputeMode());
}

HWTEST2_F(CommandEncodeStatesTest, givenHeapSharingEnabledWhenRetrievingNotInitializedSshThenExpectCorrectSbaCommand, IsAtLeastXeHpCore) {
    using STATE_BASE_ADDRESS = typename FamilyType::STATE_BASE_ADDRESS;
    using _3DSTATE_BINDING_TABLE_POOL_ALLOC = typename FamilyType::_3DSTATE_BINDING_TABLE_POOL_ALLOC;

    cmdContainer->enableHeapSharing();
    cmdContainer->dirtyHeaps = 0;
    cmdContainer->setHeapDirty(NEO::HeapType::SURFACE_STATE);

    auto gmmHelper = cmdContainer->getDevice()->getRootDeviceEnvironment().getGmmHelper();
    uint32_t statelessMocsIndex = (gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1);

    STATE_BASE_ADDRESS sba;
    EncodeStateBaseAddressArgs<FamilyType> args = createDefaultEncodeStateBaseAddressArgs<FamilyType>(cmdContainer.get(), sba, statelessMocsIndex);

    EncodeStateBaseAddress<FamilyType>::encode(args);

    GenCmdList commands;
    CmdParse<FamilyType>::parseCommandBuffer(commands,
                                             cmdContainer->getCommandStream()->getCpuBase(),
                                             cmdContainer->getCommandStream()->getUsed());

    auto itorCmd = find<STATE_BASE_ADDRESS *>(commands.begin(), commands.end());
    ASSERT_NE(commands.end(), itorCmd);
    auto sbaCmd = genCmdCast<STATE_BASE_ADDRESS *>(*itorCmd);

    EXPECT_EQ(0u, sbaCmd->getSurfaceStateBaseAddress());

    itorCmd = find<_3DSTATE_BINDING_TABLE_POOL_ALLOC *>(commands.begin(), commands.end());
    EXPECT_EQ(commands.end(), itorCmd);
}

HWTEST2_F(CommandEncodeStatesTest, givenSbaPropertiesWhenBindingBaseAddressSetThenExpectPropertiesDataDispatched, IsAtLeastXeHpCore) {
    using STATE_BASE_ADDRESS = typename FamilyType::STATE_BASE_ADDRESS;
    using _3DSTATE_BINDING_TABLE_POOL_ALLOC = typename FamilyType::_3DSTATE_BINDING_TABLE_POOL_ALLOC;

    constexpr uint64_t bindingTablePoolBaseAddress = 0x32000;
    constexpr uint32_t bindingTablePoolSize = 0x20;
    constexpr uint64_t surfaceStateBaseAddress = 0x1200;
    constexpr uint32_t surfaceStateSize = 0x10;

    cmdContainer->setHeapDirty(NEO::HeapType::SURFACE_STATE);
    auto gmmHelper = cmdContainer->getDevice()->getRootDeviceEnvironment().getGmmHelper();
    uint32_t statelessMocsIndex = (gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1);

    StateBaseAddressProperties sbaProperties;
    sbaProperties.initSupport(pDevice->getRootDeviceEnvironment());

    STATE_BASE_ADDRESS sba;
    EncodeStateBaseAddressArgs<FamilyType> args = createDefaultEncodeStateBaseAddressArgs<FamilyType>(cmdContainer.get(), sba, statelessMocsIndex);
    args.sbaProperties = &sbaProperties;

    EncodeStateBaseAddress<FamilyType>::encode(args);

    GenCmdList commands;
    CmdParse<FamilyType>::parseCommandBuffer(commands,
                                             cmdContainer->getCommandStream()->getCpuBase(),
                                             cmdContainer->getCommandStream()->getUsed());
    auto itorBindTablePoolCmd = find<_3DSTATE_BINDING_TABLE_POOL_ALLOC *>(commands.begin(), commands.end());
    EXPECT_EQ(commands.end(), itorBindTablePoolCmd);

    sbaProperties.setPropertiesBindingTableSurfaceState(bindingTablePoolBaseAddress, bindingTablePoolSize, surfaceStateBaseAddress, surfaceStateSize);

    EncodeStateBaseAddress<FamilyType>::encode(args);

    commands.clear();
    CmdParse<FamilyType>::parseCommandBuffer(commands,
                                             cmdContainer->getCommandStream()->getCpuBase(),
                                             cmdContainer->getCommandStream()->getUsed());
    itorBindTablePoolCmd = find<_3DSTATE_BINDING_TABLE_POOL_ALLOC *>(commands.begin(), commands.end());
    ASSERT_NE(commands.end(), itorBindTablePoolCmd);

    auto bindTablePoolCmd = reinterpret_cast<_3DSTATE_BINDING_TABLE_POOL_ALLOC *>(*itorBindTablePoolCmd);
    EXPECT_EQ(bindingTablePoolBaseAddress, bindTablePoolCmd->getBindingTablePoolBaseAddress());
    EXPECT_EQ(bindingTablePoolSize, bindTablePoolCmd->getBindingTablePoolBufferSize());
}

HWTEST2_F(CommandEncodeStatesTest, givenSbaPropertiesWhenGeneralBaseAddressSetThenExpectAddressFromPropertiesUsedNotFromContainer, IsAtLeastXeHpCore) {
    using STATE_BASE_ADDRESS = typename FamilyType::STATE_BASE_ADDRESS;

    auto indirectHeapBaseAddress = cmdContainer->getIndirectObjectHeapBaseAddress();
    auto indirectHeapBaseAddressProperties = indirectHeapBaseAddress + 0x10000;

    StateBaseAddressProperties sbaProperties;
    sbaProperties.setPropertiesIndirectState(indirectHeapBaseAddressProperties, MemoryConstants::kiloByte);

    STATE_BASE_ADDRESS sba;
    EncodeStateBaseAddressArgs<FamilyType> args = createDefaultEncodeStateBaseAddressArgs<FamilyType>(cmdContainer.get(), sba, 4);
    args.sbaProperties = &sbaProperties;

    EncodeStateBaseAddress<FamilyType>::encode(args);

    GenCmdList commands;
    CmdParse<FamilyType>::parseCommandBuffer(commands,
                                             cmdContainer->getCommandStream()->getCpuBase(),
                                             cmdContainer->getCommandStream()->getUsed());
    auto itorSbaCmd = find<STATE_BASE_ADDRESS *>(commands.begin(), commands.end());
    ASSERT_NE(commands.end(), itorSbaCmd);

    auto sbaCmd = reinterpret_cast<STATE_BASE_ADDRESS *>(*itorSbaCmd);
    EXPECT_EQ(indirectHeapBaseAddressProperties, sbaCmd->getGeneralStateBaseAddress());
}
