/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_container/encode_surface_state.h"
#include "shared/source/direct_submission/relaxed_ordering_helper.h"
#include "shared/source/helpers/aligned_memory.h"
#include "shared/source/memory_manager/internal_allocation_storage.h"
#include "shared/test/common/cmd_parse/gen_cmd_parse.h"
#include "shared/test/common/libult/ult_command_stream_receiver.h"
#include "shared/test/common/mocks/mock_direct_submission_hw.h"
#include "shared/test/common/mocks/mock_graphics_allocation.h"
#include "shared/test/common/mocks/mock_memory_manager.h"
#include "shared/test/common/test_macros/hw_test.h"

#include "level_zero/core/source/event/event.h"
#include "level_zero/core/source/gfx_core_helpers/l0_gfx_core_helper.h"
#include "level_zero/core/source/image/image_hw.h"
#include "level_zero/core/test/unit_tests/fixtures/cmdlist_fixture.h"
#include "level_zero/core/test/unit_tests/mocks/mock_cmdlist.h"
#include "level_zero/core/test/unit_tests/mocks/mock_cmdqueue.h"
#include "level_zero/core/test/unit_tests/mocks/mock_event.h"

namespace L0 {
namespace ult {

struct MemoryManagerCommandListCreateNegativeTest : public NEO::MockMemoryManager {
    MemoryManagerCommandListCreateNegativeTest(NEO::ExecutionEnvironment &executionEnvironment) : NEO::MockMemoryManager(const_cast<NEO::ExecutionEnvironment &>(executionEnvironment)) {}
    NEO::GraphicsAllocation *allocateGraphicsMemoryWithProperties(const NEO::AllocationProperties &properties) override {
        if (forceFailureInPrimaryAllocation) {
            return nullptr;
        }
        return NEO::MemoryManager::allocateGraphicsMemoryWithProperties(properties);
    }
    bool forceFailureInPrimaryAllocation = false;
};

template <int32_t stateBaseAddressTracking>
struct CommandListCreateNegativeFixture {
    void setUp() {
        DebugManager.flags.EnableStateBaseAddressTracking.set(stateBaseAddressTracking);

        executionEnvironment = new NEO::ExecutionEnvironment();
        executionEnvironment->prepareRootDeviceEnvironments(numRootDevices);
        for (uint32_t i = 0; i < numRootDevices; i++) {
            executionEnvironment->rootDeviceEnvironments[i]->setHwInfoAndInitHelpers(NEO::defaultHwInfo.get());
            executionEnvironment->rootDeviceEnvironments[i]->initGmm();
        }

        memoryManager = new MemoryManagerCommandListCreateNegativeTest(*executionEnvironment);
        executionEnvironment->memoryManager.reset(memoryManager);

        std::vector<std::unique_ptr<NEO::Device>> devices;
        for (uint32_t i = 0; i < numRootDevices; i++) {
            neoDevice = NEO::MockDevice::create<NEO::MockDevice>(executionEnvironment, i);
            devices.push_back(std::unique_ptr<NEO::Device>(neoDevice));
        }

        driverHandle = std::make_unique<Mock<L0::DriverHandleImp>>();
        driverHandle->initialize(std::move(devices));

        device = driverHandle->devices[0];
    }
    void tearDown() {
    }

    DebugManagerStateRestore restorer;

    NEO::ExecutionEnvironment *executionEnvironment = nullptr;
    std::unique_ptr<Mock<L0::DriverHandleImp>> driverHandle;
    NEO::MockDevice *neoDevice = nullptr;
    L0::Device *device = nullptr;
    MemoryManagerCommandListCreateNegativeTest *memoryManager = nullptr;
    const uint32_t numRootDevices = 1u;
};

using CommandListCreateNegativeTest = Test<CommandListCreateNegativeFixture<0>>;

TEST_F(CommandListCreateNegativeTest, whenDeviceAllocationFailsDuringCommandListCreateThenAppropriateValueIsReturned) {
    ze_result_t returnValue;
    memoryManager->forceFailureInPrimaryAllocation = true;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    EXPECT_EQ(ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY, returnValue);
    ASSERT_EQ(nullptr, commandList);
}

using CommandListCreateNegativeStateBaseAddressTest = Test<CommandListCreateNegativeFixture<1>>;

HWTEST2_F(CommandListCreateNegativeStateBaseAddressTest,
          GivenStateBaseAddressTrackingWhenDeviceAllocationFailsDuringCommandListCreateThenCacheIsNotInvalidatedAndAppropriateValueIsReturned,
          IsAtLeastSkl) {
    auto &csr = neoDevice->getUltCommandStreamReceiver<FamilyType>();
    auto &csrStream = csr.commandStream;

    ze_result_t returnValue;
    memoryManager->forceFailureInPrimaryAllocation = true;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, returnValue));
    EXPECT_EQ(ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY, returnValue);
    ASSERT_EQ(nullptr, commandList);

    EXPECT_EQ(0u, csrStream.getUsed());
}

TEST_F(CommandListCreateNegativeTest, whenDeviceAllocationFailsDuringCommandListImmediateCreateThenAppropriateValueIsReturned) {
    ze_result_t returnValue;
    const ze_command_queue_desc_t desc = {};
    bool internalEngine = true;
    memoryManager->forceFailureInPrimaryAllocation = true;
    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily,
                                                                              device,
                                                                              &desc,
                                                                              internalEngine,
                                                                              NEO::EngineGroupType::RenderCompute,
                                                                              returnValue));
    EXPECT_EQ(ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY, returnValue);
    ASSERT_EQ(nullptr, commandList);
}

using CommandListCreate = Test<DeviceFixture>;

HWTEST2_F(CommandListCreate, givenHostAllocInMapWhenGettingAllocInRangeThenAllocFromMapReturned, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    uint64_t gpuAddress = 0x1200;
    const void *cpuPtr = reinterpret_cast<const void *>(gpuAddress);
    size_t allocSize = 0x1000;
    NEO::MockGraphicsAllocation alloc(const_cast<void *>(cpuPtr), gpuAddress, allocSize);
    commandList->hostPtrMap.insert(std::make_pair(cpuPtr, &alloc));
    EXPECT_EQ(commandList->getHostPtrMap().size(), 1u);

    auto newBufferPtr = ptrOffset(cpuPtr, 0x10);
    auto newBufferSize = allocSize - 0x20;
    auto newAlloc = commandList->getAllocationFromHostPtrMap(newBufferPtr, newBufferSize);
    EXPECT_NE(newAlloc, nullptr);
    commandList->hostPtrMap.clear();
}

HWTEST2_F(CommandListCreate, givenHostAllocInMapWhenSizeIsOutOfRangeThenNullPtrReturned, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    uint64_t gpuAddress = 0x1200;
    const void *cpuPtr = reinterpret_cast<const void *>(gpuAddress);
    size_t allocSize = 0x1000;
    NEO::MockGraphicsAllocation alloc(const_cast<void *>(cpuPtr), gpuAddress, allocSize);
    commandList->hostPtrMap.insert(std::make_pair(cpuPtr, &alloc));
    EXPECT_EQ(commandList->getHostPtrMap().size(), 1u);

    auto newBufferPtr = ptrOffset(cpuPtr, 0x10);
    auto newBufferSize = allocSize + 0x20;
    auto newAlloc = commandList->getAllocationFromHostPtrMap(newBufferPtr, newBufferSize);
    EXPECT_EQ(newAlloc, nullptr);
    commandList->hostPtrMap.clear();
}

HWTEST2_F(CommandListCreate, givenHostAllocInMapWhenPtrIsOutOfRangeThenNullPtrReturned, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    uint64_t gpuAddress = 0x1200;
    const void *cpuPtr = reinterpret_cast<const void *>(gpuAddress);
    size_t allocSize = 0x1000;
    NEO::MockGraphicsAllocation alloc(const_cast<void *>(cpuPtr), gpuAddress, allocSize);
    commandList->hostPtrMap.insert(std::make_pair(cpuPtr, &alloc));
    EXPECT_EQ(commandList->getHostPtrMap().size(), 1u);

    auto newBufferPtr = reinterpret_cast<const void *>(gpuAddress - 0x100);
    auto newBufferSize = allocSize - 0x200;
    auto newAlloc = commandList->getAllocationFromHostPtrMap(newBufferPtr, newBufferSize);
    EXPECT_EQ(newAlloc, nullptr);
    commandList->hostPtrMap.clear();
}

