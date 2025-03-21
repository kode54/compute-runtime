/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_container/command_encoder.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/helpers/preamble.h"
#include "shared/source/helpers/register_offsets.h"
#include "shared/source/indirect_heap/indirect_heap.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/test/common/cmd_parse/gen_cmd_parse.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/helpers/unit_test_helper.h"
#include "shared/test/common/libult/ult_command_stream_receiver.h"
#include "shared/test/common/test_macros/hw_test.h"

#include "level_zero/core/source/cmdlist/cmdlist_hw_immediate.h"
#include "level_zero/core/source/event/event.h"
#include "level_zero/core/test/unit_tests/fixtures/cmdlist_fixture.h"
#include "level_zero/core/test/unit_tests/fixtures/module_fixture.h"
#include "level_zero/core/test/unit_tests/mocks/mock_cmdlist.h"
#include "level_zero/core/test/unit_tests/mocks/mock_cmdqueue.h"
#include "level_zero/core/test/unit_tests/mocks/mock_module.h"

namespace L0 {
namespace ult {

using CommandListAppendLaunchKernelMockModule = Test<ModuleMutableCommandListFixture>;
HWTEST_F(CommandListAppendLaunchKernelMockModule, givenKernelWithIndirectAllocationsAllowedThenCommandListReturnsExpectedIndirectAllocationsAllowed) {
    DebugManagerStateRestore restorer;
    NEO::DebugManager.flags.DetectIndirectAccessInKernel.set(1);
    mockKernelImmData->kernelDescriptor->kernelAttributes.hasIndirectStatelessAccess = true;
    kernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed = false;
    kernel->unifiedMemoryControls.indirectSharedAllocationsAllowed = false;
    kernel->unifiedMemoryControls.indirectHostAllocationsAllowed = true;

    EXPECT_TRUE(kernel->hasIndirectAllocationsAllowed());

    ze_group_count_t groupCount{1, 1, 1};
    ze_result_t returnValue;
    CmdListKernelLaunchParams launchParams = {};
    {
        returnValue = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
        ASSERT_EQ(ZE_RESULT_SUCCESS, returnValue);
        EXPECT_TRUE(commandList->hasIndirectAllocationsAllowed());
    }

    {
        returnValue = commandList->reset();
        ASSERT_EQ(ZE_RESULT_SUCCESS, returnValue);
        kernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed = false;
        kernel->unifiedMemoryControls.indirectSharedAllocationsAllowed = true;
        kernel->unifiedMemoryControls.indirectHostAllocationsAllowed = false;

        returnValue = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
        ASSERT_EQ(ZE_RESULT_SUCCESS, returnValue);
        EXPECT_TRUE(commandList->hasIndirectAllocationsAllowed());
    }

    {
        returnValue = commandList->reset();
        ASSERT_EQ(ZE_RESULT_SUCCESS, returnValue);
        kernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed = true;
        kernel->unifiedMemoryControls.indirectSharedAllocationsAllowed = false;
        kernel->unifiedMemoryControls.indirectHostAllocationsAllowed = false;

        returnValue = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
        ASSERT_EQ(ZE_RESULT_SUCCESS, returnValue);
        EXPECT_TRUE(commandList->hasIndirectAllocationsAllowed());
    }
}

using CommandListAppendLaunchKernel = Test<ModuleFixture>;
HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithIndirectAllocationsNotAllowedThenCommandListReturnsExpectedIndirectAllocationsAllowed) {
    createKernel();
    kernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed = false;
    kernel->unifiedMemoryControls.indirectSharedAllocationsAllowed = false;
    kernel->unifiedMemoryControls.indirectHostAllocationsAllowed = false;

    ze_group_count_t groupCount{1, 1, 1};
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);

