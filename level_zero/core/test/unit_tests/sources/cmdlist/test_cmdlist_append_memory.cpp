/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_container/encode_surface_state.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/memory_manager/internal_allocation_storage.h"
#include "shared/test/common/cmd_parse/gen_cmd_parse.h"
#include "shared/test/common/test_macros/hw_test.h"

#include "level_zero/core/test/unit_tests/fixtures/cmdlist_fixture.inl"
#include "level_zero/core/test/unit_tests/fixtures/device_fixture.h"
#include "level_zero/core/test/unit_tests/mocks/mock_cmdlist.h"
#include "level_zero/core/test/unit_tests/mocks/mock_cmdqueue.h"

namespace L0 {
namespace ult {

using AppendMemoryCopy = Test<DeviceFixture>;

HWTEST2_F(AppendMemoryCopy, givenCommandListAndHostPointersWhenMemoryCopyRegionCalledThenTwoNewAllocationAreAddedToHostMapPtr, IsAtLeastSkl) {
    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);
    ze_copy_region_t dstRegion = {4, 4, 4, 2, 2, 2};
    ze_copy_region_t srcRegion = {4, 4, 4, 2, 2, 2};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);
    EXPECT_EQ(cmdList.hostPtrMap.size(), 2u);
}

HWTEST2_F(AppendMemoryCopy, givenCommandListAndUnalignedHostPointersWhenMemoryCopyRegion2DCalledThenSrcDstPointersArePageAligned, IsAtLeastSkl) {
    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);
    ze_copy_region_t dstRegion = {4, 4, 0, 2, 2, 0};
    ze_copy_region_t srcRegion = {4, 4, 0, 2, 2, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);
    auto sshAlignmentMask = NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignmentMask();
    EXPECT_TRUE(cmdList.srcAlignedPtr == (cmdList.srcAlignedPtr & sshAlignmentMask));
    EXPECT_TRUE(cmdList.dstAlignedPtr == (cmdList.dstAlignedPtr & sshAlignmentMask));
}

HWTEST2_F(AppendMemoryCopy, givenCommandListAndUnalignedHostPointersWhenMemoryCopyRegion3DCalledThenSrcDstPointersArePageAligned, IsAtLeastSkl) {
    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);
    ze_copy_region_t dstRegion = {4, 4, 4, 2, 2, 2};
    ze_copy_region_t srcRegion = {4, 4, 4, 2, 2, 2};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);
    auto sshAlignmentMask = NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignmentMask();
    EXPECT_TRUE(cmdList.srcAlignedPtr == (cmdList.srcAlignedPtr & sshAlignmentMask));
    EXPECT_TRUE(cmdList.dstAlignedPtr == (cmdList.dstAlignedPtr & sshAlignmentMask));
}

HWTEST2_F(AppendMemoryCopy, givenCommandListAndUnalignedHostPointersWhenBlitMemoryCopyRegion2DCalledThenSrcDstNotZeroOffsetsArePassed, IsAtLeastSkl) {
    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1233);
    void *dstPtr = reinterpret_cast<void *>(0x2345);
    ze_copy_region_t dstRegion = {4, 4, 0, 2, 2, 0};
    ze_copy_region_t srcRegion = {4, 4, 0, 2, 2, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);
    EXPECT_GT(cmdList.srcBlitCopyRegionOffset, 0u);
    EXPECT_GT(cmdList.dstBlitCopyRegionOffset, 0u);
}

HWTEST2_F(AppendMemoryCopy, givenCommandListAndUnalignedHostPointersWhenBlitMemoryCopyRegion3DCalledThenSrcDstNotZeroOffsetsArePassed, IsAtLeastSkl) {
    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1233);
    void *dstPtr = reinterpret_cast<void *>(0x2345);
    ze_copy_region_t dstRegion = {4, 4, 4, 2, 2, 2};
    ze_copy_region_t srcRegion = {4, 4, 4, 2, 2, 2};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);
    EXPECT_GT(cmdList.srcBlitCopyRegionOffset, 0u);
    EXPECT_GT(cmdList.dstBlitCopyRegionOffset, 0u);
}

HWTEST2_F(AppendMemoryCopy, givenCommandListAndAlignedHostPointersWhenBlitMemoryCopyRegion3DCalledThenSrcDstZeroOffsetsArePassed, IsAtLeastSkl) {
    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);
    void *srcPtr = alignDown(reinterpret_cast<void *>(0x1233), NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignment());
    void *dstPtr = alignDown(reinterpret_cast<void *>(0x2345), NEO::EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignment());
    ze_copy_region_t dstRegion = {4, 4, 4, 2, 2, 2};
    ze_copy_region_t srcRegion = {4, 4, 4, 2, 2, 2};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);
    EXPECT_EQ(cmdList.srcBlitCopyRegionOffset, 0u);
    EXPECT_EQ(cmdList.dstBlitCopyRegionOffset, 0u);
}

