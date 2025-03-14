/*
 * Copyright (C) 2022-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/direct_submission/dispatchers/render_dispatcher.h"
#include "shared/source/direct_submission/linux/drm_direct_submission.h"
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/timestamp_packet.h"
#include "shared/source/os_interface/linux/cache_info.h"
#include "shared/source/os_interface/linux/drm_allocation.h"
#include "shared/source/os_interface/linux/drm_buffer_object.h"
#include "shared/source/os_interface/linux/drm_memory_operations_handler_bind.h"
#include "shared/source/os_interface/linux/drm_memory_operations_handler_default.h"
#include "shared/source/os_interface/linux/os_context_linux.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/utilities/tag_allocator.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/helpers/engine_descriptor_helper.h"
#include "shared/test/common/libult/linux/drm_query_mock.h"
#include "shared/test/common/libult/ult_command_stream_receiver.h"
#include "shared/test/common/mocks/linux/mock_drm_memory_manager.h"
#include "shared/test/common/mocks/mock_allocation_properties.h"
#include "shared/test/common/mocks/mock_command_stream_receiver.h"
#include "shared/test/common/mocks/mock_device.h"
#include "shared/test/common/mocks/mock_execution_environment.h"
#include "shared/test/common/mocks/mock_gmm_client_context.h"
#include "shared/test/common/mocks/mock_graphics_allocation.h"
#include "shared/test/common/os_interface/linux/device_command_stream_fixture_prelim.h"
#include "shared/test/common/test_macros/hw_test.h"

#include <memory>

using namespace NEO;

struct MockDrmMemoryOperationsHandlerBind : public DrmMemoryOperationsHandlerBind {
    using DrmMemoryOperationsHandlerBind::DrmMemoryOperationsHandlerBind;
    using DrmMemoryOperationsHandlerBind::evictImpl;

    bool useBaseEvictUnused = true;
    uint32_t evictUnusedCalled = 0;

    MemoryOperationsStatus evictUnusedAllocations(bool waitForCompletion, bool isLockNeeded) override {
        evictUnusedCalled++;
        if (useBaseEvictUnused) {
            return DrmMemoryOperationsHandlerBind::evictUnusedAllocations(waitForCompletion, isLockNeeded);
        }

        return MemoryOperationsStatus::SUCCESS;
    }
    int evictImpl(OsContext *osContext, GraphicsAllocation &gfxAllocation, DeviceBitfield deviceBitfield) override {
        EXPECT_EQ(this->rootDeviceIndex, gfxAllocation.getRootDeviceIndex());
        return DrmMemoryOperationsHandlerBind::evictImpl(osContext, gfxAllocation, deviceBitfield);
    }
};

template <uint32_t numRootDevices>
struct DrmMemoryOperationsHandlerBindFixture : public ::testing::Test {
  public:
    void setUp(bool setPerContextVms) {
        DebugManager.flags.DeferOsContextInitialization.set(0);
        DebugManager.flags.CreateMultipleSubDevices.set(2u);
        VariableBackup<bool> mockDeviceFlagBackup(&MockDevice::createSingleDevice, false);

        executionEnvironment = new ExecutionEnvironment;
        executionEnvironment->prepareRootDeviceEnvironments(numRootDevices);
        for (uint32_t i = 0u; i < numRootDevices; i++) {
            executionEnvironment->rootDeviceEnvironments[i]->setHwInfoAndInitHelpers(defaultHwInfo.get());
            executionEnvironment->rootDeviceEnvironments[i]->initGmm();
        }
        executionEnvironment->calculateMaxOsContextCount();
        for (uint32_t i = 0u; i < numRootDevices; i++) {
            auto mock = new DrmQueryMock(*executionEnvironment->rootDeviceEnvironments[i]);
            mock->setBindAvailable();
            if (setPerContextVms) {
                mock->setPerContextVMRequired(setPerContextVms);
                mock->incrementVmId = true;
            }
            executionEnvironment->rootDeviceEnvironments[i]->osInterface = std::make_unique<OSInterface>();
            executionEnvironment->rootDeviceEnvironments[i]->osInterface->setDriverModel(std::unique_ptr<DriverModel>(mock));
            executionEnvironment->rootDeviceEnvironments[i]->memoryOperationsInterface.reset(new MockDrmMemoryOperationsHandlerBind(*executionEnvironment->rootDeviceEnvironments[i].get(), i));
            executionEnvironment->rootDeviceEnvironments[i]->initGmm();

            devices.emplace_back(MockDevice::createWithExecutionEnvironment<MockDevice>(defaultHwInfo.get(), executionEnvironment, i));
        }
        memoryManager = std::make_unique<TestedDrmMemoryManager>(*executionEnvironment);
        device = devices[0].get();
        mock = executionEnvironment->rootDeviceEnvironments[0]->osInterface->getDriverModel()->as<DrmQueryMock>();
        operationHandler = static_cast<MockDrmMemoryOperationsHandlerBind *>(executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.get());
        memoryManagerBackup = executionEnvironment->memoryManager.release();
        executionEnvironment->memoryManager.reset(memoryManager.get());
        memoryManager->allRegisteredEngines = memoryManagerBackup->getRegisteredEngines();
    }
    void SetUp() override {
        setUp(false);
    }

    void TearDown() override {
        executionEnvironment->memoryManager.release();
        executionEnvironment->memoryManager.reset(memoryManagerBackup);
        for (auto &engineContainer : memoryManager->allRegisteredEngines) {
            engineContainer.clear();
        }
    }

  protected:
    ExecutionEnvironment *executionEnvironment = nullptr;
    MockDevice *device;
    std::vector<std::unique_ptr<MockDevice>> devices;
    std::unique_ptr<TestedDrmMemoryManager> memoryManager;
    MockDrmMemoryOperationsHandlerBind *operationHandler = nullptr;
    DebugManagerStateRestore restorer;
    DrmQueryMock *mock;
    MemoryManager *memoryManagerBackup;
};

using DrmMemoryOperationsHandlerBindMultiRootDeviceTest = DrmMemoryOperationsHandlerBindFixture<2u>;

TEST_F(DrmMemoryOperationsHandlerBindMultiRootDeviceTest, whenSetNewResourceBoundToVMThenAllContextsUsingThatVMHasSetNewResourceBound) {
    struct MockOsContextLinux : OsContextLinux {
        using OsContextLinux::lastFlushedTlbFlushCounter;
    };

    BufferObject mockBo(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    mock->setNewResourceBoundToVM(&mockBo, 1u);

    for (const auto &engine : device->getAllEngines()) {
        auto osContexLinux = static_cast<MockOsContextLinux *>(engine.osContext);
        if (osContexLinux->getDeviceBitfield().test(1u) && executionEnvironment->rootDeviceEnvironments[device->getRootDeviceIndex()]->getProductHelper().isTlbFlushRequired()) {
            EXPECT_TRUE(osContexLinux->isTlbFlushRequired());
        } else {
            EXPECT_FALSE(osContexLinux->isTlbFlushRequired());
        }

        osContexLinux->lastFlushedTlbFlushCounter.store(osContexLinux->peekTlbFlushCounter());
    }
    for (const auto &engine : devices[1]->getAllEngines()) {
        auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
        EXPECT_FALSE(osContexLinux->isTlbFlushRequired());
    }

    auto mock2 = executionEnvironment->rootDeviceEnvironments[1u]->osInterface->getDriverModel()->as<DrmQueryMock>();
    BufferObject mockBo2(devices[1]->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    mock2->setNewResourceBoundToVM(&mockBo2, 0u);

    for (const auto &engine : devices[1]->getAllEngines()) {
        auto osContexLinux = static_cast<MockOsContextLinux *>(engine.osContext);
        if (osContexLinux->getDeviceBitfield().test(0u) && executionEnvironment->rootDeviceEnvironments[1]->getProductHelper().isTlbFlushRequired()) {
            EXPECT_TRUE(osContexLinux->isTlbFlushRequired());
        } else {
            EXPECT_FALSE(osContexLinux->isTlbFlushRequired());
        }

        osContexLinux->lastFlushedTlbFlushCounter.store(osContexLinux->peekTlbFlushCounter());
    }
    for (const auto &engine : device->getAllEngines()) {
        auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
        EXPECT_FALSE(osContexLinux->isTlbFlushRequired());
    }

    mockBo.setAddress(0x1234);
    mock->setNewResourceBoundToVM(&mockBo, 1u);

    for (const auto &engine : device->getAllEngines()) {
        auto osContexLinux = static_cast<MockOsContextLinux *>(engine.osContext);
        if (osContexLinux->getDeviceBitfield().test(1u)) {
            EXPECT_TRUE(osContexLinux->isTlbFlushRequired());
        }

        osContexLinux->lastFlushedTlbFlushCounter.store(osContexLinux->peekTlbFlushCounter());
    }
    for (const auto &engine : devices[1]->getAllEngines()) {
        auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
        EXPECT_FALSE(osContexLinux->isTlbFlushRequired());
    }
}

template <uint32_t numRootDevices>
struct DrmMemoryOperationsHandlerBindFixture2 : public ::testing::Test {
  public:
    void setUp(bool setPerContextVms) {
        DebugManager.flags.DeferOsContextInitialization.set(0);
        DebugManager.flags.CreateMultipleSubDevices.set(2u);
        VariableBackup<bool> mockDeviceFlagBackup(&MockDevice::createSingleDevice, false);

        executionEnvironment = new ExecutionEnvironment;
        executionEnvironment->prepareRootDeviceEnvironments(numRootDevices);
        for (uint32_t i = 0u; i < numRootDevices; i++) {
            executionEnvironment->rootDeviceEnvironments[i]->setHwInfoAndInitHelpers(defaultHwInfo.get());
            executionEnvironment->rootDeviceEnvironments[i]->initGmm();
        }
        executionEnvironment->calculateMaxOsContextCount();
        for (uint32_t i = 0u; i < numRootDevices; i++) {
            auto mock = new DrmQueryMock(*executionEnvironment->rootDeviceEnvironments[i]);
            mock->setBindAvailable();
            if (setPerContextVms) {
                mock->setPerContextVMRequired(setPerContextVms);
                mock->incrementVmId = true;
            }
            executionEnvironment->rootDeviceEnvironments[i]->osInterface = std::make_unique<OSInterface>();
            executionEnvironment->rootDeviceEnvironments[i]->osInterface->setDriverModel(std::unique_ptr<DriverModel>(mock));
            if (i == 0) {
                executionEnvironment->rootDeviceEnvironments[i]->memoryOperationsInterface.reset(new DrmMemoryOperationsHandlerDefault(i));
            } else {
                executionEnvironment->rootDeviceEnvironments[i]->memoryOperationsInterface.reset(new MockDrmMemoryOperationsHandlerBind(*executionEnvironment->rootDeviceEnvironments[i].get(), i));
            }
            executionEnvironment->rootDeviceEnvironments[i]->initGmm();

            devices.emplace_back(MockDevice::createWithExecutionEnvironment<MockDevice>(defaultHwInfo.get(), executionEnvironment, i));
        }
        memoryManager = std::make_unique<TestedDrmMemoryManager>(*executionEnvironment);
        deviceDefault = devices[0].get();
        device = devices[1].get();
        mockDefault = executionEnvironment->rootDeviceEnvironments[0]->osInterface->getDriverModel()->as<DrmQueryMock>();
        mock = executionEnvironment->rootDeviceEnvironments[1]->osInterface->getDriverModel()->as<DrmQueryMock>();
        operationHandlerDefault = static_cast<DrmMemoryOperationsHandlerDefault *>(executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.get());
        operationHandler = static_cast<MockDrmMemoryOperationsHandlerBind *>(executionEnvironment->rootDeviceEnvironments[1]->memoryOperationsInterface.get());
        memoryManagerBackup = executionEnvironment->memoryManager.release();
        executionEnvironment->memoryManager.reset(memoryManager.get());
        memoryManager->allRegisteredEngines = memoryManagerBackup->getRegisteredEngines();
    }
    void SetUp() override {
        setUp(false);
    }

    void TearDown() override {
        executionEnvironment->memoryManager.release();
        executionEnvironment->memoryManager.reset(memoryManagerBackup);
        for (auto &engineContainer : memoryManager->allRegisteredEngines) {
            engineContainer.clear();
        }
    }

  protected:
    ExecutionEnvironment *executionEnvironment = nullptr;
    MockDevice *device = nullptr;
    MockDevice *deviceDefault = nullptr;
    std::vector<std::unique_ptr<MockDevice>> devices;
    std::unique_ptr<TestedDrmMemoryManager> memoryManager;
    DrmMemoryOperationsHandlerDefault *operationHandlerDefault = nullptr;
    MockDrmMemoryOperationsHandlerBind *operationHandler = nullptr;
    DebugManagerStateRestore restorer;
    DrmQueryMock *mock = nullptr;
    DrmQueryMock *mockDefault = nullptr;
    MemoryManager *memoryManagerBackup = nullptr;
};

using DrmMemoryOperationsHandlerBindMultiRootDeviceTest2 = DrmMemoryOperationsHandlerBindFixture2<2u>;

TEST_F(DrmMemoryOperationsHandlerBindMultiRootDeviceTest2, givenOperationHandlersWhenRootDeviceIndexIsChangedThenEvictSucceeds) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    auto allocationDefault = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{deviceDefault->getRootDeviceIndex(), MemoryConstants::pageSize});

    EXPECT_EQ(operationHandlerDefault->getRootDeviceIndex(), 0u);
    EXPECT_EQ(operationHandler->getRootDeviceIndex(), 1u);

    operationHandlerDefault->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocationDefault, 1));
    operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1));

    operationHandlerDefault->setRootDeviceIndex(1u);
    operationHandler->setRootDeviceIndex(0u);
    EXPECT_EQ(operationHandlerDefault->getRootDeviceIndex(), 1u);
    EXPECT_EQ(operationHandler->getRootDeviceIndex(), 0u);

    EXPECT_EQ(operationHandlerDefault->evict(device, *allocation), MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->evict(device, *allocationDefault), MemoryOperationsStatus::SUCCESS);

    operationHandlerDefault->setRootDeviceIndex(0u);
    operationHandler->setRootDeviceIndex(1u);

    memoryManager->freeGraphicsMemory(allocationDefault);
    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindMultiRootDeviceTest2, whenNoSpaceLeftOnDeviceThenEvictUnusedAllocations) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    auto allocationDefault = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{deviceDefault->getRootDeviceIndex(), MemoryConstants::pageSize});
    mock->context.vmBindReturn = -1;
    mock->baseErrno = false;
    mock->errnoRetVal = ENOSPC;
    operationHandler->useBaseEvictUnused = true;

    auto registeredAllocations = memoryManager->getSysMemAllocs();
    EXPECT_EQ(2u, registeredAllocations.size());

    EXPECT_EQ(allocation, registeredAllocations[0]);
    EXPECT_EQ(allocationDefault, registeredAllocations[1]);

    EXPECT_EQ(operationHandler->evictUnusedCalled, 0u);
    auto res = operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1));
    EXPECT_EQ(MemoryOperationsStatus::OUT_OF_MEMORY, res);
    EXPECT_EQ(operationHandler->evictUnusedCalled, 1u);

    memoryManager->freeGraphicsMemory(allocation);
    memoryManager->freeGraphicsMemory(allocationDefault);
}
using DrmMemoryOperationsHandlerBindTest = DrmMemoryOperationsHandlerBindFixture<1u>;

TEST_F(DrmMemoryOperationsHandlerBindTest, givenObjectAlwaysResidentAndNotUsedWhenRunningOutOfMemoryThenUnusedAllocationIsNotUnbound) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    for (auto &engine : device->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(GraphicsAllocation::objectNotUsed, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(0u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateResidencyTaskCount(GraphicsAllocation::objectAlwaysResident, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(1u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(GraphicsAllocation::objectNotUsed, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }

    EXPECT_EQ(mock->context.vmBindCalled, 2u);
    operationHandler->evictUnusedAllocations(false, true);

    EXPECT_EQ(mock->context.vmBindCalled, 2u);
    EXPECT_EQ(mock->context.vmUnbindCalled, 1u);

    memoryManager->freeGraphicsMemory(allocation);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenMakeEachAllocationResidentWhenCreateAllocationThenVmBindIsCalled) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.MakeEachAllocationResident.set(1);

    EXPECT_EQ(mock->context.vmBindCalled, 0u);
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    ASSERT_NE(nullptr, allocation);

    EXPECT_EQ(mock->context.vmBindCalled, 2u);

    auto &csr = device->getUltCommandStreamReceiver<FamilyType>();
    csr.makeResident(*allocation);

    EXPECT_EQ(csr.getResidencyAllocations().size(), 0u);

    memoryManager->freeGraphicsMemory(allocation);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenMakeEachAllocationResidentWhenMergeWithResidencyContainerThenVmBindIsCalled) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.MakeEachAllocationResident.set(2);

    EXPECT_EQ(mock->context.vmBindCalled, 0u);
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    EXPECT_EQ(mock->context.vmBindCalled, 0u);

    auto &csr = device->getUltCommandStreamReceiver<FamilyType>();
    ResidencyContainer residency;
    operationHandler->mergeWithResidencyContainer(&csr.getOsContext(), residency);

    EXPECT_EQ(mock->context.vmBindCalled, 2u);

    csr.makeResident(*allocation);
    EXPECT_EQ(csr.getResidencyAllocations().size(), 0u);

    memoryManager->freeGraphicsMemory(allocation);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, whenEvictUnusedResourcesWithWaitForCompletionThenWaitCsrMethodIsCalled) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    for (auto &engine : device->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(0u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(1u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    *device->getSubDevice(1u)->getDefaultEngine().commandStreamReceiver->getTagAddress() = 5;

    auto &csr = device->getUltCommandStreamReceiver<FamilyType>();
    csr.latestWaitForCompletionWithTimeoutTaskCount.store(123u);

    const auto status = operationHandler->evictUnusedAllocations(true, true);
    EXPECT_EQ(MemoryOperationsStatus::SUCCESS, status);

    auto latestWaitTaskCount = csr.latestWaitForCompletionWithTimeoutTaskCount.load();
    EXPECT_NE(latestWaitTaskCount, 123u);

    memoryManager->freeGraphicsMemory(allocation);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenGpuHangWhenEvictUnusedResourcesWithWaitForCompletionThenGpuHangIsReturned) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    auto &csr = device->getUltCommandStreamReceiver<FamilyType>();
    csr.callBaseWaitForCompletionWithTimeout = false;
    csr.returnWaitForCompletionWithTimeout = WaitStatus::GpuHang;

    const auto status = operationHandler->evictUnusedAllocations(true, true);
    EXPECT_EQ(MemoryOperationsStatus::GPU_HANG_DETECTED_DURING_OPERATION, status);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, whenRunningOutOfMemoryThenUnusedAllocationsAreUnbound) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    for (auto &engine : device->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(0u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(1u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 10;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    *device->getSubDevice(1u)->getDefaultEngine().commandStreamReceiver->getTagAddress() = 5;

    EXPECT_EQ(mock->context.vmBindCalled, 2u);

    operationHandler->evictUnusedAllocations(false, true);

    EXPECT_EQ(mock->context.vmBindCalled, 2u);
    EXPECT_EQ(mock->context.vmUnbindCalled, 1u);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenUsedAllocationInBothSubdevicesWhenEvictUnusedThenNothingIsUnbound) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    for (auto &engine : device->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 5;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(0u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 5;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }
    for (auto &engine : device->getSubDevice(1u)->getAllEngines()) {
        *engine.commandStreamReceiver->getTagAddress() = 5;
        allocation->updateTaskCount(8u, engine.osContext->getContextId());
        EXPECT_EQ(operationHandler->makeResidentWithinOsContext(engine.osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    }

    EXPECT_EQ(mock->context.vmBindCalled, 2u);

    operationHandler->evictUnusedAllocations(false, true);

    EXPECT_EQ(mock->context.vmBindCalled, 2u);
    EXPECT_EQ(mock->context.vmUnbindCalled, 0u);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenResidencyWithinOsContextFailsThenThenMergeWithResidencyContainertReturnsError) {
    struct MockDrmMemoryOperationsHandlerBindResidencyFail : public DrmMemoryOperationsHandlerBind {
        MockDrmMemoryOperationsHandlerBindResidencyFail(RootDeviceEnvironment &rootDeviceEnvironment, uint32_t rootDeviceIndex)
            : DrmMemoryOperationsHandlerBind(rootDeviceEnvironment, rootDeviceIndex) {}

        MemoryOperationsStatus makeResidentWithinOsContext(OsContext *osContext, ArrayRef<GraphicsAllocation *> gfxAllocations, bool evictable) override {
            return NEO::MemoryOperationsStatus::FAILED;
        }
    };

    ResidencyContainer residencyContainer;
    executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.reset(new MockDrmMemoryOperationsHandlerBindResidencyFail(*executionEnvironment->rootDeviceEnvironments[0], 0u));
    MockDrmMemoryOperationsHandlerBindResidencyFail *operationsHandlerResidency = static_cast<MockDrmMemoryOperationsHandlerBindResidencyFail *>(executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.get());

    auto &engines = device->getAllEngines();
    for (const auto &engine : engines) {
        EXPECT_NE(operationsHandlerResidency->mergeWithResidencyContainer(engine.osContext, residencyContainer), MemoryOperationsStatus::SUCCESS);
    }
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenEvictWithinOsContextFailsThenEvictReturnsError) {
    struct MockDrmMemoryOperationsHandlerBindEvictFail : public DrmMemoryOperationsHandlerBind {
        MockDrmMemoryOperationsHandlerBindEvictFail(RootDeviceEnvironment &rootDeviceEnvironment, uint32_t rootDeviceIndex)
            : DrmMemoryOperationsHandlerBind(rootDeviceEnvironment, rootDeviceIndex) {}

        MemoryOperationsStatus evictWithinOsContext(OsContext *osContext, GraphicsAllocation &gfxAllocation) override {
            return NEO::MemoryOperationsStatus::FAILED;
        }
    };

    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.reset(new MockDrmMemoryOperationsHandlerBindEvictFail(*executionEnvironment->rootDeviceEnvironments[0], 0u));
    MockDrmMemoryOperationsHandlerBindEvictFail *operationsHandlerEvict = static_cast<MockDrmMemoryOperationsHandlerBindEvictFail *>(executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.get());

    EXPECT_NE(operationsHandlerEvict->evict(device, *allocation), MemoryOperationsStatus::SUCCESS);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenEvictImplFailsThenEvictWithinOsContextReturnsError) {
    struct MockDrmMemoryOperationsHandlerBindEvictImplFail : public DrmMemoryOperationsHandlerBind {
        MockDrmMemoryOperationsHandlerBindEvictImplFail(RootDeviceEnvironment &rootDeviceEnvironment, uint32_t rootDeviceIndex)
            : DrmMemoryOperationsHandlerBind(rootDeviceEnvironment, rootDeviceIndex) {}

        int evictImpl(OsContext *osContext, GraphicsAllocation &gfxAllocation, DeviceBitfield deviceBitfield) override {
            return -1;
        }
    };

    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.reset(new MockDrmMemoryOperationsHandlerBindEvictImplFail(*executionEnvironment->rootDeviceEnvironments[0], 0u));
    MockDrmMemoryOperationsHandlerBindEvictImplFail *operationsHandlerEvict = static_cast<MockDrmMemoryOperationsHandlerBindEvictImplFail *>(executionEnvironment->rootDeviceEnvironments[0]->memoryOperationsInterface.get());
    auto &engines = device->getAllEngines();
    for (const auto &engine : engines) {
        EXPECT_NE(operationsHandlerEvict->evictWithinOsContext(engine.osContext, *allocation), MemoryOperationsStatus::SUCCESS);
    }

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenMakeBOsResidentFailsThenMakeResidentWithinOsContextReturnsError) {
    struct MockDrmAllocationBOsResident : public DrmAllocation {
        MockDrmAllocationBOsResident(uint32_t rootDeviceIndex, AllocationType allocationType, BufferObjects &bos, void *ptrIn, uint64_t gpuAddress, size_t sizeIn, MemoryPool pool)
            : DrmAllocation(rootDeviceIndex, allocationType, bos, ptrIn, gpuAddress, sizeIn, pool) {
        }

        int makeBOsResident(OsContext *osContext, uint32_t vmHandleId, std::vector<BufferObject *> *bufferObjects, bool bind) override {
            return -1;
        }
    };

    auto size = 1024u;
    BufferObjects bos;
    BufferObject mockBo(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    bos.push_back(&mockBo);

    auto allocation = new MockDrmAllocationBOsResident(0, AllocationType::UNKNOWN, bos, nullptr, 0u, size, MemoryPool::LocalMemory);
    auto graphicsAllocation = static_cast<GraphicsAllocation *>(allocation);

    EXPECT_EQ(operationHandler->makeResidentWithinOsContext(device->getDefaultEngine().osContext, ArrayRef<GraphicsAllocation *>(&graphicsAllocation, 1), false), MemoryOperationsStatus::OUT_OF_MEMORY);
    delete allocation;
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenDrmMemoryOperationBindWhenMakeResidentWithinOsContextEvictableAllocationThenAllocationIsNotMarkedAsAlwaysResident) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    EXPECT_EQ(operationHandler->makeResidentWithinOsContext(device->getDefaultEngine().osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), false), MemoryOperationsStatus::SUCCESS);
    EXPECT_TRUE(allocation->isAlwaysResident(device->getDefaultEngine().osContext->getContextId()));

    EXPECT_EQ(operationHandler->evict(device, *allocation), MemoryOperationsStatus::SUCCESS);

    EXPECT_EQ(operationHandler->makeResidentWithinOsContext(device->getDefaultEngine().osContext, ArrayRef<GraphicsAllocation *>(&allocation, 1), true), MemoryOperationsStatus::SUCCESS);
    EXPECT_FALSE(allocation->isAlwaysResident(device->getDefaultEngine().osContext->getContextId()));

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenDrmMemoryOperationBindWhenChangingResidencyThenOperationIsHandledProperly) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1)), MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->evict(device, *allocation), MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenDeviceWithMultipleSubdevicesWhenMakeResidentWithSubdeviceThenAllocationIsBindedOnlyInItsOsContexts) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(0u), *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(1u), *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);

    auto retVal = operationHandler->makeResident(device->getSubDevice(1u), ArrayRef<GraphicsAllocation *>(&allocation, 1));

    EXPECT_EQ(retVal, MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(0u), *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(1u), *allocation), MemoryOperationsStatus::SUCCESS);

    retVal = operationHandler->evict(device->getSubDevice(0u), *allocation);

    EXPECT_EQ(retVal, MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(0u), *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(1u), *allocation), MemoryOperationsStatus::SUCCESS);

    retVal = operationHandler->evict(device->getSubDevice(1u), *allocation);

    EXPECT_EQ(retVal, MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(0u), *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->isResident(device->getSubDevice(1u), *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, whenIoctlFailDuringEvictingThenUnrecoverableIsThrown) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::MEMORY_NOT_FOUND);
    EXPECT_EQ(operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1)), MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::SUCCESS);

    mock->context.vmUnbindReturn = -1;

    EXPECT_NE(operationHandler->evict(device, *allocation), MemoryOperationsStatus::SUCCESS);

    mock->context.vmUnbindReturn = 0;
    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, whenMakeResidentTwiceThenAllocIsBoundOnlyOnce) {
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    EXPECT_EQ(operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1)), MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1)), MemoryOperationsStatus::SUCCESS);
    EXPECT_EQ(operationHandler->isResident(device, *allocation), MemoryOperationsStatus::SUCCESS);

    EXPECT_EQ(mock->context.vmBindCalled, 2u);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, WhenVmBindAvaialableThenMemoryManagerReturnsSupportForIndirectAllocationsAsPack) {
    mock->bindAvailable = true;
    EXPECT_TRUE(memoryManager->allowIndirectAllocationsAsPack(0u));
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenNoVmBindSupportInDrmWhenCheckForSupportThenDefaultResidencyHandlerIsReturned) {
    mock->bindAvailable = false;
    auto handler = DrmMemoryOperationsHandler::create(*mock, 0u);

    mock->context.vmBindCalled = 0u;
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    handler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1));
    EXPECT_FALSE(mock->context.vmBindCalled);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenVmBindSupportAndNoMultiTileWhenCheckForSupportThenDefaultResidencyHandlerIsReturned) {
    DebugManager.flags.CreateMultipleSubDevices.set(1u);
    mock->bindAvailable = false;

    auto handler = DrmMemoryOperationsHandler::create(*mock, 0u);

    mock->context.vmBindCalled = 0u;
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    handler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1));
    EXPECT_FALSE(mock->context.vmBindCalled);

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenDisabledVmBindWhenCreateDrmHandlerThenVmBindIsNotUsed) {
    mock->context.vmBindReturn = 0;
    mock->bindAvailable = false;
    auto handler = DrmMemoryOperationsHandler::create(*mock, 0u);

    mock->context.vmBindCalled = false;
    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    handler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1));
    EXPECT_FALSE(mock->context.vmBindCalled);

    memoryManager->freeGraphicsMemory(allocation);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenVmBindSupportAndMultiSubdeviceWhenPinBOThenVmBindToAllVMsIsCalledInsteadOfExec) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    BufferObject boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject *boToPinPtr = &boToPin;

    pinBB.pin(&boToPinPtr, 1u, device->getDefaultEngine().osContext, 0u, 0u);

    EXPECT_EQ(mock->context.vmBindCalled, 2u);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenVmBindSupportAndMultiSubdeviceWhenValidateHostptrThenOnlyBindToSingleVMIsCalled) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    BufferObject boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject *boToPinPtr = &boToPin;

    pinBB.validateHostPtr(&boToPinPtr, 1u, device->getDefaultEngine().osContext, 0u, 0u);

    EXPECT_EQ(mock->context.vmBindCalled, 1u);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenVmBindSupportAndMultiSubdeviceWhenValidateHostptrThenBindToGivenVm) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    BufferObject boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject *boToPinPtr = &boToPin;
    uint32_t vmHandleId = 1u;

    pinBB.validateHostPtr(&boToPinPtr, 1u, device->getDefaultEngine().osContext, vmHandleId, 0u);

    EXPECT_EQ(mock->context.vmBindCalled, 1u);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
    EXPECT_EQ(mock->context.receivedVmBind->vmId, mock->getVirtualMemoryAddressSpace(vmHandleId));
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenVmBindSupportAndMultiSubdeviceWhenValidateMultipleBOsAndFirstBindFailsThenOnlyOneBindCalledAndErrorReturned) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;
    mock->context.vmBindReturn = -1;

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    BufferObject boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject boToPin2(device->getRootDeviceIndex(), mock, 3, 3, 0, 1);
    BufferObject *boToPinPtr[] = {&boToPin, &boToPin2};

    auto ret = pinBB.validateHostPtr(boToPinPtr, 2u, device->getDefaultEngine().osContext, 0u, 0u);

    EXPECT_EQ(ret, -1);

    EXPECT_EQ(mock->context.receivedVmBind->handle, 2u);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
}

struct DrmMemoryOperationsHandlerBindWithPerContextVms : public DrmMemoryOperationsHandlerBindFixture<2> {
    void SetUp() override {
        DrmMemoryOperationsHandlerBindFixture<2>::setUp(true);
    }

    void TearDown() override {
        DrmMemoryOperationsHandlerBindFixture<2>::TearDown();
    }
};

HWTEST_F(DrmMemoryOperationsHandlerBindWithPerContextVms, givenVmBindMultipleSubdevicesAndPErContextVmsWhenValidateHostptrThenCorrectContextsVmIdIsUsed) {
    mock->bindAvailable = true;
    mock->incrementVmId = true;

    std::unique_ptr<TestedDrmMemoryManager> memoryManager(new (std::nothrow) TestedDrmMemoryManager(true,
                                                                                                    false,
                                                                                                    true,
                                                                                                    *executionEnvironment));

    uint32_t vmIdForRootContext = 0;
    uint32_t vmIdForContext0 = 0;
    uint32_t vmIdForContext1 = 0;

    auto &engines = memoryManager->allRegisteredEngines[this->device->getRootDeviceIndex()];
    engines = EngineControlContainer{this->device->allEngines};
    engines.insert(engines.end(), this->device->getSubDevice(0)->getAllEngines().begin(), this->device->getSubDevice(0)->getAllEngines().end());
    engines.insert(engines.end(), this->device->getSubDevice(1)->getAllEngines().begin(), this->device->getSubDevice(1)->getAllEngines().end());
    for (auto &engine : engines) {
        engine.osContext->incRefInternal();
        if (engine.osContext->isDefaultContext()) {

            if (engine.osContext->getDeviceBitfield().to_ulong() == 3) {
                auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
                vmIdForRootContext = osContexLinux->getDrmVmIds()[0];
            } else if (engine.osContext->getDeviceBitfield().to_ulong() == 1) {
                auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
                vmIdForContext0 = osContexLinux->getDrmVmIds()[0];
            } else if (engine.osContext->getDeviceBitfield().to_ulong() == 2) {
                auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
                vmIdForContext1 = osContexLinux->getDrmVmIds()[1];
            }
        }
    }

    EXPECT_NE(0u, vmIdForRootContext);
    EXPECT_NE(0u, vmIdForContext0);
    EXPECT_NE(0u, vmIdForContext1);

    AllocationData allocationData;
    allocationData.size = 13u;
    allocationData.hostPtr = reinterpret_cast<const void *>(0x5001);
    allocationData.rootDeviceIndex = device->getRootDeviceIndex();
    allocationData.storageInfo.subDeviceBitfield = device->getDeviceBitfield();

    auto allocation = memoryManager->allocateGraphicsMemoryForNonSvmHostPtr(allocationData);
    EXPECT_NE(nullptr, allocation);

    memoryManager->freeGraphicsMemory(allocation);

    EXPECT_EQ(mock->context.vmBindCalled, 1u);
    EXPECT_EQ(vmIdForRootContext, mock->context.receivedVmBind->vmId);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
    auto vmBindCalledBefore = mock->context.vmBindCalled;

    allocationData.storageInfo.subDeviceBitfield = device->getSubDevice(0)->getDeviceBitfield();
    allocation = memoryManager->allocateGraphicsMemoryForNonSvmHostPtr(allocationData);

    EXPECT_NE(nullptr, allocation);

    memoryManager->freeGraphicsMemory(allocation);

    EXPECT_EQ(vmBindCalledBefore + 1, mock->context.vmBindCalled);
    EXPECT_EQ(vmIdForContext0, mock->context.receivedVmBind->vmId);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
    vmBindCalledBefore = mock->context.vmBindCalled;

    allocationData.storageInfo.subDeviceBitfield = device->getSubDevice(1)->getDeviceBitfield();
    allocation = memoryManager->allocateGraphicsMemoryForNonSvmHostPtr(allocationData);

    EXPECT_NE(nullptr, allocation);

    memoryManager->freeGraphicsMemory(allocation);

    EXPECT_EQ(vmBindCalledBefore + 1, mock->context.vmBindCalled);
    EXPECT_EQ(vmIdForContext1, mock->context.receivedVmBind->vmId);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
}

HWTEST_F(DrmMemoryOperationsHandlerBindWithPerContextVms, givenVmBindMultipleRootDevicesAndPerContextVmsWhenValidateHostptrThenCorrectContextsVmIdIsUsed) {
    mock->bindAvailable = true;
    mock->incrementVmId = true;

    auto device1 = devices[1].get();
    auto mock1 = executionEnvironment->rootDeviceEnvironments[1]->osInterface->getDriverModel()->as<DrmQueryMock>();
    mock1->bindAvailable = true;
    mock1->incrementVmId = true;

    std::unique_ptr<TestedDrmMemoryManager> memoryManager(new (std::nothrow) TestedDrmMemoryManager(true,
                                                                                                    false,
                                                                                                    true,
                                                                                                    *executionEnvironment));

    uint32_t vmIdForDevice0 = 0;
    uint32_t vmIdForDevice0Subdevice0 = 0;
    uint32_t vmIdForDevice1 = 0;
    uint32_t vmIdForDevice1Subdevice0 = 0;

    {

        auto &engines = memoryManager->allRegisteredEngines[this->device->getRootDeviceIndex()];
        engines = EngineControlContainer{this->device->allEngines};
        engines.insert(engines.end(), this->device->getSubDevice(0)->getAllEngines().begin(), this->device->getSubDevice(0)->getAllEngines().end());
        engines.insert(engines.end(), this->device->getSubDevice(1)->getAllEngines().begin(), this->device->getSubDevice(1)->getAllEngines().end());
        for (auto &engine : engines) {
            engine.osContext->incRefInternal();
            if (engine.osContext->isDefaultContext()) {
                if (engine.osContext->getDeviceBitfield().to_ulong() == 3) {
                    auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
                    vmIdForDevice0 = osContexLinux->getDrmVmIds()[0];
                } else if (engine.osContext->getDeviceBitfield().to_ulong() == 1) {
                    auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
                    vmIdForDevice0Subdevice0 = osContexLinux->getDrmVmIds()[0];
                }
            }
        }
    }

    {
        auto &engines = memoryManager->allRegisteredEngines[device1->getRootDeviceIndex()];
        engines = EngineControlContainer{this->device->allEngines};
        engines.insert(engines.end(), device1->getSubDevice(0)->getAllEngines().begin(), device1->getSubDevice(0)->getAllEngines().end());
        engines.insert(engines.end(), device1->getSubDevice(1)->getAllEngines().begin(), device1->getSubDevice(1)->getAllEngines().end());
        for (auto &engine : engines) {
            engine.osContext->incRefInternal();
            if (engine.osContext->isDefaultContext()) {

                if (engine.osContext->getDeviceBitfield().to_ulong() == 3) {
                    auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
                    vmIdForDevice1 = osContexLinux->getDrmVmIds()[0];

                } else if (engine.osContext->getDeviceBitfield().to_ulong() == 1) {
                    auto osContexLinux = static_cast<OsContextLinux *>(engine.osContext);
                    vmIdForDevice1Subdevice0 = osContexLinux->getDrmVmIds()[0];
                }
            }
        }
    }
    EXPECT_NE(0u, vmIdForDevice0);
    EXPECT_NE(0u, vmIdForDevice0Subdevice0);
    EXPECT_NE(0u, vmIdForDevice1);
    EXPECT_NE(0u, vmIdForDevice1Subdevice0);

    AllocationData allocationData;
    allocationData.size = 13u;
    allocationData.hostPtr = reinterpret_cast<const void *>(0x5001);
    allocationData.rootDeviceIndex = device1->getRootDeviceIndex();
    allocationData.storageInfo.subDeviceBitfield = device1->getDeviceBitfield();

    mock->context.vmBindCalled = 0;
    mock1->context.vmBindCalled = 0;

    auto allocation = memoryManager->allocateGraphicsMemoryForNonSvmHostPtr(allocationData);
    EXPECT_NE(nullptr, allocation);

    memoryManager->freeGraphicsMemory(allocation);

    EXPECT_EQ(mock->context.vmBindCalled, 0u);
    EXPECT_EQ(mock1->context.vmBindCalled, 1u);
    EXPECT_EQ(vmIdForDevice1, mock1->context.receivedVmBind->vmId);

    auto vmBindCalledBefore = mock1->context.vmBindCalled;

    allocationData.storageInfo.subDeviceBitfield = device->getSubDevice(0)->getDeviceBitfield();
    allocation = memoryManager->allocateGraphicsMemoryForNonSvmHostPtr(allocationData);

    EXPECT_NE(nullptr, allocation);

    memoryManager->freeGraphicsMemory(allocation);

    EXPECT_EQ(vmBindCalledBefore + 1, mock1->context.vmBindCalled);

    EXPECT_FALSE(mock->context.receivedVmBind.has_value());
    EXPECT_EQ(vmIdForDevice1Subdevice0, mock1->context.receivedVmBind->vmId);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenDirectSubmissionWhenPinBOThenVmBindIsCalledInsteadOfExec) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;
    device->getDefaultEngine().osContext->setDirectSubmissionActive();

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    BufferObject boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject *boToPinPtr = &boToPin;

    pinBB.pin(&boToPinPtr, 1u, device->getDefaultEngine().osContext, 0u, 0u);

    EXPECT_TRUE(mock->context.vmBindCalled);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenDirectSubmissionAndValidateHostptrWhenPinBOThenVmBindIsCalledInsteadOfExec) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;
    device->getDefaultEngine().osContext->setDirectSubmissionActive();

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    BufferObject boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject *boToPinPtr = &boToPin;

    pinBB.validateHostPtr(&boToPinPtr, 1u, device->getDefaultEngine().osContext, 0u, 0u);

    EXPECT_TRUE(mock->context.vmBindCalled);
    EXPECT_EQ(0, mock->ioctlCount.execbuffer2);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenVmBindSupportWhenPinBOThenAllocIsBound) {
    struct MockBO : public BufferObject {
        using BufferObject::bindInfo;
        using BufferObject::BufferObject;
    };
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    MockBO boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject *boToPinPtr = &boToPin;

    auto ret = pinBB.pin(&boToPinPtr, 1u, device->getDefaultEngine().osContext, 0u, 0u);

    EXPECT_TRUE(boToPin.bindInfo[0u][0u]);
    EXPECT_FALSE(ret);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenVmBindSupportWhenPinBOAndVmBindFailedThenAllocIsNotBound) {
    struct MockBO : public BufferObject {
        using BufferObject::bindInfo;
        using BufferObject::BufferObject;
    };
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;
    mock->context.vmBindReturn = -1;

    BufferObject pinBB(device->getRootDeviceIndex(), mock, 3, 1, 0, 1);
    MockBO boToPin(device->getRootDeviceIndex(), mock, 3, 2, 0, 1);
    BufferObject *boToPinPtr = &boToPin;

    auto ret = pinBB.pin(&boToPinPtr, 1u, device->getDefaultEngine().osContext, 0u, 0u);

    EXPECT_FALSE(boToPin.bindInfo[0u][0u]);
    EXPECT_TRUE(ret);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenCsrTagAllocatorsWhenDestructingCsrThenAllInternalAllocationsAreUnbound) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;
    auto csr = std::make_unique<UltCommandStreamReceiver<FamilyType>>(*executionEnvironment, 0, DeviceBitfield(1));
    auto osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor());
    csr->setupContext(*osContext);

    auto timestampStorageAlloc = csr->getTimestampPacketAllocator()->getTag()->getBaseGraphicsAllocation()->getDefaultGraphicsAllocation();
    auto hwTimeStampsAlloc = csr->getEventTsAllocator()->getTag()->getBaseGraphicsAllocation()->getDefaultGraphicsAllocation();
    auto hwPerfCounterAlloc = csr->getEventPerfCountAllocator(4)->getTag()->getBaseGraphicsAllocation()->getDefaultGraphicsAllocation();

    operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&timestampStorageAlloc, 1));
    operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&hwTimeStampsAlloc, 1));
    operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&hwPerfCounterAlloc, 1));

    csr.reset();

    EXPECT_EQ(mock->context.vmBindCalled, mock->context.vmUnbindCalled);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenPatIndexProgrammingEnabledWhenVmBindCalledThenSetPatIndexExtension) {
    DebugManager.flags.UseVmBind.set(1);
    mock->bindAvailable = true;

    auto csr = std::make_unique<UltCommandStreamReceiver<FamilyType>>(*executionEnvironment, 0, DeviceBitfield(1));
    auto osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor());
    csr->setupContext(*osContext);

    auto &gfxCoreHelper = executionEnvironment->rootDeviceEnvironments[0]->getHelper<GfxCoreHelper>();
    auto &productHelper = executionEnvironment->rootDeviceEnvironments[0]->getHelper<ProductHelper>();

    bool closSupported = (gfxCoreHelper.getNumCacheRegions() > 0);
    bool patIndexProgrammingSupported = productHelper.isVmBindPatIndexProgrammingSupported();

    uint64_t gpuAddress = 0x123000;
    size_t size = 1;
    BufferObject bo(0, mock, static_cast<uint64_t>(MockGmmClientContextBase::MockPatIndex::cached), 0, 1, 1);
    DrmAllocation allocation(0, 1, AllocationType::BUFFER, &bo, nullptr, gpuAddress, size, MemoryPool::System4KBPages);

    auto allocationPtr = static_cast<GraphicsAllocation *>(&allocation);

    for (int32_t debugFlag : {-1, 0, 1}) {
        if (debugFlag == 1 && !closSupported) {
            continue;
        }

        DebugManager.flags.ClosEnabled.set(debugFlag);

        mock->context.receivedVmBindPatIndex.reset();
        mock->context.receivedVmUnbindPatIndex.reset();

        bo.setPatIndex(mock->getPatIndex(allocation.getDefaultGmm(), allocation.getAllocationType(), CacheRegion::Default, CachePolicy::WriteBack, (debugFlag == 1 && closSupported)));

        operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocationPtr, 1));

        if (!patIndexProgrammingSupported) {
            EXPECT_FALSE(mock->context.receivedVmBindPatIndex);

            operationHandler->evict(device, allocation);
            EXPECT_FALSE(mock->context.receivedVmUnbindPatIndex);

            continue;
        }

        if (debugFlag == 0 || !closSupported || debugFlag == -1) {
            auto expectedIndex = static_cast<uint64_t>(MockGmmClientContextBase::MockPatIndex::cached);

            EXPECT_EQ(expectedIndex, mock->context.receivedVmBindPatIndex.value());

            operationHandler->evict(device, allocation);
            EXPECT_EQ(expectedIndex, mock->context.receivedVmUnbindPatIndex.value());
        } else {
            EXPECT_EQ(3u, mock->context.receivedVmBindPatIndex.value());

            operationHandler->evict(device, allocation);
            EXPECT_EQ(3u, mock->context.receivedVmUnbindPatIndex.value());
        }
    }
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenPatIndexErrorAndUncachedDebugFlagSetWhenGetPatIndexCalledThenAbort) {
    DebugManager.flags.UseVmBind.set(1);
    DebugManager.flags.ForceAllResourcesUncached.set(1);
    mock->bindAvailable = true;
    auto csr = std::make_unique<UltCommandStreamReceiver<FamilyType>>(*executionEnvironment, 0, DeviceBitfield(1));
    auto osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor());
    csr->setupContext(*osContext);
    auto &gfxCoreHelper = executionEnvironment->rootDeviceEnvironments[0]->getHelper<GfxCoreHelper>();
    auto &productHelper = executionEnvironment->rootDeviceEnvironments[0]->getHelper<ProductHelper>();
    bool closSupported = (gfxCoreHelper.getNumCacheRegions() > 0);
    bool patIndexProgrammingSupported = productHelper.isVmBindPatIndexProgrammingSupported();
    if (!closSupported || !patIndexProgrammingSupported) {
        GTEST_SKIP();
    }

    static_cast<MockGmmClientContextBase *>(executionEnvironment->rootDeviceEnvironments[0]->getGmmClientContext())->returnErrorOnPatIndexQuery = true;

    uint64_t gpuAddress = 0x123000;
    size_t size = 1;
    BufferObject bo(0, mock, static_cast<uint64_t>(MockGmmClientContextBase::MockPatIndex::cached), 0, 1, 1);
    DrmAllocation allocation(0, 1, AllocationType::BUFFER, &bo, nullptr, gpuAddress, size, MemoryPool::System4KBPages);

    EXPECT_ANY_THROW(mock->getPatIndex(allocation.getDefaultGmm(), allocation.getAllocationType(), CacheRegion::Default, CachePolicy::WriteBack, false));
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenUncachedDebugFlagSetWhenVmBindCalledThenSetCorrectPatIndexExtension) {
    DebugManager.flags.UseVmBind.set(1);
    DebugManager.flags.ForceAllResourcesUncached.set(1);
    mock->bindAvailable = true;

    auto csr = std::make_unique<UltCommandStreamReceiver<FamilyType>>(*executionEnvironment, 0, DeviceBitfield(1));
    auto osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor());
    csr->setupContext(*osContext);

    auto &productHelper = executionEnvironment->rootDeviceEnvironments[0]->getHelper<ProductHelper>();

    if (!productHelper.isVmBindPatIndexProgrammingSupported()) {
        GTEST_SKIP();
    }

    mock->context.receivedVmBindPatIndex.reset();
    mock->context.receivedVmUnbindPatIndex.reset();

    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1));

    auto expectedIndex = static_cast<uint64_t>(MockGmmClientContextBase::MockPatIndex::uncached);

    EXPECT_EQ(expectedIndex, mock->context.receivedVmBindPatIndex.value());

    operationHandler->evict(device, *allocation);
    EXPECT_EQ(expectedIndex, mock->context.receivedVmUnbindPatIndex.value());
    memoryManager->freeGraphicsMemory(allocation);
}

HWTEST_F(DrmMemoryOperationsHandlerBindTest, givenDebugFlagSetWhenVmBindCalledThenOverridePatIndex) {
    DebugManager.flags.UseVmBind.set(1);
    DebugManager.flags.ClosEnabled.set(1);
    DebugManager.flags.OverridePatIndex.set(1);

    mock->bindAvailable = true;

    auto csr = std::make_unique<UltCommandStreamReceiver<FamilyType>>(*executionEnvironment, 0, DeviceBitfield(1));
    auto osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor());
    csr->setupContext(*osContext);

    auto timestampStorageAlloc = csr->getTimestampPacketAllocator()->getTag()->getBaseGraphicsAllocation()->getDefaultGraphicsAllocation();

    auto &gfxCoreHelper = executionEnvironment->rootDeviceEnvironments[0]->getHelper<GfxCoreHelper>();

    if (gfxCoreHelper.getNumCacheRegions() == 0) {
        GTEST_SKIP();
    }

    operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&timestampStorageAlloc, 1));

    EXPECT_EQ(1u, mock->context.receivedVmBindPatIndex.value());

    operationHandler->evict(device, *timestampStorageAlloc);

    EXPECT_EQ(1u, mock->context.receivedVmUnbindPatIndex.value());
}

TEST_F(DrmMemoryOperationsHandlerBindTest, givenClosEnabledAndAllocationToBeCachedInCacheRegionWhenVmBindIsCalledThenSetPatIndexCorrespondingToRequestedRegion) {
    DebugManager.flags.UseVmBind.set(1);
    DebugManager.flags.ClosEnabled.set(1);
    mock->bindAvailable = true;

    auto csr = std::make_unique<MockCommandStreamReceiver>(*executionEnvironment, 0, 1);
    auto osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor());
    csr->setupContext(*osContext);

    mock->cacheInfo.reset(new CacheInfo(*mock, 64 * MemoryConstants::kiloByte, 2, 32));

    auto &gfxCoreHelper = executionEnvironment->rootDeviceEnvironments[0]->getHelper<GfxCoreHelper>();

    if (gfxCoreHelper.getNumCacheRegions() == 0) {
        GTEST_SKIP();
    }

    auto allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});

    for (auto cacheRegion : {CacheRegion::Default, CacheRegion::Region1, CacheRegion::Region2}) {
        EXPECT_TRUE(static_cast<DrmAllocation *>(allocation)->setCacheAdvice(mock, 32 * MemoryConstants::kiloByte, cacheRegion));

        mock->context.receivedVmBindPatIndex.reset();
        operationHandler->makeResident(device, ArrayRef<GraphicsAllocation *>(&allocation, 1));

        auto patIndex = gfxCoreHelper.getPatIndex(cacheRegion, CachePolicy::WriteBack);

        EXPECT_EQ(patIndex, mock->context.receivedVmBindPatIndex.value());

        mock->context.receivedVmUnbindPatIndex.reset();
        operationHandler->evict(device, *allocation);

        EXPECT_EQ(patIndex, mock->context.receivedVmUnbindPatIndex.value());
    }

    memoryManager->freeGraphicsMemory(allocation);
}

TEST(DrmResidencyHandlerTests, givenClosIndexAndMemoryTypeWhenAskingForPatIndexThenReturnCorrectValue) {
    MockExecutionEnvironment mockExecutionEnvironment{};
    auto &gfxCoreHelper = mockExecutionEnvironment.rootDeviceEnvironments[0]->getHelper<GfxCoreHelper>();

    if (gfxCoreHelper.getNumCacheRegions() == 0) {
        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::Uncached));
        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteBack));
    } else {
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::Uncached));
        EXPECT_EQ(1u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteCombined));
        EXPECT_EQ(2u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteThrough));
        EXPECT_EQ(3u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteBack));

        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::Uncached));
        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::WriteCombined));
        EXPECT_EQ(4u, gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::WriteThrough));
        EXPECT_EQ(5u, gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::WriteBack));

        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::Uncached));
        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::WriteCombined));
        EXPECT_EQ(6u, gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::WriteThrough));
        EXPECT_EQ(7u, gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::WriteBack));
    }
}

TEST(DrmResidencyHandlerTests, givenForceAllResourcesUnchashedSetAskingForPatIndexThenReturnCorrectValue) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.ForceAllResourcesUncached.set(1);

    MockExecutionEnvironment mockExecutionEnvironment{};
    auto &gfxCoreHelper = mockExecutionEnvironment.rootDeviceEnvironments[0]->getHelper<GfxCoreHelper>();

    if (gfxCoreHelper.getNumCacheRegions() == 0) {
        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::Uncached));
        EXPECT_ANY_THROW(gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteBack));
    } else {
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::Uncached));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteCombined));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteThrough));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Default, CachePolicy::WriteBack));

        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::Uncached));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::WriteCombined));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::WriteThrough));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region1, CachePolicy::WriteBack));

        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::Uncached));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::WriteCombined));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::WriteThrough));
        EXPECT_EQ(0u, gfxCoreHelper.getPatIndex(CacheRegion::Region2, CachePolicy::WriteBack));
    }
}

TEST(DrmResidencyHandlerTests, givenSupportedVmBindAndDebugFlagUseVmBindWhenQueryingIsVmBindAvailableThenBindAvailableIsInitializedOnce) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseVmBind.set(1);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.vmBindQueryValue = 1;
    EXPECT_FALSE(drm.bindAvailable);

    EXPECT_EQ(0u, drm.context.vmBindQueryCalled);
    EXPECT_TRUE(drm.isVmBindAvailable());
    EXPECT_TRUE(drm.bindAvailable);
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);

    EXPECT_TRUE(drm.isVmBindAvailable());
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);
}

TEST(DrmResidencyHandlerTests, givenDebugFlagUseVmBindWhenQueryingIsVmBindAvailableThenSupportIsOverriden) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseVmBind.set(1);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    EXPECT_FALSE(drm.bindAvailable);
    drm.context.vmBindQueryReturn = -1;

    EXPECT_EQ(0u, drm.context.vmBindQueryCalled);
    EXPECT_TRUE(drm.isVmBindAvailable());
    EXPECT_TRUE(drm.bindAvailable);
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);

    EXPECT_TRUE(drm.isVmBindAvailable());
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);
}

namespace NEO {
extern bool disableBindDefaultInTests;
}

TEST(DrmResidencyHandlerTests, givenDebugFlagUseVmBindSetDefaultAndBindAvailableInDrmWhenQueryingIsVmBindAvailableThenBindIsAvailableWhenSupported) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseVmBind.set(-1);
    VariableBackup<bool> disableBindBackup(&disableBindDefaultInTests, false);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.vmBindQueryValue = 1;
    drm.context.vmBindQueryReturn = 0;
    EXPECT_FALSE(drm.bindAvailable);
    auto &productHelper = drm.getRootDeviceEnvironment().getHelper<ProductHelper>();

    EXPECT_EQ(0u, drm.context.vmBindQueryCalled);
    EXPECT_EQ(drm.isVmBindAvailable(), productHelper.isNewResidencyModelSupported());
    EXPECT_EQ(drm.bindAvailable, productHelper.isNewResidencyModelSupported());
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);
}

TEST(DrmResidencyHandlerTests, givenDebugFlagUseVmBindSetDefaultWhenQueryingIsVmBindAvailableFailedThenBindIsNot) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseVmBind.set(-1);
    VariableBackup<bool> disableBindBackup(&disableBindDefaultInTests, false);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.vmBindQueryValue = 1;
    drm.context.vmBindQueryReturn = -1;
    EXPECT_FALSE(drm.bindAvailable);

    EXPECT_EQ(0u, drm.context.vmBindQueryCalled);
    EXPECT_FALSE(drm.isVmBindAvailable());
    EXPECT_FALSE(drm.bindAvailable);
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);
}

TEST(DrmResidencyHandlerTests, givenDebugFlagUseVmBindSetDefaultWhenQueryingIsVmBindAvailableSuccedAndReportNoBindAvailableInDrmThenBindIsNotAvailable) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseVmBind.set(-1);
    VariableBackup<bool> disableBindBackup(&disableBindDefaultInTests, false);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.vmBindQueryValue = 0;
    drm.context.vmBindQueryReturn = 0;
    EXPECT_FALSE(drm.bindAvailable);

    EXPECT_EQ(0u, drm.context.vmBindQueryCalled);
    EXPECT_FALSE(drm.isVmBindAvailable());
    EXPECT_FALSE(drm.bindAvailable);
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);
}

TEST(DrmSetPairTests, whenQueryingForSetPairAvailableAndNoDebugKeyThenFalseIsReturned) {
    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.setPairQueryValue = 0;
    drm.context.setPairQueryReturn = 0;
    EXPECT_FALSE(drm.setPairAvailable);

    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
    drm.callBaseIsSetPairAvailable = true;
    EXPECT_FALSE(drm.isSetPairAvailable());
    EXPECT_FALSE(drm.setPairAvailable);
    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
}

TEST(DrmSetPairTests, whenQueryingForSetPairAvailableAndDebugKeySetAndNoSupportAvailableThenFalseIsReturned) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnableSetPair.set(1);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.setPairQueryValue = 0;
    drm.context.setPairQueryReturn = 0;
    EXPECT_FALSE(drm.setPairAvailable);

    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
    drm.callBaseIsSetPairAvailable = true;
    EXPECT_FALSE(drm.isSetPairAvailable());
    EXPECT_FALSE(drm.setPairAvailable);
    EXPECT_EQ(1u, drm.context.setPairQueryCalled);
}

TEST(DrmSetPairTests, whenQueryingForSetPairAvailableAndDebugKeyNotSetThenNoSupportIsReturned) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnableSetPair.set(0);
    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.setPairQueryValue = 0;
    drm.context.setPairQueryReturn = 0;
    EXPECT_FALSE(drm.setPairAvailable);

    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
    drm.callBaseIsSetPairAvailable = true;
    EXPECT_FALSE(drm.isSetPairAvailable());
    EXPECT_FALSE(drm.setPairAvailable);
    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
}

TEST(DrmResidencyHandlerTests, whenQueryingForSetPairAvailableAndVmBindAvailableThenBothExpectedValueIsReturned) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseVmBind.set(-1);
    DebugManager.flags.EnableSetPair.set(1);
    VariableBackup<bool> disableBindBackup(&disableBindDefaultInTests, false);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    auto &productHelper = drm.getRootDeviceEnvironment().getHelper<ProductHelper>();

    drm.context.setPairQueryValue = 1;
    drm.context.setPairQueryReturn = 0;
    EXPECT_FALSE(drm.setPairAvailable);
    drm.callBaseIsSetPairAvailable = true;

    drm.context.vmBindQueryValue = 1;
    drm.context.vmBindQueryReturn = 0;
    EXPECT_FALSE(drm.bindAvailable);
    drm.callBaseIsVmBindAvailable = true;

    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
    EXPECT_TRUE(drm.isSetPairAvailable());
    EXPECT_TRUE(drm.setPairAvailable);
    EXPECT_EQ(1u, drm.context.setPairQueryCalled);

    EXPECT_EQ(0u, drm.context.vmBindQueryCalled);
    EXPECT_EQ(drm.isVmBindAvailable(), productHelper.isNewResidencyModelSupported());
    EXPECT_EQ(drm.bindAvailable, productHelper.isNewResidencyModelSupported());
    EXPECT_EQ(1u, drm.context.vmBindQueryCalled);
}

TEST(DrmResidencyHandlerTests, whenQueryingForSetPairAvailableAndSupportAvailableThenExpectedValueIsReturned) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnableSetPair.set(1);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.setPairQueryValue = 1;
    drm.context.setPairQueryReturn = 0;
    EXPECT_FALSE(drm.setPairAvailable);

    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
    drm.callBaseIsSetPairAvailable = true;
    EXPECT_TRUE(drm.isSetPairAvailable());
    EXPECT_TRUE(drm.setPairAvailable);
    EXPECT_EQ(1u, drm.context.setPairQueryCalled);
}

TEST(DrmResidencyHandlerTests, whenQueryingForSetPairAvailableAndFailureInQueryThenFalseIsReturned) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnableSetPair.set(1);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.setPairQueryValue = 1;
    drm.context.setPairQueryReturn = 1;
    EXPECT_FALSE(drm.setPairAvailable);

    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
    drm.callBaseIsSetPairAvailable = true;
    EXPECT_FALSE(drm.isSetPairAvailable());
    EXPECT_FALSE(drm.setPairAvailable);
    EXPECT_EQ(1u, drm.context.setPairQueryCalled);
}

TEST(DrmResidencyHandlerTests, whenQueryingForSetPairAvailableWithDebugKeySetToZeroThenFalseIsReturned) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnableSetPair.set(0);

    auto executionEnvironment = std::make_unique<MockExecutionEnvironment>();
    DrmQueryMock drm{*executionEnvironment->rootDeviceEnvironments[0]};
    drm.context.setPairQueryValue = 1;
    drm.context.setPairQueryReturn = 1;
    EXPECT_FALSE(drm.setPairAvailable);

    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
    drm.callBaseIsSetPairAvailable = true;
    EXPECT_FALSE(drm.isSetPairAvailable());
    EXPECT_FALSE(drm.setPairAvailable);
    EXPECT_EQ(0u, drm.context.setPairQueryCalled);
}