    ASSERT_EQ(ZE_RESULT_SUCCESS, result);
    ASSERT_FALSE(commandList->hasIndirectAllocationsAllowed());
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithOldestFirstThreadArbitrationPolicySetUsingSchedulingHintExtensionThenCorrectInternalPolicyIsReturned) {
    createKernel();
    ze_scheduling_hint_exp_desc_t pHint{};
    pHint.flags = ZE_SCHEDULING_HINT_EXP_FLAG_OLDEST_FIRST;
    kernel->setSchedulingHintExp(&pHint);
    ASSERT_EQ(kernel->getKernelDescriptor().kernelAttributes.threadArbitrationPolicy, NEO::ThreadArbitrationPolicy::AgeBased);
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithRRThreadArbitrationPolicySetUsingSchedulingHintExtensionThenCorrectInternalPolicyIsReturned) {
    createKernel();
    ze_scheduling_hint_exp_desc_t pHint{};
    pHint.flags = ZE_SCHEDULING_HINT_EXP_FLAG_ROUND_ROBIN;
    kernel->setSchedulingHintExp(&pHint);
    ASSERT_EQ(kernel->getKernelDescriptor().kernelAttributes.threadArbitrationPolicy, NEO::ThreadArbitrationPolicy::RoundRobin);
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithStallRRThreadArbitrationPolicySetUsingSchedulingHintExtensionThenCorrectInternalPolicyIsReturned) {
    createKernel();
    ze_scheduling_hint_exp_desc_t pHint{};
    pHint.flags = ZE_SCHEDULING_HINT_EXP_FLAG_STALL_BASED_ROUND_ROBIN;
    kernel->setSchedulingHintExp(&pHint);
    ASSERT_EQ(kernel->getKernelDescriptor().kernelAttributes.threadArbitrationPolicy, NEO::ThreadArbitrationPolicy::RoundRobinAfterDependency);
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithThreadArbitrationPolicySetUsingSchedulingHintExtensionTheSameFlagIsUsedToSetCmdListThreadArbitrationPolicy) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.ForceThreadArbitrationPolicyProgrammingWithScm.set(1);

    createKernel();
    ze_scheduling_hint_exp_desc_t *pHint = new ze_scheduling_hint_exp_desc_t;
    pHint->pNext = nullptr;
    pHint->flags = ZE_SCHEDULING_HINT_EXP_FLAG_ROUND_ROBIN;
    kernel->setSchedulingHintExp(pHint);

    ze_group_count_t groupCount{1, 1, 1};
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);

    ASSERT_EQ(ZE_RESULT_SUCCESS, result);
    ASSERT_EQ(NEO::ThreadArbitrationPolicy::RoundRobin, commandList->getFinalStreamState().stateComputeMode.threadArbitrationPolicy.value);
    delete (pHint);
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithThreadArbitrationPolicySetUsingSchedulingHintExtensionAndOverrideThreadArbitrationPolicyThenTheLatterIsUsedToSetCmdListThreadArbitrationPolicy) {
    createKernel();
    ze_scheduling_hint_exp_desc_t *pHint = new ze_scheduling_hint_exp_desc_t;
    pHint->pNext = nullptr;
    pHint->flags = ZE_SCHEDULING_HINT_EXP_FLAG_ROUND_ROBIN;
    kernel->setSchedulingHintExp(pHint);

    DebugManagerStateRestore restorer;
    DebugManager.flags.OverrideThreadArbitrationPolicy.set(0);
    DebugManager.flags.ForceThreadArbitrationPolicyProgrammingWithScm.set(1);

    ze_group_count_t groupCount{1, 1, 1};
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);

    ASSERT_EQ(ZE_RESULT_SUCCESS, result);
    ASSERT_EQ(NEO::ThreadArbitrationPolicy::AgeBased, commandList->getFinalStreamState().stateComputeMode.threadArbitrationPolicy.value);
    delete (pHint);
}

HWTEST2_F(CommandListAppendLaunchKernel, givenNotEnoughSpaceInCommandStreamWhenAppendingKernelThenBbEndIsAddedAndNewCmdBufferAllocated, IsAtLeastSkl) {
    using MI_BATCH_BUFFER_END = typename FamilyType::MI_BATCH_BUFFER_END;

    DebugManagerStateRestore restorer;
    DebugManager.flags.DispatchCmdlistCmdBufferPrimary.set(0);

    createKernel();

    ze_result_t returnValue;
    std::unique_ptr<L0::ult::CommandList> commandList(whiteboxCast(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue)));

    auto &commandContainer = commandList->getCmdContainer();
    const auto stream = commandContainer.getCommandStream();
    const auto streamCpu = stream->getCpuBase();

    Vec3<size_t> groupCount{1, 1, 1};
    auto sizeLeftInStream = sizeof(MI_BATCH_BUFFER_END);
    auto available = stream->getAvailableSpace();
    stream->getSpace(available - sizeLeftInStream);
    auto bbEndPosition = stream->getSpace(0);

    const uint32_t threadGroupDimensions[3] = {1, 1, 1};

    NEO::EncodeDispatchKernelArgs dispatchKernelArgs{
        0,
        device->getNEODevice(),
        kernel.get(),
        nullptr,
        nullptr,
        threadGroupDimensions,
        nullptr,
        PreemptionMode::MidBatch,
        0,
        0,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        commandList->getDcFlushRequired(true)};
    NEO::EncodeDispatchKernel<FamilyType>::encode(commandContainer, dispatchKernelArgs, commandList->getLogicalStateHelper());

    auto usedSpaceAfter = commandContainer.getCommandStream()->getUsed();
    ASSERT_GT(usedSpaceAfter, 0u);

    const auto streamCpu2 = stream->getCpuBase();

    EXPECT_NE(nullptr, streamCpu2);
    EXPECT_NE(streamCpu, streamCpu2);

    EXPECT_EQ(2u, commandContainer.getCmdBufferAllocations().size());

    GenCmdList cmdList;
    FamilyType::PARSE::parseCommandBuffer(cmdList, bbEndPosition, 2 * sizeof(MI_BATCH_BUFFER_END));
    auto itor = find<MI_BATCH_BUFFER_END *>(cmdList.begin(), cmdList.end());
    EXPECT_NE(cmdList.end(), itor);
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithPrintfUsedWhenAppendedToCommandListThenKernelIsStored) {
    createKernel();
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    ze_group_count_t groupCount{1, 1, 1};

    EXPECT_TRUE(kernel->kernelImmData->getDescriptor().kernelAttributes.flags.usesPrintf);
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(1u, commandList->getPrintfKernelContainer().size());
    EXPECT_EQ(kernel.get(), commandList->getPrintfKernelContainer()[0]);
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithPrintfUsedWhenAppendedToCommandListMultipleTimesThenKernelIsStoredOnce) {
    createKernel();
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    ze_group_count_t groupCount{1, 1, 1};

    EXPECT_TRUE(kernel->kernelImmData->getDescriptor().kernelAttributes.flags.usesPrintf);
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(1u, commandList->getPrintfKernelContainer().size());
    EXPECT_EQ(kernel.get(), commandList->getPrintfKernelContainer()[0]);

    result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(1u, commandList->getPrintfKernelContainer().size());
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithPrintfWhenAppendedToSynchronousImmCommandListThenPrintfBufferIsPrinted) {
    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.EnableFlushTaskSubmission.set(1);

    ze_result_t returnValue;
    ze_command_queue_desc_t queueDesc = {};
    queueDesc.mode = ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS;

    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily, device, &queueDesc, false, NEO::EngineGroupType::RenderCompute, returnValue));

    Mock<Kernel> kernel;
    kernel.descriptor.kernelAttributes.flags.usesPrintf = true;

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(1u, kernel.printPrintfOutputCalledTimes);
    EXPECT_FALSE(kernel.hangDetectedPassedToPrintfOutput);
    EXPECT_EQ(0u, commandList->getPrintfKernelContainer().size());

    result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(2u, kernel.printPrintfOutputCalledTimes);
    EXPECT_FALSE(kernel.hangDetectedPassedToPrintfOutput);
    EXPECT_EQ(0u, commandList->getPrintfKernelContainer().size());
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithPrintfWhenAppendedToAsynchronousImmCommandListThenPrintfBufferIsPrinted) {
    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.EnableFlushTaskSubmission.set(1);

    ze_result_t returnValue;
    ze_command_queue_desc_t queueDesc = {};
    queueDesc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;

    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily, device, &queueDesc, false, NEO::EngineGroupType::RenderCompute, returnValue));

    Mock<Kernel> kernel;
    kernel.descriptor.kernelAttributes.flags.usesPrintf = true;

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(1u, kernel.printPrintfOutputCalledTimes);
    EXPECT_FALSE(kernel.hangDetectedPassedToPrintfOutput);
    EXPECT_EQ(0u, commandList->getPrintfKernelContainer().size());

    result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(2u, kernel.printPrintfOutputCalledTimes);
    EXPECT_FALSE(kernel.hangDetectedPassedToPrintfOutput);
    EXPECT_EQ(0u, commandList->getPrintfKernelContainer().size());
}