HWTEST2_F(AppendMemoryCopy, givenCopyCommandListAndDestinationPtrOffsetWhenMemoryCopyRegionToSameUsmHostAllocationThenDestinationBlitCopyRegionHasOffset, IsAtLeastSkl) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using XY_COPY_BLT = typename GfxFamily::XY_COPY_BLT;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);

    constexpr size_t allocSize = 4096;
    void *buffer = nullptr;
    ze_host_mem_alloc_desc_t hostDesc = {};
    auto result = context->allocHostMem(&hostDesc, allocSize, allocSize, &buffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    uint32_t offset = 64u;
    void *srcPtr = buffer;
    void *dstPtr = ptrOffset(buffer, offset);
    ze_copy_region_t dstRegion = {0, 0, 0, 8, 4, 0};
    ze_copy_region_t srcRegion = {0, 0, 0, 8, 4, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);

    auto &cmdContainer = cmdList.getCmdContainer();
    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList, ptrOffset(cmdContainer.getCommandStream()->getCpuBase(), 0), cmdContainer.getCommandStream()->getUsed()));
    auto itor = find<XY_COPY_BLT *>(genCmdList.begin(), genCmdList.end());
    ASSERT_NE(genCmdList.end(), itor);

    auto bltCmd = genCmdCast<XY_COPY_BLT *>(*itor);
    EXPECT_EQ(bltCmd->getSourceBaseAddress(), reinterpret_cast<uintptr_t>(srcPtr));
    EXPECT_EQ(bltCmd->getDestinationBaseAddress(), reinterpret_cast<uintptr_t>(dstPtr));

    context->freeMem(buffer);
}

HWTEST2_F(AppendMemoryCopy, givenCopyCommandListAndSourcePtrOffsetWhenMemoryCopyRegionToSameUsmHostAllocationThenSourceBlitCopyRegionHasOffset, IsAtLeastSkl) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using XY_COPY_BLT = typename GfxFamily::XY_COPY_BLT;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);

    constexpr size_t allocSize = 4096;
    void *buffer = nullptr;
    ze_host_mem_alloc_desc_t hostDesc = {};
    auto result = context->allocHostMem(&hostDesc, allocSize, allocSize, &buffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    uint32_t offset = 64u;
    void *srcPtr = ptrOffset(buffer, offset);
    void *dstPtr = buffer;
    ze_copy_region_t dstRegion = {0, 0, 0, 8, 4, 0};
    ze_copy_region_t srcRegion = {0, 0, 0, 8, 4, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);

    auto &cmdContainer = cmdList.getCmdContainer();
    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList, ptrOffset(cmdContainer.getCommandStream()->getCpuBase(), 0), cmdContainer.getCommandStream()->getUsed()));
    auto itor = find<XY_COPY_BLT *>(genCmdList.begin(), genCmdList.end());
    ASSERT_NE(genCmdList.end(), itor);

    auto bltCmd = genCmdCast<XY_COPY_BLT *>(*itor);
    EXPECT_EQ(bltCmd->getSourceBaseAddress(), reinterpret_cast<uintptr_t>(srcPtr));
    EXPECT_EQ(bltCmd->getDestinationBaseAddress(), reinterpret_cast<uintptr_t>(dstPtr));

    context->freeMem(buffer);
}

HWTEST2_F(AppendMemoryCopy, givenCopyCommandListAndDestinationPtrOffsetWhenMemoryCopyRegionToSameUsmSharedAllocationThenDestinationBlitCopyRegionHasOffset, IsAtLeastSkl) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using XY_COPY_BLT = typename GfxFamily::XY_COPY_BLT;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);

    constexpr size_t allocSize = 4096;
    void *buffer = nullptr;
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_device_mem_alloc_desc_t deviceDesc = {};
    auto result = context->allocSharedMem(device->toHandle(), &deviceDesc, &hostDesc, allocSize, allocSize, &buffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    uint32_t offset = 64u;
    void *srcPtr = buffer;
    void *dstPtr = ptrOffset(buffer, offset);
    ze_copy_region_t dstRegion = {0, 0, 0, 8, 4, 0};
    ze_copy_region_t srcRegion = {0, 0, 0, 8, 4, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);

    auto &cmdContainer = cmdList.getCmdContainer();
    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList, ptrOffset(cmdContainer.getCommandStream()->getCpuBase(), 0), cmdContainer.getCommandStream()->getUsed()));
    auto itor = find<XY_COPY_BLT *>(genCmdList.begin(), genCmdList.end());
    ASSERT_NE(genCmdList.end(), itor);

    auto bltCmd = genCmdCast<XY_COPY_BLT *>(*itor);
    EXPECT_EQ(bltCmd->getSourceBaseAddress(), reinterpret_cast<uintptr_t>(srcPtr));
    EXPECT_EQ(bltCmd->getDestinationBaseAddress(), reinterpret_cast<uintptr_t>(dstPtr));

    context->freeMem(buffer);
}

