/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "shared/source/os_interface/os_interface.h"
#include "shared/source/os_interface/windows/os_environment_win.h"
#include "shared/source/os_interface/windows/wddm_memory_operations_handler.h"
#include "shared/test/common/helpers/engine_descriptor_helper.h"
#include "shared/test/common/helpers/execution_environment_helper.h"
#include "shared/test/common/mocks/mock_gmm.h"
#include "shared/test/common/mocks/mock_gmm_page_table_mngr.h"
#include "shared/test/common/mocks/mock_wddm_residency_allocations_container.h"
#include "shared/test/common/mocks/windows/mock_gdi_interface.h"
#include "shared/test/common/os_interface/windows/mock_wddm_memory_manager.h"
#include "shared/test/common/os_interface/windows/wddm_fixture.h"
#include "shared/test/common/test_macros/hw_test.h"

#include "opencl/test/unit_test/mocks/mock_context.h"

#include "gtest/gtest.h"

#include <type_traits>

using namespace NEO;
using namespace ::testing;

class WddmMemoryManagerFixture : public GdiDllFixture {
  public:
    void setUp();

    void tearDown() {
        GdiDllFixture::tearDown();
    }

    ExecutionEnvironment *executionEnvironment;
    RootDeviceEnvironment *rootDeviceEnvironment = nullptr;
    std::unique_ptr<MockWddmMemoryManager> memoryManager;
    WddmMock *wddm = nullptr;
    const uint32_t rootDeviceIndex = 0u;
};

typedef ::Test<WddmMemoryManagerFixture> WddmMemoryManagerTest;

class MockWddmMemoryManagerFixture {
  public:
    void SetUp() {
        auto osEnvironment = new OsEnvironmentWin();
        gdi = new MockGdi();
        osEnvironment->gdi.reset(gdi);
        executionEnvironment.osEnvironment.reset(osEnvironment);

        executionEnvironment.prepareRootDeviceEnvironments(2u);
        for (auto i = 0u; i < executionEnvironment.rootDeviceEnvironments.size(); i++) {
            executionEnvironment.rootDeviceEnvironments[i]->setHwInfoAndInitHelpers(defaultHwInfo.get());
            executionEnvironment.rootDeviceEnvironments[i]->initGmm();
            auto wddm = static_cast<WddmMock *>(Wddm::createWddm(nullptr, *executionEnvironment.rootDeviceEnvironments[i]));
            constexpr uint64_t heap32Base = (is32bit) ? 0x1000 : 0x800000000000;
            wddm->setHeap32(heap32Base, 1000 * MemoryConstants::pageSize - 1);
            wddm->init();
        }
        rootDeviceEnvironment = executionEnvironment.rootDeviceEnvironments[0].get();
        wddm = static_cast<WddmMock *>(rootDeviceEnvironment->osInterface->getDriverModel()->as<Wddm>());
        rootDeviceEnvironment->memoryOperationsInterface = std::make_unique<WddmMemoryOperationsHandler>(wddm);
        executionEnvironment.initializeMemoryManager();

        memoryManager = std::make_unique<MockWddmMemoryManager>(executionEnvironment);
        csr.reset(createCommandStream(executionEnvironment, 0u, 1));
        auto hwInfo = rootDeviceEnvironment->getHardwareInfo();
        auto &gfxCoreHelper = rootDeviceEnvironment->getHelper<GfxCoreHelper>();
        osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor(gfxCoreHelper.getGpgpuEngineInstances(*rootDeviceEnvironment)[0],
                                                                                                                      PreemptionHelper::getDefaultPreemptionMode(*hwInfo)));
        osContext->ensureContextInitialized();

        osContext->incRefInternal();
        mockTemporaryResources = reinterpret_cast<MockWddmResidentAllocationsContainer *>(wddm->getTemporaryResourcesContainer());
    }

    void TearDown() {
        osContext->decRefInternal();
    }

    RootDeviceEnvironment *rootDeviceEnvironment = nullptr;
    MockExecutionEnvironment executionEnvironment;
    std::unique_ptr<MockWddmMemoryManager> memoryManager;
    std::unique_ptr<CommandStreamReceiver> csr;

    WddmMock *wddm = nullptr;
    MockWddmResidentAllocationsContainer *mockTemporaryResources;
    OsContext *osContext = nullptr;
    MockGdi *gdi = nullptr;
};