HWTEST_F(CommandListAppendLaunchKernel, givenKernelWithPrintfWhenAppendToSynchronousImmCommandListHangsThenPrintfBufferIsPrinted) {
    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.EnableFlushTaskSubmission.set(1);

    ze_result_t returnValue;
    ze_command_queue_desc_t queueDesc = {};
    queueDesc.mode = ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS;
    TaskCountType currentTaskCount = 33u;
    auto &csr = neoDevice->getUltCommandStreamReceiver<FamilyType>();
    csr.latestWaitForCompletionWithTimeoutTaskCount = currentTaskCount;
    csr.callBaseWaitForCompletionWithTimeout = false;
    csr.returnWaitForCompletionWithTimeout = WaitStatus::GpuHang;

    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily, device, &queueDesc, false, NEO::EngineGroupType::RenderCompute, returnValue));

    Mock<Kernel> kernel;
    kernel.descriptor.kernelAttributes.flags.usesPrintf = true;

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_ERROR_DEVICE_LOST, result);
    EXPECT_EQ(1u, kernel.printPrintfOutputCalledTimes);
    EXPECT_TRUE(kernel.hangDetectedPassedToPrintfOutput);
    EXPECT_EQ(0u, commandList->getPrintfKernelContainer().size());

    result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_ERROR_DEVICE_LOST, result);
    EXPECT_EQ(2u, kernel.printPrintfOutputCalledTimes);
    EXPECT_TRUE(kernel.hangDetectedPassedToPrintfOutput);
    EXPECT_EQ(0u, commandList->getPrintfKernelContainer().size());
}

HWTEST_F(CommandListAppendLaunchKernel, WhenAppendingMultipleTimesThenSshIsNotDepletedButReallocated) {
    createKernel();
    ze_result_t returnValue;

    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.UseBindlessMode.set(0);

    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    ze_group_count_t groupCount{1, 1, 1};

    auto kernelSshSize = kernel->getSurfaceStateHeapDataSize();
    auto ssh = commandList->getCmdContainer().getIndirectHeap(NEO::HeapType::SURFACE_STATE);
    auto sshHeapSize = ssh->getMaxAvailableSpace();
    auto initialAllocation = ssh->getGraphicsAllocation();
    EXPECT_NE(nullptr, initialAllocation);
    const_cast<KernelDescriptor::AddressingMode &>(kernel->getKernelDescriptor().kernelAttributes.bufferAddressingMode) = KernelDescriptor::BindfulAndStateless;
    CmdListKernelLaunchParams launchParams = {};
    for (size_t i = 0; i < sshHeapSize / kernelSshSize + 1; i++) {
        auto result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
        ASSERT_EQ(ZE_RESULT_SUCCESS, result);
    }

    auto reallocatedAllocation = ssh->getGraphicsAllocation();
    EXPECT_NE(nullptr, reallocatedAllocation);
    EXPECT_NE(initialAllocation, reallocatedAllocation);
}