HWTEST2_F(AppendMemoryCopy, givenCopyCommandListAndSourcePtrOffsetWhenMemoryCopyRegionToSameUsmSharedAllocationThenSourceBlitCopyRegionHasOffset, IsAtLeastSkl) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using XY_COPY_BLT = typename GfxFamily::XY_COPY_BLT;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);

    constexpr size_t allocSize = 4096;
    void *buffer = nullptr;
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_device_mem_alloc_desc_t deviceDesc = {};
    auto result = context->allocSharedMem(device->toHandle(), &deviceDesc, &hostDesc, allocSize, allocSize, &buffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    uint32_t offset = 64u;
    void *srcPtr = ptrOffset(buffer, offset);
    void *dstPtr = buffer;
    ze_copy_region_t dstRegion = {0, 0, 0, 8, 4, 0};
    ze_copy_region_t srcRegion = {0, 0, 0, 8, 4, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);

    auto &cmdContainer = cmdList.getCmdContainer();
    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList, ptrOffset(cmdContainer.getCommandStream()->getCpuBase(), 0), cmdContainer.getCommandStream()->getUsed()));
    auto itor = find<XY_COPY_BLT *>(genCmdList.begin(), genCmdList.end());
    ASSERT_NE(genCmdList.end(), itor);

    auto bltCmd = genCmdCast<XY_COPY_BLT *>(*itor);
    EXPECT_EQ(bltCmd->getSourceBaseAddress(), reinterpret_cast<uintptr_t>(srcPtr));
    EXPECT_EQ(bltCmd->getDestinationBaseAddress(), reinterpret_cast<uintptr_t>(dstPtr));

    context->freeMem(buffer);
}

HWTEST2_F(AppendMemoryCopy, givenCopyCommandListAndDestinationPtrOffsetWhenMemoryCopyRegionToSameUsmDeviceAllocationThenDestinationBlitCopyRegionHasOffset, IsAtLeastSkl) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using XY_COPY_BLT = typename GfxFamily::XY_COPY_BLT;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);

    constexpr size_t allocSize = 4096;
    void *buffer = nullptr;
    ze_device_mem_alloc_desc_t deviceDesc = {};
    auto result = context->allocDeviceMem(device->toHandle(), &deviceDesc, allocSize, allocSize, &buffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    uint32_t offset = 64u;
    void *srcPtr = buffer;
    void *dstPtr = ptrOffset(buffer, offset);
    ze_copy_region_t dstRegion = {0, 0, 0, 8, 4, 0};
    ze_copy_region_t srcRegion = {0, 0, 0, 8, 4, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);

    auto &cmdContainer = cmdList.getCmdContainer();
    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList, ptrOffset(cmdContainer.getCommandStream()->getCpuBase(), 0), cmdContainer.getCommandStream()->getUsed()));
    auto itor = find<XY_COPY_BLT *>(genCmdList.begin(), genCmdList.end());
    ASSERT_NE(genCmdList.end(), itor);

    auto bltCmd = genCmdCast<XY_COPY_BLT *>(*itor);
    EXPECT_EQ(bltCmd->getSourceBaseAddress(), reinterpret_cast<uintptr_t>(srcPtr));
    EXPECT_EQ(bltCmd->getDestinationBaseAddress(), reinterpret_cast<uintptr_t>(dstPtr));

    context->freeMem(buffer);
}

HWTEST2_F(AppendMemoryCopy, givenCopyCommandListAndSourcePtrOffsetWhenMemoryCopyRegionToSameUsmDeviceAllocationThenSourceBlitCopyRegionHasOffset, IsAtLeastSkl) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using XY_COPY_BLT = typename GfxFamily::XY_COPY_BLT;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::Copy, 0u);

    constexpr size_t allocSize = 4096;
    void *buffer = nullptr;
    ze_device_mem_alloc_desc_t deviceDesc = {};
    auto result = context->allocDeviceMem(device->toHandle(), &deviceDesc, allocSize, allocSize, &buffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    uint32_t offset = 64u;
    void *srcPtr = ptrOffset(buffer, offset);
    void *dstPtr = buffer;
    ze_copy_region_t dstRegion = {0, 0, 0, 8, 4, 0};
    ze_copy_region_t srcRegion = {0, 0, 0, 8, 4, 0};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);

    auto &cmdContainer = cmdList.getCmdContainer();
    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList, ptrOffset(cmdContainer.getCommandStream()->getCpuBase(), 0), cmdContainer.getCommandStream()->getUsed()));
    auto itor = find<XY_COPY_BLT *>(genCmdList.begin(), genCmdList.end());
    ASSERT_NE(genCmdList.end(), itor);

    auto bltCmd = genCmdCast<XY_COPY_BLT *>(*itor);
    EXPECT_EQ(bltCmd->getSourceBaseAddress(), reinterpret_cast<uintptr_t>(srcPtr));
    EXPECT_EQ(bltCmd->getDestinationBaseAddress(), reinterpret_cast<uintptr_t>(dstPtr));

    context->freeMem(buffer);
}