HWTEST2_F(CommandListCreate, givenHostAllocInMapWhenGetHostPtrAllocCalledThenCorrectOffsetIsSet, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    uint64_t gpuAddress = 0x1200;
    const void *cpuPtr = reinterpret_cast<const void *>(gpuAddress);
    size_t allocSize = 0x1000;
    NEO::MockGraphicsAllocation alloc(const_cast<void *>(cpuPtr), gpuAddress, allocSize);
    commandList->hostPtrMap.insert(std::make_pair(cpuPtr, &alloc));
    EXPECT_EQ(commandList->getHostPtrMap().size(), 1u);

    size_t expectedOffset = 0x10;
    auto newBufferPtr = ptrOffset(cpuPtr, expectedOffset);
    auto newBufferSize = allocSize - 0x20;
    auto newAlloc = commandList->getHostPtrAlloc(newBufferPtr, newBufferSize, false);
    EXPECT_NE(nullptr, newAlloc);
    commandList->hostPtrMap.clear();
}

template <NEO::AllocationType AllocType>
class DeviceHostPtrFailMock : public Mock<DeviceImp> {
  public:
    using Mock<L0::DeviceImp>::Mock;
    NEO::GraphicsAllocation *allocateMemoryFromHostPtr(const void *buffer, size_t size, bool hostCopyAllowed) override {
        return nullptr;
    }
    const NEO::HardwareInfo &getHwInfo() const override {
        return neoDevice->getHardwareInfo();
    }
};

HWTEST2_F(CommandListCreate, givenGetAlignedAllocationCalledWithInvalidPtrThenNullptrReturned, IsAtLeastSkl) {
    auto failDevice = std::make_unique<DeviceHostPtrFailMock<NEO::AllocationType::INTERNAL_HOST_MEMORY>>(device->getNEODevice(), execEnv);
    failDevice->neoDevice = device->getNEODevice();
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(failDevice.get(), NEO::EngineGroupType::Copy, 0u);

    size_t cmdListHostPtrSize = MemoryConstants::pageSize;
    void *cmdListHostBuffer = reinterpret_cast<void *>(0x1234);
    AlignedAllocationData outData = {};
    outData = commandList->getAlignedAllocationData(device, cmdListHostBuffer, cmdListHostPtrSize, false);
    EXPECT_EQ(nullptr, outData.alloc);
}

HWTEST2_F(CommandListCreate, givenHostAllocInMapWhenPtrIsInMapThenAllocationReturned, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    uint64_t gpuAddress = 0x1200;
    const void *cpuPtr = reinterpret_cast<const void *>(gpuAddress);
    size_t allocSize = 0x1000;
    NEO::MockGraphicsAllocation alloc(const_cast<void *>(cpuPtr), gpuAddress, allocSize);
    commandList->hostPtrMap.insert(std::make_pair(cpuPtr, &alloc));
    EXPECT_EQ(commandList->getHostPtrMap().size(), 1u);

    auto newBufferPtr = cpuPtr;
    auto newBufferSize = allocSize - 0x20;
    auto newAlloc = commandList->getAllocationFromHostPtrMap(newBufferPtr, newBufferSize);
    EXPECT_EQ(newAlloc, &alloc);
    commandList->hostPtrMap.clear();
}

HWTEST2_F(CommandListCreate, givenHostAllocInMapWhenPtrIsInMapButWithBiggerSizeThenNullPtrReturned, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    uint64_t gpuAddress = 0x1200;
    const void *cpuPtr = reinterpret_cast<const void *>(gpuAddress);
    size_t allocSize = 0x1000;
    NEO::MockGraphicsAllocation alloc(const_cast<void *>(cpuPtr), gpuAddress, allocSize);
    commandList->hostPtrMap.insert(std::make_pair(cpuPtr, &alloc));
    EXPECT_EQ(commandList->getHostPtrMap().size(), 1u);

    auto newBufferPtr = cpuPtr;
    auto newBufferSize = allocSize + 0x20;
    auto newAlloc = commandList->getAllocationFromHostPtrMap(newBufferPtr, newBufferSize);
    EXPECT_EQ(newAlloc, nullptr);
    commandList->hostPtrMap.clear();
}

HWTEST2_F(CommandListCreate, givenHostAllocInMapWhenPtrLowerThanAnyInMapThenNullPtrReturned, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(device, NEO::EngineGroupType::Copy, 0u);
    uint64_t gpuAddress = 0x1200;
    const void *cpuPtr = reinterpret_cast<const void *>(gpuAddress);
    size_t allocSize = 0x1000;
    NEO::MockGraphicsAllocation alloc(const_cast<void *>(cpuPtr), gpuAddress, allocSize);
    commandList->hostPtrMap.insert(std::make_pair(cpuPtr, &alloc));
    EXPECT_EQ(commandList->getHostPtrMap().size(), 1u);

    auto newBufferPtr = reinterpret_cast<const void *>(gpuAddress - 0x10);
    auto newBufferSize = allocSize - 0x20;
    auto newAlloc = commandList->getAllocationFromHostPtrMap(newBufferPtr, newBufferSize);
    EXPECT_EQ(newAlloc, nullptr);
    commandList->hostPtrMap.clear();
}

HWTEST2_F(CommandListCreate,
          givenCmdListHostPointerUsedWhenGettingAlignedAllocationThenRetrieveProperOffsetAndAddress,
          IsAtLeastSkl) {
    auto commandList = std::make_unique<::L0::ult::CommandListCoreFamily<gfxCoreFamily>>();
    commandList->initialize(device, NEO::EngineGroupType::RenderCompute, 0u);

    size_t cmdListHostPtrSize = MemoryConstants::pageSize;
    void *cmdListHostBuffer = device->getNEODevice()->getMemoryManager()->allocateSystemMemory(cmdListHostPtrSize, cmdListHostPtrSize);
    void *startMemory = cmdListHostBuffer;
    void *baseAddress = alignDown(startMemory, MemoryConstants::pageSize);
    size_t expectedOffset = ptrDiff(startMemory, baseAddress);

    AlignedAllocationData outData = commandList->getAlignedAllocationData(device, startMemory, cmdListHostPtrSize, false);
    ASSERT_NE(nullptr, outData.alloc);
    auto firstAlloc = outData.alloc;
    auto expectedGpuAddress = static_cast<uintptr_t>(alignDown(outData.alloc->getGpuAddress(), MemoryConstants::pageSize));
    EXPECT_EQ(startMemory, outData.alloc->getUnderlyingBuffer());
    EXPECT_EQ(expectedGpuAddress, outData.alignedAllocationPtr);
    EXPECT_EQ(expectedOffset, outData.offset);

    size_t offset = 0x21u;
    void *offsetMemory = ptrOffset(startMemory, offset);
    expectedOffset = ptrDiff(offsetMemory, baseAddress);
    size_t alignedOffset = offset & EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignmentMask();
    expectedGpuAddress = ptrOffset(expectedGpuAddress, alignedOffset);
    EXPECT_EQ(outData.offset + offset, expectedOffset);

    outData = commandList->getAlignedAllocationData(device, offsetMemory, 4u, false);
    ASSERT_NE(nullptr, outData.alloc);
    EXPECT_EQ(firstAlloc, outData.alloc);
    EXPECT_EQ(startMemory, outData.alloc->getUnderlyingBuffer());
    EXPECT_EQ(expectedGpuAddress, outData.alignedAllocationPtr);
    EXPECT_EQ((expectedOffset & (EncodeSurfaceState<FamilyType>::getSurfaceBaseAddressAlignment() - 1)), outData.offset);

    commandList->removeHostPtrAllocations();
    device->getNEODevice()->getMemoryManager()->freeSystemMemory(cmdListHostBuffer);
}

using PlatformSupport = IsWithinProducts<IGFX_SKYLAKE, IGFX_DG1>;
HWTEST2_F(CommandListCreate,
          givenCommandListWhenMemoryCopyRegionHavingHostMemoryWithSignalAndWaitScopeEventsUsingRenderEngineThenPipeControlsWithDcFlushIsFound,
          PlatformSupport) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    auto &commandContainer = commandList->getCmdContainer();
    commandContainer.slmSizeRef() = 0;

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);
    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};
    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    result = commandList->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                 srcBuffer, &sr, width, 0, events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));

    auto allPcCommands = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());

    uint32_t dcFlushPipeControls = 0;
    for (auto it : allPcCommands) {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*it);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControls++;
        }
    }
    EXPECT_EQ(2u, dcFlushPipeControls);
}