using TimestampEventSupport = IsWithinProducts<IGFX_SKYLAKE, IGFX_TIGERLAKE_LP>;
HWTEST2_F(CommandListAppendLaunchKernel, givenTimestampEventsWhenAppendingKernelThenSRMAndPCEncoded, TimestampEventSupport) {
    using GPGPU_WALKER = typename FamilyType::GPGPU_WALKER;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;
    using POST_SYNC_OPERATION = typename PIPE_CONTROL::POST_SYNC_OPERATION;
    using MI_LOAD_REGISTER_REG = typename FamilyType::MI_LOAD_REGISTER_REG;

    Mock<::L0::Kernel> kernel;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    auto usedSpaceBefore = commandList->getCmdContainer().getCommandStream()->getUsed();
    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_DEVICE;

    auto eventPool = std::unique_ptr<::L0::EventPool>(::L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    auto event = std::unique_ptr<::L0::Event>(::L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(
        kernel.toHandle(), &groupCount, event->toHandle(), 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto usedSpaceAfter = commandList->getCmdContainer().getCommandStream()->getUsed();
    EXPECT_GT(usedSpaceAfter, usedSpaceBefore);

    GenCmdList cmdList;
    EXPECT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList->getCmdContainer().getCommandStream()->getCpuBase(), 0), usedSpaceAfter));

    auto itor = find<MI_LOAD_REGISTER_REG *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), itor);
    {
        auto cmd = genCmdCast<MI_LOAD_REGISTER_REG *>(*itor);
        EXPECT_EQ(REG_GLOBAL_TIMESTAMP_LDW, cmd->getSourceRegisterAddress());
    }
    itor++;

    itor = find<MI_LOAD_REGISTER_REG *>(itor, cmdList.end());
    ASSERT_NE(cmdList.end(), itor);
    {
        auto cmd = genCmdCast<MI_LOAD_REGISTER_REG *>(*itor);
        EXPECT_EQ(GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, cmd->getSourceRegisterAddress());
    }
    itor++;

    itor = find<GPGPU_WALKER *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), itor);
    itor++;

    itor = find<PIPE_CONTROL *>(itor, cmdList.end());
    ASSERT_NE(cmdList.end(), itor);
    {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*itor);
        EXPECT_TRUE(cmd->getCommandStreamerStallEnable());
        EXPECT_TRUE(cmd->getDcFlushEnable());
    }
    itor++;

    itor = find<MI_LOAD_REGISTER_REG *>(itor, cmdList.end());
    ASSERT_NE(cmdList.end(), itor);
    {
        auto cmd = genCmdCast<MI_LOAD_REGISTER_REG *>(*itor);
        EXPECT_EQ(REG_GLOBAL_TIMESTAMP_LDW, cmd->getSourceRegisterAddress());
    }
    itor++;

    itor = find<MI_LOAD_REGISTER_REG *>(itor, cmdList.end());
    EXPECT_NE(cmdList.end(), itor);
    {
        auto cmd = genCmdCast<MI_LOAD_REGISTER_REG *>(*itor);
        EXPECT_EQ(GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, cmd->getSourceRegisterAddress());
    }
    itor++;

    auto numPCs = findAll<PIPE_CONTROL *>(itor, cmdList.end());
    // we should not have PC when signal scope is device
    ASSERT_EQ(0u, numPCs.size());

    {
        auto itorEvent = std::find(std::begin(commandList->getCmdContainer().getResidencyContainer()),
                                   std::end(commandList->getCmdContainer().getResidencyContainer()),
                                   &event->getAllocation(device));
        EXPECT_NE(itorEvent, std::end(commandList->getCmdContainer().getResidencyContainer()));
    }
}

HWTEST2_F(CommandListAppendLaunchKernel, givenKernelLaunchWithTSEventAndScopeFlagHostThenPCWithDCFlushEncoded, TimestampEventSupport) {
    using GPGPU_WALKER = typename FamilyType::GPGPU_WALKER;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;
    using POST_SYNC_OPERATION = typename PIPE_CONTROL::POST_SYNC_OPERATION;
    using MI_STORE_REGISTER_MEM = typename FamilyType::MI_STORE_REGISTER_MEM;

    Mock<::L0::Kernel> kernel;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    auto usedSpaceBefore = commandList->getCmdContainer().getCommandStream()->getUsed();
    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;

    const ze_event_desc_t eventDesc = {
        ZE_STRUCTURE_TYPE_EVENT_DESC,
        nullptr,
        0,
        ZE_EVENT_SCOPE_FLAG_HOST,
        ZE_EVENT_SCOPE_FLAG_HOST};

    auto eventPool = std::unique_ptr<::L0::EventPool>(::L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    auto event = std::unique_ptr<::L0::Event>(::L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(
        kernel.toHandle(), &groupCount, event->toHandle(), 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto usedSpaceAfter = commandList->getCmdContainer().getCommandStream()->getUsed();
    EXPECT_GT(usedSpaceAfter, usedSpaceBefore);

    GenCmdList cmdList;
    EXPECT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList->getCmdContainer().getCommandStream()->getCpuBase(), 0), usedSpaceAfter));

    auto itorPC = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(0u, itorPC.size());

    PIPE_CONTROL *cmd = genCmdCast<PIPE_CONTROL *>(*itorPC[itorPC.size() - 1]);
    EXPECT_TRUE(cmd->getCommandStreamerStallEnable());
    EXPECT_TRUE(cmd->getDcFlushEnable());
}