HWTEST2_F(AppendMemoryCopy, givenCommandListAndHostPointersWhenMemoryCopyRegionCalledThenPipeControlWithDcFlushAdded, IsAtLeastSkl) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);
    ze_copy_region_t dstRegion = {4, 4, 4, 2, 2, 2};
    ze_copy_region_t srcRegion = {4, 4, 4, 2, 2, 2};
    cmdList.appendMemoryCopyRegion(dstPtr, &dstRegion, 0, 0, srcPtr, &srcRegion, 0, 0, nullptr, 0, nullptr, false);

    auto &commandContainer = cmdList.commandContainer;
    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList, ptrOffset(commandContainer.getCommandStream()->getCpuBase(), 0), commandContainer.getCommandStream()->getUsed()));

    auto pc = genCmdCast<PIPE_CONTROL *>(*genCmdList.rbegin());

    if (NEO::MemorySynchronizationCommands<FamilyType>::getDcFlushEnable(true, device->getNEODevice()->getRootDeviceEnvironment())) {
        EXPECT_NE(nullptr, pc);
        EXPECT_TRUE(pc->getDcFlushEnable());
    } else {
        EXPECT_EQ(nullptr, pc);
    }
}

HWTEST2_F(AppendMemoryCopy, givenImmediateCommandListWhenAppendingMemoryCopyThenSuccessIsReturned, IsAtLeastSkl) {
    Mock<CommandQueue> cmdQueue;
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    ASSERT_NE(nullptr, commandList);
    ze_result_t ret = commandList->initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    ASSERT_EQ(ZE_RESULT_SUCCESS, ret);
    commandList->device = device;
    commandList->cmdQImmediate = &cmdQueue;
    commandList->cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    commandList->csr = device->getNEODevice()->getDefaultEngine().commandStreamReceiver;

    auto result = commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(1u, cmdQueue.executeCommandListsCalled);
    EXPECT_EQ(1u, cmdQueue.synchronizeCalled);

    commandList->cmdQImmediate = nullptr;
}

HWTEST2_F(AppendMemoryCopy, givenImmediateCommandListWhenAppendingMemoryCopyWithInvalidEventThenInvalidArgumentErrorIsReturned, IsAtLeastSkl) {
    Mock<CommandQueue> cmdQueue;
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    ASSERT_NE(nullptr, commandList);
    ze_result_t ret = commandList->initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    ASSERT_EQ(ZE_RESULT_SUCCESS, ret);
    commandList->device = device;
    commandList->cmdQImmediate = &cmdQueue;
    commandList->cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    commandList->csr = device->getNEODevice()->getInternalEngine().commandStreamReceiver;

    auto result = commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 1, nullptr, false);
    ASSERT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, result);

    commandList->cmdQImmediate = nullptr;
}

HWTEST2_F(AppendMemoryCopy, givenAsyncImmediateCommandListWhenAppendingMemoryCopyWithCopyEngineThenSuccessIsReturned, IsAtLeastSkl) {
    Mock<CommandQueue> cmdQueue;
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    ASSERT_NE(nullptr, commandList);
    ze_result_t ret = commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    ASSERT_EQ(ZE_RESULT_SUCCESS, ret);
    commandList->device = device;
    commandList->cmdQImmediate = &cmdQueue;
    commandList->cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    commandList->csr = device->getNEODevice()->getDefaultEngine().commandStreamReceiver;

    auto result = commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(1u, cmdQueue.executeCommandListsCalled);
    EXPECT_EQ(0u, cmdQueue.synchronizeCalled);
    EXPECT_EQ(0u, commandList->commandContainer.getResidencyContainer().size());
    commandList->cmdQImmediate = nullptr;
    commandList->csr->getInternalAllocationStorage()->getTemporaryAllocations().freeAllGraphicsAllocations(device->getNEODevice());
}