HWTEST2_F(CommandListCreate,
          givenCommandListWhenMemoryCopyRegionHavingDeviceMemoryWithNoSignalAndWaitScopeEventsUsingRenderEngineThenPipeControlWithDcFlushIsFound,
          PlatformSupport) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    auto &commandContainer = commandList->getCmdContainer();
    commandContainer.slmSizeRef() = 0;

    void *srcBuffer = nullptr;
    void *dstBuffer = nullptr;
    ze_device_mem_alloc_desc_t deviceDesc = {};
    result = context->allocDeviceMem(device->toHandle(), &deviceDesc, 16384u, 4096u, &srcBuffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);
    result = context->allocDeviceMem(device->toHandle(), &deviceDesc, 16384u, 4096u, &dstBuffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};
    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    result = commandList->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                 srcBuffer, &sr, width, 0, events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));

    auto allPcCommands = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
    uint32_t dcFlushPipeControls = 0;
    for (auto it : allPcCommands) {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*it);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControls++;
        }
    }
    EXPECT_EQ(1u, dcFlushPipeControls);

    context->freeMem(srcBuffer);
    context->freeMem(dstBuffer);
}

HWTEST2_F(CommandListCreate,
          givenCommandListWhenMemoryFillHavingDeviceMemoryWithSignalAndNoWaitScopeEventsUsingRenderEngineThenPipeControlWithDcFlushIsFound,
          PlatformSupport) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    auto &commandContainer = commandList->getCmdContainer();
    commandContainer.slmSizeRef() = 0;

    void *dstBuffer = nullptr;
    ze_device_mem_alloc_desc_t deviceDesc = {};
    result = context->allocDeviceMem(device->toHandle(), &deviceDesc, 16384u, 4096u, &dstBuffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    int one = 1;
    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    result = commandList->appendMemoryFill(dstBuffer, reinterpret_cast<void *>(&one), sizeof(one), 4096u,
                                           events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));

    auto allPcCommands = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
    uint32_t dcFlushPipeControls = 0;
    for (auto it : allPcCommands) {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*it);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControls++;
        }
    }
    EXPECT_EQ(1u, dcFlushPipeControls);

    context->freeMem(dstBuffer);
}

HWTEST2_F(CommandListCreate,
          givenCommandListWhenMemoryFillHavingSharedMemoryWithSignalAndWaitScopeEventsUsingRenderEngineThenPipeControlsWithDcFlushIsFound,
          PlatformSupport) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    auto &commandContainer = commandList->getCmdContainer();
    commandContainer.slmSizeRef() = 0;

    void *dstBuffer = nullptr;
    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    result = context->allocSharedMem(device->toHandle(), &deviceDesc, &hostDesc, 16384u, 4096u, &dstBuffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    int one = 1;

    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    result = commandList->appendMemoryFill(dstBuffer, reinterpret_cast<void *>(&one), sizeof(one), 4096u,
                                           events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));

    auto allPcCommands = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
    uint32_t dcFlushPipeControls = 0;
    for (auto it : allPcCommands) {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*it);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControls++;
        }
    }
    EXPECT_EQ(2u, dcFlushPipeControls);

    context->freeMem(dstBuffer);
}

HWTEST2_F(CommandListCreate, givenCommandListWhenMemoryFillHavingHostMemoryWithSignalAndWaitScopeEventsUsingRenderEngineThenPipeControlWithDcFlushIsFound, PlatformSupport) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    auto &commandContainer = commandList->getCmdContainer();
    commandContainer.slmSizeRef() = 0;

    void *dstBuffer = nullptr;
    ze_host_mem_alloc_desc_t hostDesc = {};
    result = context->allocHostMem(&hostDesc, 16384u, 4090u, &dstBuffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    int one = 1;
    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    result = commandList->appendMemoryFill(dstBuffer, reinterpret_cast<void *>(&one), sizeof(one), 4090u,
                                           events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();
    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));

    auto allPcCommands = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
    uint32_t dcFlushPipeControls = 0;
    for (auto it : allPcCommands) {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*it);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControls++;
        }
    }
    EXPECT_EQ(2u, dcFlushPipeControls);

    context->freeMem(dstBuffer);
}

HWTEST2_F(CommandListCreate, givenCommandListWhenMemoryFillHavingEventsWithDeviceScopeThenPCDueToWaitEventIsAddedAndPCDueToSignalEventIsAddedWithDCFlush, PlatformSupport) {
    using SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    auto &commandContainer = commandList->getCmdContainer();
    commandContainer.slmSizeRef() = 0;

    void *dstBuffer = nullptr;
    ze_host_mem_alloc_desc_t hostDesc = {};
    result = context->allocHostMem(&hostDesc, 16384u, 4090u, &dstBuffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_DEVICE;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_DEVICE;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    int one = 1;

    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    result = commandList->appendMemoryFill(dstBuffer, reinterpret_cast<void *>(&one), sizeof(one), 4090u,
                                           events[0], 1, &events[1], false);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));

    auto itor = find<SEMAPHORE_WAIT *>(cmdList.begin(), cmdList.end());
    EXPECT_NE(cmdList.end(), itor);
    auto allPcCommands = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
    uint32_t dcFlushPipeControls = 0;
    for (auto it : allPcCommands) {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*it);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControls++;
        }
    }
    EXPECT_EQ(2u, dcFlushPipeControls);

    context->freeMem(dstBuffer);
}

HWTEST2_F(CommandListCreate, givenCommandListWhenMemoryFillHavingEventsWithDeviceScopeThenPCDueToWaitEventIsNotAddedAndPCDueToSignalEventIsAddedWithDCFlush, PlatformSupport) {
    using SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::RenderCompute, 0u, result));
    auto &commandContainer = commandList->getCmdContainer();
    commandContainer.slmSizeRef() = 0;

    void *dstBuffer = nullptr;
    ze_host_mem_alloc_desc_t hostDesc = {};
    result = context->allocHostMem(&hostDesc, 16384u, 4090u, &dstBuffer);
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = 0;
    eventDesc.signal = 0;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    int one = 1;
    size_t usedBefore = commandContainer.getCommandStream()->getUsed();
    result = commandList->appendMemoryFill(dstBuffer, reinterpret_cast<void *>(&one), sizeof(one), 4090u,
                                           events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    size_t usedAfter = commandContainer.getCommandStream()->getUsed();

    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
        cmdList,
        ptrOffset(commandContainer.getCommandStream()->getCpuBase(), usedBefore),
        usedAfter - usedBefore));

    auto allPcCommands = findAll<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
    uint32_t dcFlushPipeControls = 0;
    for (auto it : allPcCommands) {
        auto cmd = genCmdCast<PIPE_CONTROL *>(*it);
        if (cmd->getDcFlushEnable()) {
            dcFlushPipeControls++;
        }
    }
    EXPECT_EQ(1u, dcFlushPipeControls);

    context->freeMem(dstBuffer);
}

HWTEST2_F(CommandListCreate, givenCommandListWhenMemoryCopyRegionWithSignalAndWaitEventsUsingCopyEngineThenSuccessIsReturned, IsAtLeastSkl) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::Copy, 0u, result));

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);
    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};
    result = commandList->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                 srcBuffer, &sr, width, 0, events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

HWTEST2_F(CommandListCreate, givenCommandListWhenMemoryCopyRegionWithSignalAndInvalidWaitHandleUsingCopyEngineThenErrorIsReturned, IsAtLeastSkl) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::Copy, 0u, result));

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);
    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};
    result = commandList->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                 srcBuffer, &sr, width, 0, events[0], 1, nullptr, false);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, result);
}