HWTEST2_F(CommandListAppendLaunchKernel, givenForcePipeControlPriorToWalkerKeyThenAdditionalPCIsAdded, IsAtLeastXeHpCore) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    Mock<::L0::Kernel> kernel;
    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandListBase(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    auto usedSpaceBefore = commandListBase->getCmdContainer().getCommandStream()->getUsed();

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    result = commandListBase->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto usedSpaceAfter = commandListBase->getCmdContainer().getCommandStream()->getUsed();
    EXPECT_GT(usedSpaceAfter, usedSpaceBefore);

    GenCmdList cmdListBase;
    EXPECT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdListBase, ptrOffset(commandListBase->getCmdContainer().getCommandStream()->getCpuBase(), 0), usedSpaceAfter));

    auto itorPC = findAll<PIPE_CONTROL *>(cmdListBase.begin(), cmdListBase.end());

    size_t numberOfPCsBase = itorPC.size();

    DebugManagerStateRestore restorer;
    DebugManager.flags.ForcePipeControlPriorToWalker.set(1);

    std::unique_ptr<L0::CommandList> commandListWithDebugKey(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    usedSpaceBefore = commandListWithDebugKey->getCmdContainer().getCommandStream()->getUsed();

    result = commandListWithDebugKey->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    usedSpaceAfter = commandListWithDebugKey->getCmdContainer().getCommandStream()->getUsed();
    EXPECT_GT(usedSpaceAfter, usedSpaceBefore);

    GenCmdList cmdListBaseWithDebugKey;
    EXPECT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdListBaseWithDebugKey, ptrOffset(commandListWithDebugKey->getCmdContainer().getCommandStream()->getCpuBase(), 0), usedSpaceAfter));

    itorPC = findAll<PIPE_CONTROL *>(cmdListBaseWithDebugKey.begin(), cmdListBaseWithDebugKey.end());

    size_t numberOfPCsWithDebugKey = itorPC.size();

    EXPECT_EQ(numberOfPCsWithDebugKey, numberOfPCsBase + 1);
}

HWTEST2_F(CommandListAppendLaunchKernel, givenForcePipeControlPriorToWalkerKeyAndNoSpaceThenNewBatchBufferAllocationIsUsed, IsAtLeastXeHpCore) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.ForcePipeControlPriorToWalker.set(1);

    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    Mock<::L0::Kernel> kernel;
    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto firstBatchBufferAllocation = commandList->getCmdContainer().getCommandStream()->getGraphicsAllocation();

    auto useSize = commandList->getCmdContainer().getCommandStream()->getAvailableSpace();
    useSize -= sizeof(PIPE_CONTROL);
    commandList->getCmdContainer().getCommandStream()->getSpace(useSize);

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto secondBatchBufferAllocation = commandList->getCmdContainer().getCommandStream()->getGraphicsAllocation();

    EXPECT_NE(firstBatchBufferAllocation, secondBatchBufferAllocation);
}