HWTEST2_F(AppendMemoryCopy, givenAsyncImmediateCommandListWhenAppendingMemoryCopyWithCopyEngineThenProgramCmdStreamWithFlushTask, IsAtLeastSkl) {
    using MI_BATCH_BUFFER_START = typename FamilyType::MI_BATCH_BUFFER_START;
    using MI_FLUSH_DW = typename FamilyType::MI_FLUSH_DW;

    DebugManagerStateRestore restore;
    NEO::DebugManager.flags.EnableFlushTaskSubmission.set(1);
    auto ultCsr = static_cast<UltCommandStreamReceiver<FamilyType> *>(device->getNEODevice()->getDefaultEngine().commandStreamReceiver);
    ultCsr->storeMakeResidentAllocations = true;

    auto cmdQueue = std::make_unique<Mock<CommandQueue>>();
    cmdQueue->csr = ultCsr;
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    ASSERT_NE(nullptr, commandList);
    commandList->isFlushTaskSubmissionEnabled = true;
    ze_result_t ret = commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    ASSERT_EQ(ZE_RESULT_SUCCESS, ret);
    commandList->device = device;
    commandList->isSyncModeQueue = false;
    commandList->cmdQImmediate = cmdQueue.get();
    commandList->cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    commandList->csr = ultCsr;

    // Program CSR state on first submit

    EXPECT_EQ(0u, ultCsr->getCS(0).getUsed());

    bool hwContextProgrammingRequired = (ultCsr->getCmdsSizeForHardwareContext() > 0);

    size_t expectedSize = 0;
    if (hwContextProgrammingRequired) {
        expectedSize = alignUp(ultCsr->getCmdsSizeForHardwareContext() + sizeof(typename FamilyType::MI_BATCH_BUFFER_START), MemoryConstants::cacheLineSize);
    }

    ASSERT_EQ(ZE_RESULT_SUCCESS, commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false));

    EXPECT_EQ(expectedSize, ultCsr->getCS(0).getUsed());
    EXPECT_TRUE(ultCsr->isMadeResident(commandList->commandContainer.getCommandStream()->getGraphicsAllocation()));

    size_t offset = 0;
    if constexpr (FamilyType::isUsingMiMemFence) {
        if (ultCsr->globalFenceAllocation) {
            using STATE_SYSTEM_MEM_FENCE_ADDRESS = typename FamilyType::STATE_SYSTEM_MEM_FENCE_ADDRESS;
            auto sysMemFence = genCmdCast<STATE_SYSTEM_MEM_FENCE_ADDRESS *>(ultCsr->getCS(0).getCpuBase());
            ASSERT_NE(nullptr, sysMemFence);
            EXPECT_EQ(ultCsr->globalFenceAllocation->getGpuAddress(), sysMemFence->getSystemMemoryFenceAddress());
            offset += sizeof(STATE_SYSTEM_MEM_FENCE_ADDRESS);
        }
    }

    if (hwContextProgrammingRequired) {
        auto bbStartCmd = genCmdCast<MI_BATCH_BUFFER_START *>(ptrOffset(ultCsr->getCS(0).getCpuBase(), offset));
        ASSERT_NE(nullptr, bbStartCmd);

        EXPECT_EQ(commandList->commandContainer.getCommandStream()->getGpuBase(), bbStartCmd->getBatchBufferStartAddress());
    }

    auto findTagUpdate = [](void *streamBase, size_t sizeUsed, uint64_t tagAddress) -> bool {
        GenCmdList genCmdList;
        EXPECT_TRUE(FamilyType::PARSE::parseCommandBuffer(genCmdList, streamBase, sizeUsed));

        auto itor = find<MI_FLUSH_DW *>(genCmdList.begin(), genCmdList.end());
        bool found = false;

        while (itor != genCmdList.end()) {
            auto cmd = genCmdCast<MI_FLUSH_DW *>(*itor);
            if (cmd && cmd->getDestinationAddress() == tagAddress) {
                found = true;
                break;
            }
            itor++;
        }

        return found;
    };

    EXPECT_FALSE(findTagUpdate(commandList->commandContainer.getCommandStream()->getCpuBase(),
                               commandList->commandContainer.getCommandStream()->getUsed(),
                               ultCsr->getTagAllocation()->getGpuAddress()));

    // Dont program CSR state on next submit
    size_t csrOfffset = ultCsr->getCS(0).getUsed();
    size_t cmdListOffset = commandList->commandContainer.getCommandStream()->getUsed();

    ASSERT_EQ(ZE_RESULT_SUCCESS, commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false));

    EXPECT_EQ(csrOfffset, ultCsr->getCS(0).getUsed());

    EXPECT_FALSE(findTagUpdate(ptrOffset(commandList->commandContainer.getCommandStream()->getCpuBase(), cmdListOffset),
                               commandList->commandContainer.getCommandStream()->getUsed() - cmdListOffset,
                               ultCsr->getTagAllocation()->getGpuAddress()));
}