HWTEST2_F(CommandListCreate, givenCommandListWhenMemoryCopyRegionHasEmptyRegionWithSignalAndWaitEventsUsingCopyEngineThenSuccessIsReturned, IsAtLeastSkl) {
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList(CommandList::create(productFamily, device, NEO::EngineGroupType::Copy, 0u, result));

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    // set regions to 0
    ze_copy_region_t sr = {0U, 0U, 0U, 0U, 0U, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, 0U, 0U, 0U};
    result = commandList->appendMemoryCopyRegion(dstBuffer, &dr, 0, 0,
                                                 srcBuffer, &sr, 0, 0, events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

HWTEST2_F(CommandListCreate, givenImmediateCommandListWhenMemoryCopyRegionWithSignalAndWaitEventsUsingRenderEngineThenSuccessIsReturned, IsAtLeastSkl) {
    const ze_command_queue_desc_t desc = {};
    bool internalEngine = true;

    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList0(CommandList::createImmediate(productFamily,
                                                                               device,
                                                                               &desc,
                                                                               internalEngine,
                                                                               NEO::EngineGroupType::RenderCompute,
                                                                               result));
    ASSERT_NE(nullptr, commandList0);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList0.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalEngine().commandStreamReceiver);

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);
    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};
    result = commandList0->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                  srcBuffer, &sr, width, 0, events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(CommandListCreate, givenImmediateCommandListWhenMemoryCopyRegionWithSignalAndWaitEventsUsingRenderEngineInALoopThenSuccessIsReturned) {
    const ze_command_queue_desc_t desc = {};
    bool internalEngine = true;

    ze_result_t ret = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList0(CommandList::createImmediate(productFamily,
                                                                               device,
                                                                               &desc,
                                                                               internalEngine,
                                                                               NEO::EngineGroupType::RenderCompute,
                                                                               ret));
    ASSERT_NE(nullptr, commandList0);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList0.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalEngine().commandStreamReceiver);

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);
    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, ret));
    EXPECT_EQ(ZE_RESULT_SUCCESS, ret);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(getHelper<L0GfxCoreHelper>().createEvent(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(getHelper<L0GfxCoreHelper>().createEvent(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};

    for (auto i = 0; i < 2000; i++) {
        ret = commandList0->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                   srcBuffer, &sr, width, 0, events[0], 1, &events[1], false);
    }
    EXPECT_EQ(ZE_RESULT_SUCCESS, ret);
}

HWTEST2_F(CommandListCreate, givenImmediateCommandListWhenMemoryCopyRegionWithSignalAndWaitEventsUsingCopyEngineThenSuccessIsReturned, IsAtLeastSkl) {
    const ze_command_queue_desc_t desc = {};
    bool internalEngine = true;

    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList0(CommandList::createImmediate(productFamily,
                                                                               device,
                                                                               &desc,
                                                                               internalEngine,
                                                                               NEO::EngineGroupType::Copy,
                                                                               returnValue));
    ASSERT_NE(nullptr, commandList0);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList0.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    if (neoDevice->getInternalCopyEngine()) {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalCopyEngine()->commandStreamReceiver);
    } else {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalEngine().commandStreamReceiver);
    }

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);
    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};
    auto result = commandList0->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                       srcBuffer, &sr, width, 0, events[0], 1, &events[1], false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

struct CommandListCreateWithBcs : public CommandListCreate {
    void SetUp() override {
        VariableBackup<HardwareInfo> backupHwInfo(defaultHwInfo.get());
        defaultHwInfo->capabilityTable.blitterOperationsSupported = true;
        defaultHwInfo->featureTable.ftrBcsInfo.set(0, true);
        CommandListCreate::SetUp();
    }
};
HWTEST2_F(CommandListCreateWithBcs, givenImmediateCommandListWhenCopyRegionFromImageToImageUsingRenderThenSuccessIsReturned, IsAtLeastXeHpCore) {
    const ze_command_queue_desc_t queueDesc = {};
    bool internalEngine = true;

    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList0(CommandList::createImmediate(productFamily,
                                                                               device,
                                                                               &queueDesc,
                                                                               internalEngine,
                                                                               NEO::EngineGroupType::Copy,
                                                                               returnValue));
    ASSERT_NE(nullptr, commandList0);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList0.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    if (neoDevice->getInternalCopyEngine()) {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalCopyEngine()->commandStreamReceiver);
    } else {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalEngine().commandStreamReceiver);
    }

    ze_image_desc_t desc = {};
    desc.stype = ZE_STRUCTURE_TYPE_IMAGE_DESC;
    desc.type = ZE_IMAGE_TYPE_3D;
    desc.format.layout = ZE_IMAGE_FORMAT_LAYOUT_8_8_8_8;
    desc.format.type = ZE_IMAGE_FORMAT_TYPE_UINT;
    desc.width = 11;
    desc.height = 13;
    desc.depth = 17;

    desc.format.x = ZE_IMAGE_FORMAT_SWIZZLE_A;
    desc.format.y = ZE_IMAGE_FORMAT_SWIZZLE_0;
    desc.format.z = ZE_IMAGE_FORMAT_SWIZZLE_1;
    desc.format.w = ZE_IMAGE_FORMAT_SWIZZLE_X;
    auto imageHWSrc = std::make_unique<WhiteBox<::L0::ImageCoreFamily<gfxCoreFamily>>>();
    auto imageHWDst = std::make_unique<WhiteBox<::L0::ImageCoreFamily<gfxCoreFamily>>>();
    imageHWSrc->initialize(device, &desc);
    imageHWDst->initialize(device, &desc);

    ze_image_region_t srcRegion = {4, 4, 4, 2, 2, 2};
    ze_image_region_t dstRegion = {4, 4, 4, 2, 2, 2};
    returnValue = commandList0->appendImageCopyRegion(imageHWDst->toHandle(), imageHWSrc->toHandle(), &dstRegion, &srcRegion, nullptr, 0, nullptr, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
}

HWTEST2_F(CommandListCreateWithBcs, givenImmediateCommandListWhenCopyRegionFromImageToImageUsingCopyWintInvalidRegionArguementsThenErrorIsReturned, IsAtLeastXeHpCore) {
    const ze_command_queue_desc_t queueDesc = {};
    bool internalEngine = true;

    neoDevice->getRootDeviceEnvironment().getMutableHardwareInfo()->capabilityTable.blitterOperationsSupported = true;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList0(CommandList::createImmediate(productFamily,
                                                                               device,
                                                                               &queueDesc,
                                                                               internalEngine,
                                                                               NEO::EngineGroupType::Copy,
                                                                               returnValue));
    ASSERT_NE(nullptr, commandList0);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList0.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    if (neoDevice->getInternalCopyEngine()) {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalCopyEngine()->commandStreamReceiver);
    } else {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalEngine().commandStreamReceiver);
    }

    ze_image_desc_t desc = {};
    desc.stype = ZE_STRUCTURE_TYPE_IMAGE_DESC;
    desc.type = ZE_IMAGE_TYPE_3D;
    desc.format.layout = ZE_IMAGE_FORMAT_LAYOUT_8_8_8_8;
    desc.format.type = ZE_IMAGE_FORMAT_TYPE_UINT;
    desc.width = 11;
    desc.height = 13;
    desc.depth = 17;

    desc.format.x = ZE_IMAGE_FORMAT_SWIZZLE_A;
    desc.format.y = ZE_IMAGE_FORMAT_SWIZZLE_0;
    desc.format.z = ZE_IMAGE_FORMAT_SWIZZLE_1;
    desc.format.w = ZE_IMAGE_FORMAT_SWIZZLE_X;

    auto imageHWSrc = std::make_unique<WhiteBox<::L0::ImageCoreFamily<gfxCoreFamily>>>();
    auto imageHWDst = std::make_unique<WhiteBox<::L0::ImageCoreFamily<gfxCoreFamily>>>();
    imageHWSrc->initialize(device, &desc);
    imageHWDst->initialize(device, &desc);

    ze_image_region_t srcRegion = {4, 4, 4, 2, 2, 2};
    ze_image_region_t dstRegion = {2, 2, 2, 4, 4, 4};
    returnValue = commandList0->appendImageCopyRegion(imageHWDst->toHandle(), imageHWSrc->toHandle(), &dstRegion, &srcRegion, nullptr, 0, nullptr, false);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, returnValue);
}