using SupportedPlatforms = IsWithinProducts<IGFX_SKYLAKE, IGFX_DG1>;
HWTEST2_F(CommandListAppendLaunchKernel, givenCommandListWhenAppendLaunchKernelSeveralTimesThenAlwaysFirstEventPacketIsUsed, SupportedPlatforms) {
    createKernel();
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP | ZE_EVENT_POOL_FLAG_HOST_VISIBLE;

    const ze_event_desc_t eventDesc = {
        ZE_STRUCTURE_TYPE_EVENT_DESC,
        nullptr,
        0,
        ZE_EVENT_SCOPE_FLAG_HOST,
        ZE_EVENT_SCOPE_FLAG_HOST};

    auto eventPool = std::unique_ptr<::L0::EventPool>(::L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    auto event = std::unique_ptr<::L0::Event>(::L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    EXPECT_EQ(1u, event->getPacketsInUse());
    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    for (uint32_t i = 0; i < NEO::TimestampPacketSizeControl::preferredPacketCount + 4; i++) {
        auto result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, event->toHandle(), 0, nullptr, launchParams, false);
        EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    }
    EXPECT_EQ(1u, event->getPacketsInUse());
}

HWTEST_F(CommandListAppendLaunchKernel, givenIndirectDispatchWhenAppendingThenWorkGroupCountAndGlobalWorkSizeAndWorkDimIsSetInCrossThreadData) {
    using MI_STORE_REGISTER_MEM = typename FamilyType::MI_STORE_REGISTER_MEM;
    using MI_LOAD_REGISTER_REG = typename FamilyType::MI_LOAD_REGISTER_REG;
    using MI_LOAD_REGISTER_IMM = typename FamilyType::MI_LOAD_REGISTER_IMM;

    Mock<::L0::Kernel> kernel;
    kernel.groupSize[0] = 2;
    kernel.descriptor.payloadMappings.dispatchTraits.numWorkGroups[0] = 2;
    kernel.descriptor.payloadMappings.dispatchTraits.globalWorkSize[0] = 2;
    kernel.descriptor.payloadMappings.dispatchTraits.workDim = 4;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));

    void *alloc = nullptr;
    ze_device_mem_alloc_desc_t deviceDesc = {};
    auto result = context->allocDeviceMem(device->toHandle(), &deviceDesc, 16384u, 4096u, &alloc);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);

    result = commandList->appendLaunchKernelIndirect(kernel.toHandle(),
                                                     static_cast<ze_group_count_t *>(alloc),
                                                     nullptr, 0, nullptr, false);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);

    kernel.groupSize[2] = 2;
    result = commandList->appendLaunchKernelIndirect(kernel.toHandle(),
                                                     static_cast<ze_group_count_t *>(alloc),
                                                     nullptr, 0, nullptr, false);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList->getCmdContainer().getCommandStream()->getCpuBase(), 0), commandList->getCmdContainer().getCommandStream()->getUsed()));

    auto itor = find<MI_STORE_REGISTER_MEM *>(cmdList.begin(), cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_LOAD_REGISTER_REG *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor = find<MI_LOAD_REGISTER_IMM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor = find<MI_STORE_REGISTER_MEM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_LOAD_REGISTER_IMM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor = find<MI_LOAD_REGISTER_IMM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_LOAD_REGISTER_REG *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    itor++; // MI_MATH_ALU_INST_INLINE doesn't have tagMI_COMMAND_OPCODE, can't find it in cmdList
    EXPECT_NE(itor, cmdList.end());
    itor++;
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_LOAD_REGISTER_IMM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor = find<MI_LOAD_REGISTER_REG *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    itor++; // MI_MATH_ALU_INST_INLINE doesn't have tagMI_COMMAND_OPCODE, can't find it in cmdList
    EXPECT_NE(itor, cmdList.end());
    itor++;
    EXPECT_NE(itor, cmdList.end());
    itor++;
    EXPECT_NE(itor, cmdList.end());
    itor++;
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_LOAD_REGISTER_REG *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor++; // MI_MATH_ALU_INST_INLINE doesn't have tagMI_COMMAND_OPCODE, can't find it in cmdList
    EXPECT_NE(itor, cmdList.end());
    itor++;
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_STORE_REGISTER_MEM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_STORE_REGISTER_MEM *>(++itor, cmdList.end()); // kernel with groupSize[2] = 2
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_LOAD_REGISTER_REG *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor = find<MI_LOAD_REGISTER_IMM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor = find<MI_STORE_REGISTER_MEM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    itor = find<MI_LOAD_REGISTER_IMM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());
    itor = find<MI_STORE_REGISTER_MEM *>(++itor, cmdList.end());
    EXPECT_NE(itor, cmdList.end());

    context->freeMem(alloc);
}

HWTEST_F(CommandListAppendLaunchKernel, givenCommandListWhenResetCalledThenStateIsCleaned) {
    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.EnableStateBaseAddressTracking.set(0);

    using STATE_BASE_ADDRESS = typename FamilyType::STATE_BASE_ADDRESS;
    createKernel();

    ze_result_t returnValue;
    auto commandList = std::unique_ptr<CommandList>(whiteboxCast(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue)));
    ASSERT_NE(nullptr, commandList);
    ASSERT_NE(nullptr, commandList->getCmdContainer().getCommandStream());

    auto commandListControl = std::unique_ptr<CommandList>(whiteboxCast(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue)));
    ASSERT_NE(nullptr, commandListControl);
    ASSERT_NE(nullptr, commandListControl->getCmdContainer().getCommandStream());

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(
        kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    result = commandList->close();
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    result = commandList->reset();
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    ASSERT_EQ(device, commandList->device);
    ASSERT_NE(nullptr, commandList->getCmdContainer().getCommandStream());
    ASSERT_GE(commandListControl->getCmdContainer().getCmdBufferAllocations()[0]->getUnderlyingBufferSize(), commandList->getCmdContainer().getCmdBufferAllocations()[0]->getUnderlyingBufferSize());
    ASSERT_EQ(commandListControl->getCmdContainer().getResidencyContainer().size(),
              commandList->getCmdContainer().getResidencyContainer().size());
    ASSERT_EQ(commandListControl->getCmdContainer().getDeallocationContainer().size(),
              commandList->getCmdContainer().getDeallocationContainer().size());
    ASSERT_EQ(commandListControl->getPrintfKernelContainer().size(),
              commandList->getPrintfKernelContainer().size());
    ASSERT_EQ(commandListControl->getCmdContainer().getCommandStream()->getUsed(), commandList->getCmdContainer().getCommandStream()->getUsed());
    ASSERT_EQ(commandListControl->getCmdContainer().slmSizeRef(), commandList->getCmdContainer().slmSizeRef());

    for (uint32_t i = 0; i < NEO::HeapType::NUM_TYPES; i++) {
        auto heapType = static_cast<NEO::HeapType>(i);
        if (NEO::HeapType::DYNAMIC_STATE == heapType && !device->getHwInfo().capabilityTable.supportsImages) {
            ASSERT_EQ(nullptr, commandListControl->getCmdContainer().getIndirectHeapAllocation(heapType));
            ASSERT_EQ(nullptr, commandListControl->getCmdContainer().getIndirectHeap(heapType));
        } else {
            ASSERT_NE(nullptr, commandListControl->getCmdContainer().getIndirectHeapAllocation(heapType));
            ASSERT_NE(nullptr, commandList->getCmdContainer().getIndirectHeapAllocation(heapType));
            ASSERT_EQ(commandListControl->getCmdContainer().getIndirectHeapAllocation(heapType)->getUnderlyingBufferSize(),
                      commandList->getCmdContainer().getIndirectHeapAllocation(heapType)->getUnderlyingBufferSize());

            ASSERT_NE(nullptr, commandListControl->getCmdContainer().getIndirectHeap(heapType));
            ASSERT_NE(nullptr, commandList->getCmdContainer().getIndirectHeap(heapType));
            ASSERT_EQ(commandListControl->getCmdContainer().getIndirectHeap(heapType)->getUsed(),
                      commandList->getCmdContainer().getIndirectHeap(heapType)->getUsed());

            ASSERT_EQ(commandListControl->getCmdContainer().isHeapDirty(heapType), commandList->getCmdContainer().isHeapDirty(heapType));
        }
    }

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList->getCmdContainer().getCommandStream()->getCpuBase(), 0), commandList->getCmdContainer().getCommandStream()->getUsed()));

    auto itor = find<STATE_BASE_ADDRESS *>(cmdList.begin(), cmdList.end());
    EXPECT_NE(cmdList.end(), itor);
}