HWTEST2_F(AppendMemoryCopy, givenSyncImmediateCommandListWhenAppendingMemoryCopyWithCopyEngineThenProgramCmdStreamWithFlushTask, IsAtLeastSkl) {
    using MI_BATCH_BUFFER_START = typename FamilyType::MI_BATCH_BUFFER_START;
    using MI_FLUSH_DW = typename FamilyType::MI_FLUSH_DW;

    DebugManagerStateRestore restore;
    NEO::DebugManager.flags.EnableFlushTaskSubmission.set(1);
    auto ultCsr = static_cast<UltCommandStreamReceiver<FamilyType> *>(device->getNEODevice()->getDefaultEngine().commandStreamReceiver);
    ultCsr->storeMakeResidentAllocations = true;

    auto cmdQueue = std::make_unique<Mock<CommandQueue>>();
    cmdQueue->csr = ultCsr;
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    ASSERT_NE(nullptr, commandList);
    commandList->isFlushTaskSubmissionEnabled = true;
    ze_result_t ret = commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    ASSERT_EQ(ZE_RESULT_SUCCESS, ret);
    commandList->device = device;
    commandList->isSyncModeQueue = true;
    commandList->cmdQImmediate = cmdQueue.get();
    commandList->cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    commandList->csr = ultCsr;

    // Program CSR state on first submit

    EXPECT_EQ(0u, ultCsr->getCS(0).getUsed());

    bool hwContextProgrammingRequired = (ultCsr->getCmdsSizeForHardwareContext() > 0);

    size_t expectedSize = 0;
    if (hwContextProgrammingRequired) {
        expectedSize = alignUp(ultCsr->getCmdsSizeForHardwareContext() + sizeof(typename FamilyType::MI_BATCH_BUFFER_START), MemoryConstants::cacheLineSize);
    }

    ASSERT_EQ(ZE_RESULT_SUCCESS, commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false));

    EXPECT_EQ(expectedSize, ultCsr->getCS(0).getUsed());
    EXPECT_TRUE(ultCsr->isMadeResident(commandList->commandContainer.getCommandStream()->getGraphicsAllocation()));

    size_t offset = 0;
    if constexpr (FamilyType::isUsingMiMemFence) {
        if (ultCsr->globalFenceAllocation) {
            using STATE_SYSTEM_MEM_FENCE_ADDRESS = typename FamilyType::STATE_SYSTEM_MEM_FENCE_ADDRESS;
            auto sysMemFence = genCmdCast<STATE_SYSTEM_MEM_FENCE_ADDRESS *>(ultCsr->getCS(0).getCpuBase());
            ASSERT_NE(nullptr, sysMemFence);
            EXPECT_EQ(ultCsr->globalFenceAllocation->getGpuAddress(), sysMemFence->getSystemMemoryFenceAddress());
            offset += sizeof(STATE_SYSTEM_MEM_FENCE_ADDRESS);
        }
    }

    if (hwContextProgrammingRequired) {
        auto bbStartCmd = genCmdCast<MI_BATCH_BUFFER_START *>(ptrOffset(ultCsr->getCS(0).getCpuBase(), offset));
        ASSERT_NE(nullptr, bbStartCmd);
        EXPECT_EQ(commandList->commandContainer.getCommandStream()->getGpuBase(), bbStartCmd->getBatchBufferStartAddress());
    }

    auto findTagUpdate = [](void *streamBase, size_t sizeUsed, uint64_t tagAddress) -> bool {
        GenCmdList genCmdList;
        EXPECT_TRUE(FamilyType::PARSE::parseCommandBuffer(genCmdList, streamBase, sizeUsed));

        auto itor = find<MI_FLUSH_DW *>(genCmdList.begin(), genCmdList.end());
        bool found = false;

        while (itor != genCmdList.end()) {
            auto cmd = genCmdCast<MI_FLUSH_DW *>(*itor);
            if (cmd && cmd->getDestinationAddress() == tagAddress) {
                found = true;
                break;
            }
            itor++;
        }

        return found;
    };

    EXPECT_TRUE(findTagUpdate(commandList->commandContainer.getCommandStream()->getCpuBase(),
                              commandList->commandContainer.getCommandStream()->getUsed(),
                              ultCsr->getTagAllocation()->getGpuAddress()));

    // Dont program CSR state on next submit
    size_t csrOfffset = ultCsr->getCS(0).getUsed();
    size_t cmdListOffset = commandList->commandContainer.getCommandStream()->getUsed();

    ASSERT_EQ(ZE_RESULT_SUCCESS, commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false));

    EXPECT_EQ(csrOfffset, ultCsr->getCS(0).getUsed());

    EXPECT_TRUE(findTagUpdate(ptrOffset(commandList->commandContainer.getCommandStream()->getCpuBase(), cmdListOffset),
                              commandList->commandContainer.getCommandStream()->getUsed() - cmdListOffset,
                              ultCsr->getTagAllocation()->getGpuAddress()));
}