HWTEST2_F(CommandListCreateWithBcs, givenImmediateCommandListWhenCopyFromImageToImageUsingRenderThenSuccessIsReturned, IsAtLeastXeHpCore) {
    const ze_command_queue_desc_t queueDesc = {};
    bool internalEngine = true;

    neoDevice->getRootDeviceEnvironment().getMutableHardwareInfo()->capabilityTable.blitterOperationsSupported = true;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList0(CommandList::createImmediate(productFamily,
                                                                               device,
                                                                               &queueDesc,
                                                                               internalEngine,
                                                                               NEO::EngineGroupType::Copy,
                                                                               returnValue));
    ASSERT_NE(nullptr, commandList0);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList0.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    if (neoDevice->getInternalCopyEngine()) {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalCopyEngine()->commandStreamReceiver);
    } else {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalEngine().commandStreamReceiver);
    }

    ze_image_desc_t desc = {};
    desc.stype = ZE_STRUCTURE_TYPE_IMAGE_DESC;
    desc.type = ZE_IMAGE_TYPE_3D;
    desc.format.layout = ZE_IMAGE_FORMAT_LAYOUT_8_8_8_8;
    desc.format.type = ZE_IMAGE_FORMAT_TYPE_UINT;
    desc.width = 11;
    desc.height = 13;
    desc.depth = 17;

    desc.format.x = ZE_IMAGE_FORMAT_SWIZZLE_A;
    desc.format.y = ZE_IMAGE_FORMAT_SWIZZLE_0;
    desc.format.z = ZE_IMAGE_FORMAT_SWIZZLE_1;
    desc.format.w = ZE_IMAGE_FORMAT_SWIZZLE_X;

    auto imageHWSrc = std::make_unique<WhiteBox<::L0::ImageCoreFamily<gfxCoreFamily>>>();
    auto imageHWDst = std::make_unique<WhiteBox<::L0::ImageCoreFamily<gfxCoreFamily>>>();
    imageHWSrc->initialize(device, &desc);
    imageHWDst->initialize(device, &desc);

    returnValue = commandList0->appendImageCopy(imageHWDst->toHandle(), imageHWSrc->toHandle(), nullptr, 0, nullptr, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
}

HWTEST2_F(CommandListCreateWithBcs, givenImmediateCommandListWhenMemoryCopyRegionWithSignalAndInvalidWaitHandleUsingCopyEngineThenErrorIsReturned, IsAtLeastSkl) {
    const ze_command_queue_desc_t desc = {};
    bool internalEngine = true;

    neoDevice->getRootDeviceEnvironment().getMutableHardwareInfo()->capabilityTable.blitterOperationsSupported = true;
    ze_result_t result = ZE_RESULT_SUCCESS;
    std::unique_ptr<L0::CommandList> commandList0(CommandList::createImmediate(productFamily,
                                                                               device,
                                                                               &desc,
                                                                               internalEngine,
                                                                               NEO::EngineGroupType::Copy,
                                                                               result));
    ASSERT_NE(nullptr, commandList0);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList0.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    if (neoDevice->getInternalCopyEngine()) {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalCopyEngine()->commandStreamReceiver);
    } else {
        EXPECT_EQ(cmdQueue->getCsr(), neoDevice->getInternalEngine().commandStreamReceiver);
    }

    void *srcBuffer = reinterpret_cast<void *>(0x1234);
    void *dstBuffer = reinterpret_cast<void *>(0x2345);
    uint32_t width = 16;
    uint32_t height = 16;

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 2;
    auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, result));
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    std::vector<ze_event_handle_t> events;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
    auto event = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event.get());
    eventDesc.index = 1;
    auto event1 = std::unique_ptr<L0::Event>(L0::Event::create<typename FamilyType::TimestampPacketType>(eventPool.get(), &eventDesc, device));
    events.push_back(event1.get());

    ze_copy_region_t sr = {0U, 0U, 0U, width, height, 0U};
    ze_copy_region_t dr = {0U, 0U, 0U, width, height, 0U};
    result = commandList0->appendMemoryCopyRegion(dstBuffer, &dr, width, 0,
                                                  srcBuffer, &sr, width, 0, events[0], 1, nullptr, false);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, result);
}

TEST_F(CommandListCreate, whenCreatingImmCmdListWithASyncModeAndAppendSignalEventWithTimestampThenUpdateTaskCountNeededFlagIsDisabled) {
    ze_command_queue_desc_t desc = {};
    desc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily, device, &desc, false, NEO::EngineGroupType::RenderCompute, returnValue));
    ASSERT_NE(nullptr, commandList);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList.get());

    EXPECT_EQ(device, commandList->getDevice());
    EXPECT_EQ(1u, commandList->getCmdListType());
    EXPECT_NE(nullptr, whiteBoxCmdList->cmdQImmediate);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE | ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.signal = 0;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;

    ze_event_handle_t event = nullptr;

    std::unique_ptr<L0::EventPool> eventPool(EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    ASSERT_NE(nullptr, eventPool);

    eventPool->createEvent(&eventDesc, &event);

    std::unique_ptr<Event> eventObject(static_cast<Event *>(L0::Event::fromHandle(event)));
    ASSERT_NE(nullptr, eventObject->csrs[0]);
    ASSERT_EQ(device->getNEODevice()->getDefaultEngine().commandStreamReceiver, eventObject->csrs[0]);

    commandList->appendSignalEvent(event);

    auto result = eventObject->hostSignal();
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(eventObject->queryStatus(), ZE_RESULT_SUCCESS);
}

TEST_F(CommandListCreate, whenCreatingImmCmdListWithASyncModeAndAppendBarrierThenUpdateTaskCountNeededFlagIsDisabled) {
    ze_command_queue_desc_t desc = {};
    desc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily, device, &desc, false, NEO::EngineGroupType::RenderCompute, returnValue));
    ASSERT_NE(nullptr, commandList);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList.get());

    EXPECT_EQ(device, commandList->getDevice());
    EXPECT_EQ(1u, commandList->getCmdListType());
    EXPECT_NE(nullptr, whiteBoxCmdList->cmdQImmediate);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE | ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;

    ze_event_handle_t event = nullptr;

    std::unique_ptr<L0::EventPool> eventPool(EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    ASSERT_NE(nullptr, eventPool);

    eventPool->createEvent(&eventDesc, &event);

    std::unique_ptr<Event> eventObject(static_cast<Event *>(L0::Event::fromHandle(event)));
    ASSERT_NE(nullptr, eventObject->csrs[0]);
    ASSERT_EQ(device->getNEODevice()->getDefaultEngine().commandStreamReceiver, eventObject->csrs[0]);

    commandList->appendBarrier(event, 0, nullptr);

    auto result = eventObject->hostSignal();
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(eventObject->queryStatus(), ZE_RESULT_SUCCESS);

    commandList->appendBarrier(event, 0, nullptr);
}

TEST_F(CommandListCreate, whenCreatingImmCmdListWithASyncModeAndAppendEventResetThenUpdateTaskCountNeededFlagIsDisabled) {
    ze_command_queue_desc_t desc = {};
    desc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily, device, &desc, false, NEO::EngineGroupType::RenderCompute, returnValue));
    ASSERT_NE(nullptr, commandList);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList.get());

    EXPECT_EQ(device, commandList->getDevice());
    EXPECT_EQ(1u, commandList->getCmdListType());
    EXPECT_NE(nullptr, whiteBoxCmdList->cmdQImmediate);

    ze_event_pool_desc_t eventPoolDesc = {};
    eventPoolDesc.count = 1;
    eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE | ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;

    ze_event_desc_t eventDesc = {};
    eventDesc.index = 0;
    eventDesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
    eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;

    ze_event_handle_t event = nullptr;

    std::unique_ptr<L0::EventPool> eventPool(EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
    EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
    ASSERT_NE(nullptr, eventPool);

    eventPool->createEvent(&eventDesc, &event);

    std::unique_ptr<Event> eventObject(static_cast<Event *>(L0::Event::fromHandle(event)));
    ASSERT_NE(nullptr, eventObject->csrs[0]);
    ASSERT_EQ(device->getNEODevice()->getDefaultEngine().commandStreamReceiver, eventObject->csrs[0]);

    commandList->appendEventReset(event);

    auto result = eventObject->hostSignal();
    ASSERT_EQ(ZE_RESULT_SUCCESS, result);

    EXPECT_EQ(eventObject->queryStatus(), ZE_RESULT_SUCCESS);
}