typedef ::Test<MockWddmMemoryManagerFixture> WddmMemoryManagerResidencyTest;

class ExecutionEnvironmentFixture : public ::testing::Test {
  public:
    MockExecutionEnvironment executionEnvironment;
};

class WddmMemoryManagerFixtureWithGmockWddm : public ExecutionEnvironmentFixture {
  public:
    MockWddmMemoryManager *memoryManager = nullptr;

    void SetUp() override {
        // wddm is deleted by memory manager

        wddm = new WddmMock(*executionEnvironment.rootDeviceEnvironments[0]);
        ASSERT_NE(nullptr, wddm);
        auto preemptionMode = PreemptionHelper::getDefaultPreemptionMode(*defaultHwInfo);
        wddm->init();
        executionEnvironment.rootDeviceEnvironments[0]->memoryOperationsInterface = std::make_unique<WddmMemoryOperationsHandler>(wddm);
        osInterface = executionEnvironment.rootDeviceEnvironments[0]->osInterface.get();
        memoryManager = new (std::nothrow) MockWddmMemoryManager(executionEnvironment);
        executionEnvironment.memoryManager.reset(memoryManager);
        // assert we have memory manager
        ASSERT_NE(nullptr, memoryManager);
        csr.reset(createCommandStream(executionEnvironment, 0u, 1));
        auto &gfxCoreHelper = executionEnvironment.rootDeviceEnvironments[0]->getHelper<GfxCoreHelper>();
        osContext = memoryManager->createAndRegisterOsContext(csr.get(), EngineDescriptorHelper::getDefaultDescriptor(gfxCoreHelper.getGpgpuEngineInstances(*executionEnvironment.rootDeviceEnvironments[0])[0],
                                                                                                                      preemptionMode));
        osContext->incRefInternal();
    }

    void TearDown() override {
        osContext->decRefInternal();
    }

    WddmMock *wddm = nullptr;
    std::unique_ptr<CommandStreamReceiver> csr;
    OSInterface *osInterface;
    OsContext *osContext;
};

using WddmMemoryManagerTest2 = WddmMemoryManagerFixtureWithGmockWddm;

class BufferWithWddmMemory : public ::testing::Test,
                             public WddmMemoryManagerFixture {
  public:
  protected:
    void SetUp() override {
        WddmMemoryManagerFixture::setUp();
        tmp = context.getMemoryManager();
        context.memoryManager = memoryManager.get();
        flags = 0;
    }

    void TearDown() override {
        context.memoryManager = tmp;
        WddmMemoryManagerFixture::tearDown();
    }

    MemoryManager *tmp;
    MockContext context;
    cl_mem_flags flags;
    cl_int retVal;
};

class WddmMemoryManagerSimpleTest : public MockWddmMemoryManagerFixture, public ::testing::Test {
  public:
    void SetUp() override {
        MockWddmMemoryManagerFixture::SetUp();
    }
    void TearDown() override {
        MockWddmMemoryManagerFixture::TearDown();
    }
};

class MockWddmMemoryManagerTest : public ::testing::Test {
  public:
    void SetUp() override {
        executionEnvironment = getExecutionEnvironmentImpl(hwInfo, 2);
        executionEnvironment->incRefInternal();
        wddm = new WddmMock(*executionEnvironment->rootDeviceEnvironments[1].get());
        executionEnvironment->rootDeviceEnvironments[rootDeviceIndex]->osInterface->setDriverModel(std::unique_ptr<DriverModel>(wddm));
        executionEnvironment->rootDeviceEnvironments[rootDeviceIndex]->memoryOperationsInterface = std::make_unique<WddmMemoryOperationsHandler>(wddm);
    }

    void TearDown() override {
        executionEnvironment->decRefInternal();
    }

    HardwareInfo *hwInfo = nullptr;
    WddmMock *wddm = nullptr;
    ExecutionEnvironment *executionEnvironment = nullptr;
    const uint32_t rootDeviceIndex = 0u;
};

using OsAgnosticMemoryManagerUsingWddmTest = MockWddmMemoryManagerTest;