HWTEST2_F(AppendMemoryCopy, givenSyncModeImmediateCommandListWhenAppendingMemoryCopyWithCopyEngineThenSuccessIsReturned, IsAtLeastSkl) {
    Mock<CommandQueue> cmdQueue;
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    ASSERT_NE(nullptr, commandList);
    ze_result_t ret = commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    ASSERT_EQ(ZE_RESULT_SUCCESS, ret);
    commandList->device = device;
    commandList->cmdQImmediate = &cmdQueue;
    commandList->cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    commandList->isSyncModeQueue = true;
    commandList->csr = device->getNEODevice()->getDefaultEngine().commandStreamReceiver;

    auto result = commandList->appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(1u, cmdQueue.executeCommandListsCalled);
    EXPECT_EQ(1u, cmdQueue.synchronizeCalled);

    commandList->cmdQImmediate = nullptr;
    commandList->csr->getInternalAllocationStorage()->getTemporaryAllocations().freeAllGraphicsAllocations(device->getNEODevice());
}

HWTEST2_F(AppendMemoryCopy, givenCommandListAndHostPointersWhenMemoryCopyCalledThenPipeControlWithDcFlushAdded, IsAtLeastSkl) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    MockAppendMemoryCopy<gfxCoreFamily> cmdList;
    cmdList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    auto &commandContainer = cmdList.commandContainer;

    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    cmdList.appendMemoryCopy(dstPtr, srcPtr, 8, nullptr, 0, nullptr, false);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();

    GenCmdList genCmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        genCmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));
    auto itor = find<PIPE_CONTROL *>(genCmdList.begin(), genCmdList.end());
    PIPE_CONTROL *cmd = nullptr;
    uint32_t dcFlushPipeControl = 0;
    while (itor != genCmdList.end()) {
        cmd = genCmdCast<PIPE_CONTROL *>(*itor);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControl++;
        }
        itor++;
    }
    uint32_t expectedDcFlushPipeControl =
        NEO::MemorySynchronizationCommands<FamilyType>::getDcFlushEnable(true, device->getNEODevice()->getRootDeviceEnvironment()) ? 1 : 0;
    EXPECT_EQ(expectedDcFlushPipeControl, dcFlushPipeControl);
}

HWTEST2_F(AppendMemoryCopy, givenCopyCommandListWhenTimestampPassedToMemoryCopyThenAppendProfilingCalledOnceBeforeAndAfterCommand, IsAtLeastSkl) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using MI_STORE_REGISTER_MEM = typename GfxFamily::MI_STORE_REGISTER_MEM;
    using MI_FLUSH_DW = typename GfxFamily::MI_FLUSH_DW;

    MockAppendMemoryCopy<gfxCoreFamily> commandList;
    commandList.initialize(device, NEO::EngineGroupType::Copy, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);
    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;
    eventPoolDesc.count = 1;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.signal = 0;
    eventDesc.wait = 0;

    ze_result_t result = ZE_RESULT_SUCCESS;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));

    commandList.appendMemoryCopy(dstPtr, srcPtr, 0x100, event->toHandle(), 0, nullptr, false);
    EXPECT_EQ(commandList.appendMemoryCopyBlitCalled, 1u);
    EXPECT_EQ(1u, event->getPacketsInUse());

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList.commandContainer.getCommandStream()->getCpuBase(), 0), commandList.commandContainer.getCommandStream()->getUsed()));
    auto itor = find<MI_STORE_REGISTER_MEM *>(cmdList.begin(), cmdList.end());
    EXPECT_NE(cmdList.end(), itor);
    auto cmd = genCmdCast<MI_STORE_REGISTER_MEM *>(*itor);
    EXPECT_EQ(cmd->getRegisterAddress(), REG_GLOBAL_TIMESTAMP_LDW);
    itor++;
    EXPECT_NE(cmdList.end(), itor);
    cmd = genCmdCast<MI_STORE_REGISTER_MEM *>(*itor);
    EXPECT_EQ(cmd->getRegisterAddress(), GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW);

    itor = find<MI_FLUSH_DW *>(itor, cmdList.end());
    EXPECT_NE(cmdList.end(), itor);

    itor = find<MI_STORE_REGISTER_MEM *>(itor, cmdList.end());
    EXPECT_NE(cmdList.end(), itor);
    cmd = genCmdCast<MI_STORE_REGISTER_MEM *>(*itor);
    EXPECT_EQ(cmd->getRegisterAddress(), REG_GLOBAL_TIMESTAMP_LDW);
    itor++;
    EXPECT_NE(cmdList.end(), itor);
    cmd = genCmdCast<MI_STORE_REGISTER_MEM *>(*itor);
    EXPECT_EQ(cmd->getRegisterAddress(), GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW);
    itor++;
    EXPECT_EQ(cmdList.end(), itor);
}