HWTEST_F(CommandListAppendLaunchKernel, WhenAddingKernelsThenResidencyContainerDoesNotContainDuplicatesAfterClosingCommandList) {
    Mock<::L0::Kernel> kernel;

    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    for (int i = 0; i < 4; ++i) {
        auto result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
        EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    }

    commandList->close();

    uint32_t it = 0;
    const auto &residencyCont = commandList->getCmdContainer().getResidencyContainer();
    for (auto alloc : residencyCont) {
        auto occurences = std::count(residencyCont.begin(), residencyCont.end(), alloc);
        EXPECT_EQ(1U, static_cast<uint32_t>(occurences)) << it;
        ++it;
    }
}

HWTEST_F(CommandListAppendLaunchKernel, givenSingleValidWaitEventsThenAddSemaphoreToCommandStream) {
    using MI_SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    Mock<::L0::Kernel> kernel;

    ze_result_t returnValue;
    auto commandList = std::unique_ptr<L0::CommandList>(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    ASSERT_NE(nullptr, commandList->getCmdContainer().getCommandStream());
    auto usedSpaceBefore = commandList->getCmdContainer().getCommandStream()->getUsed();

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
    eventPoolDesc.count = 1;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;

    std::unique_ptr<::L0::EventPool> eventPool(::L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    std::unique_ptr<::L0::Event> event(::L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    ze_event_handle_t hEventHandle = event->toHandle();

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 1, &hEventHandle, launchParams, false);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    auto usedSpaceAfter = commandList->getCmdContainer().getCommandStream()->getUsed();
    ASSERT_GT(usedSpaceAfter, usedSpaceBefore);

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList->getCmdContainer().getCommandStream()->getCpuBase(), 0), usedSpaceAfter));

    auto itor = find<MI_SEMAPHORE_WAIT *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), itor);

    {
        auto cmd = genCmdCast<MI_SEMAPHORE_WAIT *>(*itor);
        EXPECT_EQ(cmd->getCompareOperation(),
                  MI_SEMAPHORE_WAIT::COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD);
        EXPECT_EQ(static_cast<uint32_t>(-1), cmd->getSemaphoreDataDword());
        auto addressSpace = device->getHwInfo().capabilityTable.gpuAddressSpace;

        uint64_t gpuAddress = event->getCompletionFieldGpuAddress(device);

        EXPECT_EQ(gpuAddress & addressSpace, cmd->getSemaphoreGraphicsAddress() & addressSpace);
    }
}

HWTEST_F(CommandListAppendLaunchKernel, givenMultipleValidWaitEventsThenAddSemaphoreCommands) {
    using MI_SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    Mock<::L0::Kernel> kernel;

    ze_result_t returnValue;
    auto commandList = std::unique_ptr<L0::CommandList>(L0::CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    ASSERT_NE(nullptr, commandList->getCmdContainer().getCommandStream());
    auto usedSpaceBefore = commandList->getCmdContainer().getCommandStream()->getUsed();

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
    eventPoolDesc.count = 2;

    ze_event_desc_t eventDesc1 = {};
    eventDesc1.index = 0;

    ze_event_desc_t eventDesc2 = {};
    eventDesc2.index = 1;

    std::unique_ptr<::L0::EventPool> eventPool(::L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    std::unique_ptr<::L0::Event> event1(::L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc1, device));
    std::unique_ptr<::L0::Event> event2(::L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc2, device));
    ze_event_handle_t hEventHandle1 = event1->toHandle();
    ze_event_handle_t hEventHandle2 = event2->toHandle();

    ze_event_handle_t waitEvents[2];
    waitEvents[0] = hEventHandle1;
    waitEvents[1] = hEventHandle2;

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    auto result = commandList->appendLaunchKernel(kernel.toHandle(), &groupCount, nullptr, 2, waitEvents, launchParams, false);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    auto usedSpaceAfter = commandList->getCmdContainer().getCommandStream()->getUsed();
    ASSERT_GT(usedSpaceAfter, usedSpaceBefore);

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList, ptrOffset(commandList->getCmdContainer().getCommandStream()->getCpuBase(), 0), usedSpaceAfter));
    using MI_SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    auto itor = findAll<MI_SEMAPHORE_WAIT *>(cmdList.begin(), cmdList.end());
    ASSERT_FALSE(itor.empty());
    ASSERT_EQ(2, static_cast<int>(itor.size()));
}