TEST_F(CommandListCreateWithBcs, givenQueueDescriptionwhenCreatingImmediateCommandListForCopyEnigneThenItHasImmediateCommandQueueCreated) {
    auto &engineGroups = neoDevice->getRegularEngineGroups();
    for (uint32_t ordinal = 0; ordinal < engineGroups.size(); ordinal++) {
        for (uint32_t index = 0; index < engineGroups[ordinal].engines.size(); index++) {
            ze_command_queue_desc_t desc = {};
            desc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
            desc.ordinal = ordinal;
            desc.index = index;
            ze_result_t returnValue;
            std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily, device, &desc, false, NEO::EngineGroupType::Copy, returnValue));
            ASSERT_NE(nullptr, commandList);
            auto whiteBoxCmdList = static_cast<CommandList *>(commandList.get());

            EXPECT_EQ(device, commandList->getDevice());
            EXPECT_EQ(CommandList::CommandListType::TYPE_IMMEDIATE, commandList->getCmdListType());
            EXPECT_NE(nullptr, whiteBoxCmdList->cmdQImmediate);

            ze_event_pool_desc_t eventPoolDesc = {};
            eventPoolDesc.count = 3;
            eventPoolDesc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE;

            ze_event_desc_t eventDesc = {};
            eventDesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
            eventDesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
            auto eventPool = std::unique_ptr<L0::EventPool>(L0::EventPool::create(driverHandle.get(), context, 0, nullptr, &eventPoolDesc, returnValue));
            EXPECT_EQ(ZE_RESULT_SUCCESS, returnValue);
            auto event = std::unique_ptr<L0::Event>(getHelper<L0GfxCoreHelper>().createEvent(eventPool.get(), &eventDesc, device));
            auto event1 = std::unique_ptr<L0::Event>(getHelper<L0GfxCoreHelper>().createEvent(eventPool.get(), &eventDesc, device));
            auto event2 = std::unique_ptr<L0::Event>(getHelper<L0GfxCoreHelper>().createEvent(eventPool.get(), &eventDesc, device));
            ze_event_handle_t events[] = {event1->toHandle(), event2->toHandle()};

            commandList->appendBarrier(nullptr, 0, nullptr);
            commandList->appendBarrier(event->toHandle(), 2, events);

            auto result = event->hostSignal();
            ASSERT_EQ(ZE_RESULT_SUCCESS, result);
            result = event1->hostSignal();
            ASSERT_EQ(ZE_RESULT_SUCCESS, result);
            result = event2->hostSignal();
            ASSERT_EQ(ZE_RESULT_SUCCESS, result);
        }
    }
}

HWTEST2_F(CommandListCreateWithBcs,
          givenInternalImmediateCommandListCreatedAsLinkedCopyWhenUsingInternalCopyEngineThenSelectCopyTypeCommandList, IsAtLeastXeHpCore) {
    const ze_command_queue_desc_t queueDesc = {};
    bool internalEngine = true;

    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily,
                                                                              device,
                                                                              &queueDesc,
                                                                              internalEngine,
                                                                              NEO::EngineGroupType::LinkedCopy,
                                                                              returnValue));
    ASSERT_NE(nullptr, commandList);
    auto whiteBoxCmdList = static_cast<CommandList *>(commandList.get());

    CommandQueueImp *cmdQueue = reinterpret_cast<CommandQueueImp *>(whiteBoxCmdList->cmdQImmediate);
    auto internalCopyEngine = neoDevice->getInternalCopyEngine();
    EXPECT_NE(nullptr, internalCopyEngine);
    EXPECT_EQ(cmdQueue->getCsr(), internalCopyEngine->commandStreamReceiver);
    EXPECT_TRUE(commandList->isCopyOnly());
}

HWTEST2_F(CommandListCreateWithBcs, givenForceFlushTaskEnabledWhenCreatingCommandListUsingLinkedCopyThenFlushTaskModeUsed, IsAtLeastXeHpCore) {
    DebugManagerStateRestore restorer;
    NEO::DebugManager.flags.EnableFlushTaskSubmission.set(1);

    const ze_command_queue_desc_t queueDesc = {};
    bool internalEngine = false;

    ze_result_t returnValue;
    std::unique_ptr<L0::CommandList> commandList(CommandList::createImmediate(productFamily,
                                                                              device,
                                                                              &queueDesc,
                                                                              internalEngine,
                                                                              NEO::EngineGroupType::LinkedCopy,
                                                                              returnValue));
    ASSERT_NE(nullptr, commandList);

    EXPECT_TRUE(commandList->isCopyOnly());
    EXPECT_TRUE(commandList->flushTaskSubmissionEnabled());
}

HWTEST2_F(CommandListCreate, whenGettingCommandsToPatchThenCorrectValuesAreReturned, IsAtLeastSkl) {
    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    EXPECT_EQ(&commandList->requiredStreamState, &commandList->getRequiredStreamState());
    EXPECT_EQ(&commandList->finalStreamState, &commandList->getFinalStreamState());
    EXPECT_EQ(&commandList->commandsToPatch, &commandList->getCommandsToPatch());
}

HWTEST2_F(CommandListCreate, givenNonEmptyCommandsToPatchWhenClearCommandsToPatchIsCalledThenCommandsAreCorrectlyCleared, IsAtLeastSkl) {
    using VFE_STATE_TYPE = typename FamilyType::VFE_STATE_TYPE;

    auto pCommandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    EXPECT_TRUE(pCommandList->commandsToPatch.empty());
    EXPECT_NO_THROW(pCommandList->clearCommandsToPatch());
    EXPECT_TRUE(pCommandList->commandsToPatch.empty());

    CommandList::CommandToPatch commandToPatch{};
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_ANY_THROW(pCommandList->clearCommandsToPatch());
    pCommandList->commandsToPatch.clear();

    commandToPatch.type = CommandList::CommandToPatch::CommandType::FrontEndState;
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_ANY_THROW(pCommandList->clearCommandsToPatch());
    pCommandList->commandsToPatch.clear();

    commandToPatch.pCommand = new VFE_STATE_TYPE;
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_NO_THROW(pCommandList->clearCommandsToPatch());
    EXPECT_TRUE(pCommandList->commandsToPatch.empty());

    commandToPatch = {};
    commandToPatch.type = CommandList::CommandToPatch::PauseOnEnqueueSemaphoreStart;
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_ANY_THROW(pCommandList->clearCommandsToPatch());
    pCommandList->commandsToPatch.clear();

    commandToPatch.pCommand = reinterpret_cast<void *>(0x1234);
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_NO_THROW(pCommandList->clearCommandsToPatch());
    EXPECT_TRUE(pCommandList->commandsToPatch.empty());

    commandToPatch = {};
    commandToPatch.type = CommandList::CommandToPatch::PauseOnEnqueueSemaphoreEnd;
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_ANY_THROW(pCommandList->clearCommandsToPatch());
    pCommandList->commandsToPatch.clear();

    commandToPatch.pCommand = reinterpret_cast<void *>(0x1234);
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_NO_THROW(pCommandList->clearCommandsToPatch());
    EXPECT_TRUE(pCommandList->commandsToPatch.empty());

    commandToPatch = {};
    commandToPatch.type = CommandList::CommandToPatch::PauseOnEnqueuePipeControlStart;
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_ANY_THROW(pCommandList->clearCommandsToPatch());
    pCommandList->commandsToPatch.clear();

    commandToPatch.pCommand = reinterpret_cast<void *>(0x1234);
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_NO_THROW(pCommandList->clearCommandsToPatch());
    EXPECT_TRUE(pCommandList->commandsToPatch.empty());

    commandToPatch = {};
    commandToPatch.type = CommandList::CommandToPatch::PauseOnEnqueuePipeControlEnd;
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_ANY_THROW(pCommandList->clearCommandsToPatch());
    pCommandList->commandsToPatch.clear();

    commandToPatch.pCommand = reinterpret_cast<void *>(0x1234);
    pCommandList->commandsToPatch.push_back(commandToPatch);
    EXPECT_NO_THROW(pCommandList->clearCommandsToPatch());
    EXPECT_TRUE(pCommandList->commandsToPatch.empty());
}

template <NEO::AllocationType AllocType>
class MyDeviceMock : public Mock<DeviceImp> {
  public:
    using Mock<L0::DeviceImp>::Mock;
    NEO::GraphicsAllocation *allocateMemoryFromHostPtr(const void *buffer, size_t size, bool hostCopyAllowed) override {
        auto alloc = std::make_unique<NEO::MockGraphicsAllocation>(const_cast<void *>(buffer), reinterpret_cast<uintptr_t>(buffer), size);
        alloc->allocationType = AllocType;
        return alloc.release();
    }
    const NEO::HardwareInfo &getHwInfo() const override {
        return neoDevice->getHardwareInfo();
    }
};