using SupportedPlatforms = IsWithinProducts<IGFX_SKYLAKE, IGFX_DG1>;
HWTEST2_F(AppendMemoryCopy,
          givenCommandListUsesTimestampPassedToMemoryCopyWhenTwoKernelsAreUsedThenAppendProfilingCalledForSinglePacket, SupportedPlatforms) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using GPGPU_WALKER = typename GfxFamily::GPGPU_WALKER;

    MockAppendMemoryCopy<gfxCoreFamily> commandList;
    commandList.appendMemoryCopyKernelWithGACallBase = true;

    commandList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1234);
    void *dstPtr = reinterpret_cast<void *>(0x2345);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;

    ze_result_t result = ZE_RESULT_SUCCESS;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));

    uint64_t globalStartAddress = event->getGpuAddress(device) + event->getGlobalStartOffset();
    uint64_t contextStartAddress = event->getGpuAddress(device) + event->getContextStartOffset();
    uint64_t globalEndAddress = event->getGpuAddress(device) + event->getGlobalEndOffset();
    uint64_t contextEndAddress = event->getGpuAddress(device) + event->getContextEndOffset();

    commandList.appendMemoryCopy(dstPtr, srcPtr, 0x100, event->toHandle(), 0, nullptr, false);
    EXPECT_EQ(2u, commandList.appendMemoryCopyKernelWithGACalled);
    EXPECT_EQ(0u, commandList.appendMemoryCopyBlitCalled);
    EXPECT_EQ(1u, event->getPacketsInUse());
    EXPECT_EQ(1u, event->getKernelCount());

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList.commandContainer.getCommandStream()->getCpuBase(), 0),
        commandList.commandContainer.getCommandStream()->getUsed()));

    auto itorWalkers = findAll<GPGPU_WALKER *>(cmdList.begin(), cmdList.end());
    auto begin = cmdList.begin();
    ASSERT_EQ(2u, itorWalkers.size());
    auto secondWalker = itorWalkers[1];

    validateTimestampRegisters<FamilyType>(cmdList,
                                           begin,
                                           REG_GLOBAL_TIMESTAMP_LDW, globalStartAddress,
                                           GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, contextStartAddress,
                                           false);

    validateTimestampRegisters<FamilyType>(cmdList,
                                           secondWalker,
                                           REG_GLOBAL_TIMESTAMP_LDW, globalEndAddress,
                                           GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, contextEndAddress,
                                           false);
}

HWTEST2_F(AppendMemoryCopy,
          givenCommandListUsesTimestampPassedToMemoryCopyWhenThreeKernelsAreUsedThenAppendProfilingCalledForSinglePacket, SupportedPlatforms) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using GPGPU_WALKER = typename GfxFamily::GPGPU_WALKER;

    MockAppendMemoryCopy<gfxCoreFamily> commandList;
    commandList.appendMemoryCopyKernelWithGACallBase = true;

    commandList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    void *srcPtr = reinterpret_cast<void *>(0x1231);
    void *dstPtr = reinterpret_cast<void *>(0x200002345);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;

    ze_result_t result = ZE_RESULT_SUCCESS;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));

    uint64_t globalStartAddress = event->getGpuAddress(device) + event->getGlobalStartOffset();
    uint64_t contextStartAddress = event->getGpuAddress(device) + event->getContextStartOffset();
    uint64_t globalEndAddress = event->getGpuAddress(device) + event->getGlobalEndOffset();
    uint64_t contextEndAddress = event->getGpuAddress(device) + event->getContextEndOffset();

    commandList.appendMemoryCopy(dstPtr, srcPtr, 0x100002345, event->toHandle(), 0, nullptr, false);
    EXPECT_EQ(3u, commandList.appendMemoryCopyKernelWithGACalled);
    EXPECT_EQ(0u, commandList.appendMemoryCopyBlitCalled);
    EXPECT_EQ(1u, event->getPacketsInUse());
    EXPECT_EQ(1u, event->getKernelCount());

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList.commandContainer.getCommandStream()->getCpuBase(), 0),
        commandList.commandContainer.getCommandStream()->getUsed()));

    auto itorWalkers = findAll<GPGPU_WALKER *>(cmdList.begin(), cmdList.end());
    auto begin = cmdList.begin();
    ASSERT_EQ(3u, itorWalkers.size());
    auto thirdWalker = itorWalkers[2];

    validateTimestampRegisters<FamilyType>(cmdList,
                                           begin,
                                           REG_GLOBAL_TIMESTAMP_LDW, globalStartAddress,
                                           GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, contextStartAddress,
                                           false);

    validateTimestampRegisters<FamilyType>(cmdList,
                                           thirdWalker,
                                           REG_GLOBAL_TIMESTAMP_LDW, globalEndAddress,
                                           GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, contextEndAddress,
                                           false);
}

} // namespace ult
} // namespace L0