HWTEST_F(CommandListAppendLaunchKernel, givenInvalidEventListWhenAppendLaunchCooperativeKernelIsCalledThenErrorIsReturned) {
    createKernel();

    ze_group_count_t groupCount{1, 1, 1};
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    returnValue = commandList->appendLaunchCooperativeKernel(kernel->toHandle(), &groupCount, nullptr, 1, nullptr, false);

    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, returnValue);
}

HWTEST2_F(CommandListAppendLaunchKernel, givenImmediateCommandListWhenAppendLaunchCooperativeKernelUsingFlushTaskThenExpectCorrectExecuteCall, IsAtLeastSkl) {
    createKernel();
    MockCommandListImmediateHw<gfxCoreFamily> cmdList;
    cmdList.isFlushTaskSubmissionEnabled = true;
    cmdList.cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    cmdList.csr = device->getNEODevice()->getDefaultEngine().commandStreamReceiver;
    cmdList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    cmdList.commandContainer.setImmediateCmdListCsr(device->getNEODevice()->getDefaultEngine().commandStreamReceiver);
    ze_group_count_t groupCount{1, 1, 1};
    ze_result_t returnValue;
    returnValue = cmdList.appendLaunchCooperativeKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, false);
    EXPECT_EQ(0u, cmdList.executeCommandListImmediateCalledCount);
    EXPECT_EQ(1u, cmdList.executeCommandListImmediateWithFlushTaskCalledCount);
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
}

HWTEST2_F(CommandListAppendLaunchKernel, givenImmediateCommandListWhenAppendLaunchCooperativeKernelNotUsingFlushTaskThenExpectCorrectExecuteCall, IsAtLeastSkl) {
    createKernel();
    MockCommandListImmediateHw<gfxCoreFamily> cmdList;
    cmdList.isFlushTaskSubmissionEnabled = false;
    cmdList.cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    cmdList.csr = device->getNEODevice()->getDefaultEngine().commandStreamReceiver;
    cmdList.initialize(device, NEO::EngineGroupType::RenderCompute, 0u);
    cmdList.commandContainer.setImmediateCmdListCsr(device->getNEODevice()->getDefaultEngine().commandStreamReceiver);
    ze_group_count_t groupCount{1, 1, 1};
    ze_result_t returnValue;
    returnValue = cmdList.appendLaunchCooperativeKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, false);
    EXPECT_EQ(1u, cmdList.executeCommandListImmediateCalledCount);
    EXPECT_EQ(0u, cmdList.executeCommandListImmediateWithFlushTaskCalledCount);
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
}

HWTEST2_F(CommandListAppendLaunchKernel, whenUpdateStreamPropertiesIsCalledThenCorrectThreadArbitrationPolicyIsSet, IsAtLeastSkl) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.ForceThreadArbitrationPolicyProgrammingWithScm.set(1);

    auto &gfxCoreHelper = device->getGfxCoreHelper();
    auto expectedThreadArbitrationPolicy = gfxCoreHelper.getDefaultThreadArbitrationPolicy();
    int32_t threadArbitrationPolicyValues[] = {
        ThreadArbitrationPolicy::AgeBased, ThreadArbitrationPolicy::RoundRobin,
        ThreadArbitrationPolicy::RoundRobinAfterDependency};

    Mock<::L0::Kernel> kernel;
    auto mockModule = std::unique_ptr<Module>(new Mock<Module>(device, nullptr));
    kernel.module = mockModule.get();

    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    auto result = commandList->initialize(device, NEO::EngineGroupType::Compute, 0u);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(-1, commandList->requiredStreamState.stateComputeMode.threadArbitrationPolicy.value);
    EXPECT_EQ(-1, commandList->finalStreamState.stateComputeMode.threadArbitrationPolicy.value);

    const ze_group_count_t launchKernelArgs = {};
    commandList->updateStreamProperties(kernel, false, &launchKernelArgs, false);
    EXPECT_EQ(expectedThreadArbitrationPolicy, commandList->finalStreamState.stateComputeMode.threadArbitrationPolicy.value);

    for (auto threadArbitrationPolicy : threadArbitrationPolicyValues) {
        DebugManager.flags.OverrideThreadArbitrationPolicy.set(threadArbitrationPolicy);
        commandList->reset();
        commandList->updateStreamProperties(kernel, false, &launchKernelArgs, false);
        EXPECT_EQ(threadArbitrationPolicy, commandList->finalStreamState.stateComputeMode.threadArbitrationPolicy.value);
    }
}

} // namespace ult
} // namespace L0