HWTEST2_F(CommandListCreate, givenHostPtrAllocAllocWhenInternalMemCreatedThenNewAllocAddedToDealocationContainer, IsAtLeastSkl) {
    auto myDevice = std::make_unique<MyDeviceMock<NEO::AllocationType::INTERNAL_HOST_MEMORY>>(device->getNEODevice(), execEnv);
    myDevice->neoDevice = device->getNEODevice();
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(myDevice.get(), NEO::EngineGroupType::Copy, 0u);
    auto buffer = std::make_unique<uint8_t>(0x100);

    auto deallocationSize = commandList->commandContainer.getDeallocationContainer().size();
    auto alloc = commandList->getHostPtrAlloc(buffer.get(), 0x80, true);
    EXPECT_EQ(deallocationSize + 1, commandList->commandContainer.getDeallocationContainer().size());
    EXPECT_NE(alloc, nullptr);
    driverHandle->getMemoryManager()->freeGraphicsMemory(alloc);
    commandList->commandContainer.getDeallocationContainer().clear();
}

HWTEST2_F(CommandListCreate, givenHostPtrAllocAllocWhenExternalMemCreatedThenNewAllocAddedToHostPtrMap, IsAtLeastSkl) {
    auto myDevice = std::make_unique<MyDeviceMock<NEO::AllocationType::EXTERNAL_HOST_PTR>>(device->getNEODevice(), execEnv);
    myDevice->neoDevice = device->getNEODevice();
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(myDevice.get(), NEO::EngineGroupType::Copy, 0u);
    auto buffer = std::make_unique<uint8_t>(0x100);

    auto hostPtrMapSize = commandList->getHostPtrMap().size();
    auto alloc = commandList->getHostPtrAlloc(buffer.get(), 0x100, true);
    EXPECT_EQ(hostPtrMapSize + 1, commandList->getHostPtrMap().size());
    EXPECT_NE(alloc, nullptr);
    driverHandle->getMemoryManager()->freeGraphicsMemory(alloc);
    commandList->hostPtrMap.clear();
}

HWTEST2_F(CommandListCreateWithBcs, givenHostPtrAllocAllocAndImmediateCmdListWhenExternalMemCreatedThenNewAllocAddedToInternalAllocationStorage, IsAtLeastSkl) {
    auto myDevice = std::make_unique<MyDeviceMock<NEO::AllocationType::EXTERNAL_HOST_PTR>>(device->getNEODevice(), execEnv);
    myDevice->neoDevice = device->getNEODevice();
    auto commandList = std::make_unique<WhiteBox<L0::CommandListCoreFamilyImmediate<gfxCoreFamily>>>();
    commandList->initialize(myDevice.get(), NEO::EngineGroupType::Copy, 0u);
    commandList->cmdListType = CommandList::CommandListType::TYPE_IMMEDIATE;
    if (neoDevice->getInternalCopyEngine()) {
        commandList->csr = neoDevice->getInternalCopyEngine()->commandStreamReceiver;
    } else {
        commandList->csr = neoDevice->getInternalEngine().commandStreamReceiver;
    }
    auto buffer = std::make_unique<uint8_t>(0x100);

    EXPECT_TRUE(commandList->csr->getInternalAllocationStorage()->getTemporaryAllocations().peekIsEmpty());
    auto alloc = commandList->getHostPtrAlloc(buffer.get(), 0x100, true);
    EXPECT_FALSE(commandList->csr->getInternalAllocationStorage()->getTemporaryAllocations().peekIsEmpty());
    EXPECT_EQ(alloc, commandList->csr->getInternalAllocationStorage()->getTemporaryAllocations().peekHead());
    EXPECT_EQ(commandList->csr->peekTaskCount(), commandList->csr->getInternalAllocationStorage()->getTemporaryAllocations().peekHead()->getTaskCount(commandList->csr->getOsContext().getContextId()));
    EXPECT_EQ(1u, commandList->csr->getInternalAllocationStorage()->getTemporaryAllocations().peekHead()->hostPtrTaskCountAssignment);
}

HWTEST2_F(CommandListCreate, givenGetAlignedAllocationWhenInternalMemWithinDifferentAllocThenReturnNewAlloc, IsAtLeastSkl) {
    auto myDevice = std::make_unique<MyDeviceMock<NEO::AllocationType::INTERNAL_HOST_MEMORY>>(device->getNEODevice(), execEnv);
    myDevice->neoDevice = device->getNEODevice();
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(myDevice.get(), NEO::EngineGroupType::Copy, 0u);
    auto buffer = std::make_unique<uint8_t>(0x100);

    auto outData1 = commandList->getAlignedAllocationData(device, buffer.get(), 0x100, true);
    auto outData2 = commandList->getAlignedAllocationData(device, &buffer.get()[5], 0x1, true);
    EXPECT_NE(outData1.alloc, outData2.alloc);
    driverHandle->getMemoryManager()->freeGraphicsMemory(outData1.alloc);
    driverHandle->getMemoryManager()->freeGraphicsMemory(outData2.alloc);
    commandList->commandContainer.getDeallocationContainer().clear();
}
HWTEST2_F(CommandListCreate, givenGetAlignedAllocationWhenExternalMemWithinDifferentAllocThenReturnPreviouslyAllocatedMem, IsAtLeastSkl) {
    auto myDevice = std::make_unique<MyDeviceMock<NEO::AllocationType::EXTERNAL_HOST_PTR>>(device->getNEODevice(), execEnv);
    myDevice->neoDevice = device->getNEODevice();
    auto commandList = std::make_unique<WhiteBox<::L0::CommandListCoreFamily<gfxCoreFamily>>>();
    commandList->initialize(myDevice.get(), NEO::EngineGroupType::Copy, 0u);
    auto buffer = std::make_unique<uint8_t>(0x100);

    auto outData1 = commandList->getAlignedAllocationData(device, buffer.get(), 0x100, true);
    auto outData2 = commandList->getAlignedAllocationData(device, &buffer.get()[5], 0x1, true);
    EXPECT_EQ(outData1.alloc, outData2.alloc);
    driverHandle->getMemoryManager()->freeGraphicsMemory(outData1.alloc);
    commandList->hostPtrMap.clear();
}

using FrontEndPrimaryBatchBufferCommandListTest = Test<FrontEndCommandListFixture<1>>;
HWTEST2_F(FrontEndPrimaryBatchBufferCommandListTest,
          givenFrontEndTrackingIsUsedWhenPropertyDisableEuFusionSupportedThenExpectFrontEndAddedToPatchlist,
          IsAtLeastXeHpCore) {
    using CFE_STATE = typename FamilyType::CFE_STATE;

    NEO::FrontEndPropertiesSupport fePropertiesSupport = {};
    auto &productHelper = device->getProductHelper();
    productHelper.fillFrontEndPropertiesSupportStructure(fePropertiesSupport, device->getHwInfo());

    EXPECT_TRUE(commandList->frontEndStateTracking);

    auto &cmdStream = *commandList->getCmdContainer().getCommandStream();

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};
    ze_result_t result;
    result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto &commandsToPatch = commandList->commandsToPatch;
    EXPECT_EQ(0u, commandsToPatch.size());

    mockKernelImmData->kernelDescriptor->kernelAttributes.perThreadScratchSize[0] = 0x40;
    mockKernelImmData->kernelDescriptor->kernelAttributes.flags.requiresDisabledEUFusion = 1;

    size_t usedBefore = cmdStream.getUsed();
    result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.disableEuFusion) {
        ASSERT_EQ(1u, commandsToPatch.size());
        CommandList::CommandToPatch &cfePatch = commandsToPatch[0];
        EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfePatch.type);

        void *expectedDestination = ptrOffset(cmdStream.getCpuBase(), usedBefore);
        EXPECT_EQ(expectedDestination, cfePatch.pDestination);

        auto cfeCmd = genCmdCast<CFE_STATE *>(cfePatch.pCommand);
        ASSERT_NE(nullptr, cfeCmd);
        EXPECT_TRUE(NEO::UnitTestHelper<FamilyType>::getDisableFusionStateFromFrontEndCommand(*cfeCmd));
        EXPECT_EQ(0u, cfeCmd->getScratchSpaceBuffer());
    } else {
        EXPECT_EQ(0u, commandsToPatch.size());
    }

    result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.disableEuFusion) {
        EXPECT_EQ(1u, commandsToPatch.size());
    } else {
        EXPECT_EQ(0u, commandsToPatch.size());
    }

    mockKernelImmData->kernelDescriptor->kernelAttributes.flags.requiresDisabledEUFusion = 0;

    usedBefore = cmdStream.getUsed();
    result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.disableEuFusion) {
        ASSERT_EQ(2u, commandsToPatch.size());
        CommandList::CommandToPatch &cfePatch = commandsToPatch[1];
        EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfePatch.type);

        void *expectedDestination = ptrOffset(cmdStream.getCpuBase(), usedBefore);
        EXPECT_EQ(expectedDestination, cfePatch.pDestination);

        auto cfeCmd = genCmdCast<CFE_STATE *>(cfePatch.pCommand);
        ASSERT_NE(nullptr, cfeCmd);
        EXPECT_FALSE(NEO::UnitTestHelper<FamilyType>::getDisableFusionStateFromFrontEndCommand(*cfeCmd));
        EXPECT_EQ(0u, cfeCmd->getScratchSpaceBuffer());
    }

    mockKernelImmData->kernelDescriptor->kernelAttributes.flags.requiresDisabledEUFusion = 1;

    usedBefore = cmdStream.getUsed();
    result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.disableEuFusion) {
        ASSERT_EQ(3u, commandsToPatch.size());
        CommandList::CommandToPatch &cfePatch = commandsToPatch[2];
        EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfePatch.type);

        void *expectedDestination = ptrOffset(cmdStream.getCpuBase(), usedBefore);
        EXPECT_EQ(expectedDestination, cfePatch.pDestination);

        auto cfeCmd = genCmdCast<CFE_STATE *>(cfePatch.pCommand);
        ASSERT_NE(nullptr, cfeCmd);
        EXPECT_TRUE(NEO::UnitTestHelper<FamilyType>::getDisableFusionStateFromFrontEndCommand(*cfeCmd));
        EXPECT_EQ(0u, cfeCmd->getScratchSpaceBuffer());
    } else {
        EXPECT_EQ(0u, commandsToPatch.size());
    }

    result = commandList->close();
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto commandListHandle = commandList->toHandle();
    result = commandQueue->executeCommandLists(1, &commandListHandle, nullptr, true);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.disableEuFusion) {
        ASSERT_EQ(3u, commandsToPatch.size());

        bool disableFusionStates[] = {true, false, true};
        uint32_t disableFusionStatesIdx = 0;

        for (const auto &cfeToPatch : commandsToPatch) {
            EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfeToPatch.type);
            auto cfeCmd = genCmdCast<CFE_STATE *>(cfeToPatch.pDestination);
            ASSERT_NE(nullptr, cfeCmd);

            EXPECT_EQ(disableFusionStates[disableFusionStatesIdx++],
                      NEO::UnitTestHelper<FamilyType>::getDisableFusionStateFromFrontEndCommand(*cfeCmd));
            EXPECT_NE(0u, cfeCmd->getScratchSpaceBuffer());
        }

        result = commandList->reset();
        EXPECT_EQ(ZE_RESULT_SUCCESS, result);
        EXPECT_EQ(0u, commandsToPatch.size());
    }
}

HWTEST2_F(FrontEndPrimaryBatchBufferCommandListTest,
          givenFrontEndTrackingCmdListIsExecutedWhenPropertyComputeDispatchAllWalkerSupportedThenExpectFrontEndAddedToPatchlist,
          IsAtLeastXeHpCore) {
    using CFE_STATE = typename FamilyType::CFE_STATE;

    NEO::FrontEndPropertiesSupport fePropertiesSupport = {};
    auto &productHelper = device->getProductHelper();
    productHelper.fillFrontEndPropertiesSupportStructure(fePropertiesSupport, device->getHwInfo());

    mockKernelImmData->kernelDescriptor->kernelAttributes.perThreadScratchSize[0] = 0x40;

    NEO::DebugManager.flags.AllowMixingRegularAndCooperativeKernels.set(1);

    EXPECT_TRUE(commandList->frontEndStateTracking);
    EXPECT_TRUE(commandQueue->frontEndStateTracking);

    ze_group_count_t groupCount{1, 1, 1};
    CmdListKernelLaunchParams launchParams = {};

    ze_result_t result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto &commandsToPatch = commandList->commandsToPatch;
    EXPECT_EQ(0u, commandsToPatch.size());

    result = commandList->appendLaunchCooperativeKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.computeDispatchAllWalker) {
        ASSERT_EQ(1u, commandsToPatch.size());
        CommandList::CommandToPatch &cfePatch = commandsToPatch[0];
        EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfePatch.type);

        auto cfeCmd = genCmdCast<CFE_STATE *>(cfePatch.pCommand);
        ASSERT_NE(nullptr, cfeCmd);
        EXPECT_TRUE(NEO::UnitTestHelper<FamilyType>::getComputeDispatchAllWalkerFromFrontEndCommand(*cfeCmd));
        EXPECT_EQ(0u, cfeCmd->getScratchSpaceBuffer());
    } else {
        EXPECT_EQ(0u, commandsToPatch.size());
    }

    result = commandList->appendLaunchCooperativeKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.computeDispatchAllWalker) {
        EXPECT_EQ(1u, commandsToPatch.size());
    } else {
        EXPECT_EQ(0u, commandsToPatch.size());
    }

    result = commandList->appendLaunchKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, launchParams, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.computeDispatchAllWalker) {
        ASSERT_EQ(2u, commandsToPatch.size());
        CommandList::CommandToPatch &cfePatch = commandsToPatch[1];
        EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfePatch.type);

        auto cfeCmd = genCmdCast<CFE_STATE *>(cfePatch.pCommand);
        ASSERT_NE(nullptr, cfeCmd);
        EXPECT_FALSE(NEO::UnitTestHelper<FamilyType>::getComputeDispatchAllWalkerFromFrontEndCommand(*cfeCmd));
        EXPECT_EQ(0u, cfeCmd->getScratchSpaceBuffer());
    } else {
        EXPECT_EQ(0u, commandsToPatch.size());
    }

    result = commandList->appendLaunchCooperativeKernel(kernel->toHandle(), &groupCount, nullptr, 0, nullptr, false);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.computeDispatchAllWalker) {
        ASSERT_EQ(3u, commandsToPatch.size());
        CommandList::CommandToPatch &cfePatch = commandsToPatch[2];
        EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfePatch.type);

        auto cfeCmd = genCmdCast<CFE_STATE *>(cfePatch.pCommand);
        ASSERT_NE(nullptr, cfeCmd);
        EXPECT_TRUE(NEO::UnitTestHelper<FamilyType>::getComputeDispatchAllWalkerFromFrontEndCommand(*cfeCmd));
        EXPECT_EQ(0u, cfeCmd->getScratchSpaceBuffer());
    } else {
        EXPECT_EQ(0u, commandsToPatch.size());
    }

    result = commandList->close();
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    auto commandListHandle = commandList->toHandle();
    result = commandQueue->executeCommandLists(1, &commandListHandle, nullptr, true);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    if (fePropertiesSupport.computeDispatchAllWalker) {
        ASSERT_EQ(3u, commandsToPatch.size());

        bool computeDispatchAllWalkerStates[] = {true, false, true};
        uint32_t computeDispatchAllWalkerStatesIdx = 0;

        for (const auto &cfeToPatch : commandsToPatch) {
            EXPECT_EQ(CommandList::CommandToPatch::FrontEndState, cfeToPatch.type);
            auto cfeCmd = genCmdCast<CFE_STATE *>(cfeToPatch.pDestination);
            ASSERT_NE(nullptr, cfeCmd);

            EXPECT_EQ(computeDispatchAllWalkerStates[computeDispatchAllWalkerStatesIdx++],
                      NEO::UnitTestHelper<FamilyType>::getComputeDispatchAllWalkerFromFrontEndCommand(*cfeCmd));
            EXPECT_NE(0u, cfeCmd->getScratchSpaceBuffer());
        }

        result = commandList->reset();
        EXPECT_EQ(ZE_RESULT_SUCCESS, result);
        EXPECT_EQ(0u, commandsToPatch.size());
    }
}

} // namespace ult
} // namespace L0
