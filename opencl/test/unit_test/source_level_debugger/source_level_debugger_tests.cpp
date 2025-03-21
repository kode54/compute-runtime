/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_container/cmdcontainer.h"
#include "shared/source/device/device.h"
#include "shared/source/helpers/file_io.h"
#include "shared/source/indirect_heap/heap_size.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/source/program/kernel_info.h"
#include "shared/source/source_level_debugger/source_level_debugger.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/helpers/debugger_library_restore.h"
#include "shared/test/common/helpers/execution_environment_helper.h"
#include "shared/test/common/helpers/ult_hw_config.h"
#include "shared/test/common/helpers/variable_backup.h"
#include "shared/test/common/mocks/mock_gmm_helper.h"
#include "shared/test/common/mocks/mock_source_level_debugger.h"
#include "shared/test/common/test_macros/hw_test.h"

#include "opencl/source/platform/platform.h"
#include "opencl/test/unit_test/mocks/mock_cl_device.h"
#include "opencl/test/unit_test/mocks/mock_platform.h"

#include <memory>
#include <string>

using namespace NEO;
using std::string;
using std::unique_ptr;

class SourceLevelDebuggerSupportedFixture : public ::testing::Test {
  public:
    void SetUp() override {
        hwInfo.capabilityTable.debuggerSupported = true;
    }

    NEO::HardwareInfo hwInfo = *NEO::defaultHwInfo;
};

TEST(SourceLevelDebugger, whenSourceLevelDebuggerIsCreatedThenLegacyModeIsTrue) {
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(true);

    MockSourceLevelDebugger debugger;
    EXPECT_TRUE(debugger.isLegacy());
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenPlatformWhenItIsCreatedThenSourceLevelDebuggerIsCreatedInExecutionEnvironment, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    auto executionEnvironment = MockDevice::prepareExecutionEnvironment(&hwInfo, 0u);
    MockPlatform platform(*executionEnvironment);
    platform.initializeWithNewDevices();

    EXPECT_NE(nullptr, executionEnvironment->rootDeviceEnvironments[0]->debugger);
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenPlatformWhenSourceLevelDebuggerIsCreatedThenRuntimeCapabilityHasFusedEusDisabled, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    auto executionEnvironment = MockDevice::prepareExecutionEnvironment(&hwInfo, 0u);
    MockPlatform platform(*executionEnvironment);
    platform.initializeWithNewDevices();

    ASSERT_NE(nullptr, executionEnvironment->rootDeviceEnvironments[0]->debugger);
    EXPECT_FALSE(executionEnvironment->rootDeviceEnvironments[0]->getHardwareInfo()->capabilityTable.fusedEuEnabled);
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenPlatformWhenInitializingSourceLevelDebuggerFailsThenRuntimeCapabilityFusedEusAreNotModified, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    interceptor.initRetVal = -1;
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);
    auto executionEnvironment = MockDevice::prepareExecutionEnvironment(&hwInfo, 0u);
    MockPlatform platform(*executionEnvironment);
    platform.initializeWithNewDevices();

    bool defaultValue = hwInfo.capabilityTable.fusedEuEnabled;

    ASSERT_NE(nullptr, executionEnvironment->rootDeviceEnvironments[0]->debugger);
    EXPECT_EQ(defaultValue, executionEnvironment->rootDeviceEnvironments[0]->getHardwareInfo()->capabilityTable.fusedEuEnabled);
}

TEST(SourceLevelDebugger, givenNoKernelDebuggerLibraryWhenSourceLevelDebuggerIsCreatedThenLibraryIsNotLoaded) {
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(false);

    MockSourceLevelDebugger debugger;
    EXPECT_EQ(nullptr, debugger.debuggerLibrary.get());
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryAvailableWhenSourceLevelDebuggerIsConstructedThenLibraryIsLoaded) {
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(true);

    MockSourceLevelDebugger debugger;
    EXPECT_NE(nullptr, debugger.debuggerLibrary.get());
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryAvailableWhenIsDebuggerActiveIsCalledThenFalseIsReturned) {
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(true);

    MockSourceLevelDebugger debugger;
    bool active = debugger.isDebuggerActive();
    EXPECT_FALSE(active);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenIsDebuggerActiveIsCalledThenTrueIsReturned) {
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);

    MockSourceLevelDebugger debugger;
    bool active = debugger.isDebuggerActive();
    EXPECT_TRUE(active);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotAvailableWhenIsDebuggerActiveIsCalledThenFalseIsReturned) {
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(false);

    MockSourceLevelDebugger debugger;
    bool active = debugger.isDebuggerActive();
    EXPECT_FALSE(active);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenNotifySourceCodeIsCalledThenDebuggerLibraryFunctionIsCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    GfxDbgSourceCode argOut;
    char fileName[] = "filename";
    argOut.sourceName = fileName;
    argOut.sourceNameMaxLen = sizeof(fileName);
    interceptor.sourceCodeArgOut = &argOut;

    const char source[] = "sourceCode";
    string file;
    debugger.callBaseNotifySourceCode = true;
    debugger.notifySourceCode(source, sizeof(source), file);

    EXPECT_TRUE(interceptor.sourceCodeCalled);
    EXPECT_EQ(reinterpret_cast<GfxDeviceHandle>(static_cast<uint64_t>(MockSourceLevelDebugger::mockDeviceHandle)), interceptor.sourceCodeArgIn.hDevice);
    EXPECT_EQ(source, interceptor.sourceCodeArgIn.sourceCode);
    EXPECT_EQ(sizeof(source), interceptor.sourceCodeArgIn.sourceCodeSize);
    EXPECT_NE(nullptr, interceptor.sourceCodeArgIn.sourceName);
    EXPECT_NE(0u, interceptor.sourceCodeArgIn.sourceNameMaxLen);

    EXPECT_STREQ(fileName, file.c_str());
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotActiveWhenNotifySourceCodeIsCalledThenDebuggerLibraryFunctionIsNotCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    debugger.setActive(false);

    const char source[] = "sourceCode";
    string file;
    debugger.callBaseNotifySourceCode = true;
    debugger.notifySourceCode(source, sizeof(source), file);
    EXPECT_FALSE(interceptor.sourceCodeCalled);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenNotifyNewDeviceIsCalledThenDebuggerLibraryFunctionIsCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    debugger.callBaseNotifyNewDevice = true;
    debugger.notifyNewDevice(4);

    EXPECT_TRUE(interceptor.newDeviceCalled);
    EXPECT_EQ(reinterpret_cast<GfxDeviceHandle>(static_cast<uint64_t>(4u)), interceptor.newDeviceArgIn.dh);
    EXPECT_EQ(4u, debugger.deviceHandle);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotActiveWhenNotifyNewDeviceIsCalledThenDebuggerLibraryFunctionIsNotCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    debugger.callBaseNotifyNewDevice = true;

    debugger.setActive(false);
    debugger.notifyNewDevice(4);
    EXPECT_FALSE(interceptor.newDeviceCalled);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenIsOptimizationDisabledIsCalledThenDebuggerLibraryFunctionIsCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    debugger.callBaseIsOptimizationDisabled = true;
    bool isOptDisabled = debugger.isOptimizationDisabled();
    EXPECT_FALSE(isOptDisabled);

    EXPECT_TRUE(interceptor.optionCalled);
    EXPECT_EQ(GfxDbgOptionNames::DBG_OPTION_IS_OPTIMIZATION_DISABLED, interceptor.optionArgIn.optionName);
    EXPECT_NE(nullptr, interceptor.optionArgIn.value);
    EXPECT_LT(0u, interceptor.optionArgIn.valueLen);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotActiveWhenIsOptimizationDisabledIsCalledThenDebuggerLibraryFunctionIsNotCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    debugger.setActive(false);
    debugger.callBaseIsOptimizationDisabled = true;
    bool isOptDisabled = debugger.isOptimizationDisabled();
    EXPECT_FALSE(isOptDisabled);
    EXPECT_FALSE(interceptor.optionCalled);
}

TEST(SourceLevelDebugger, givenActiveDebuggerWhenGetDebuggerOptionReturnsZeroThenIsOptimizationDisabledReturnsFalse) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    char value = '1';
    GfxDbgOption optionArgOut;
    interceptor.optionArgOut = &optionArgOut;
    interceptor.optionArgOut->value = &value;
    interceptor.optionArgOut->valueLen = sizeof(value);
    interceptor.optionRetVal = 0;

    MockSourceLevelDebugger debugger;
    debugger.callBaseIsOptimizationDisabled = true;
    bool isOptDisabled = debugger.isOptimizationDisabled();
    EXPECT_FALSE(isOptDisabled);
}

TEST(SourceLevelDebugger, givenActiveDebuggerAndOptDisabledWhenGetDebuggerOptionReturnsNonZeroAndOneInValueThenIsOptimizationDisabledReturnsTrue) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    char value[2] = {'1', 0};
    GfxDbgOption optionArgOut;
    interceptor.optionArgOut = &optionArgOut;
    interceptor.optionArgOut->value = value;
    interceptor.optionArgOut->valueLen = sizeof(value);
    interceptor.optionRetVal = 1;

    MockSourceLevelDebugger debugger;
    debugger.callBaseIsOptimizationDisabled = true;
    bool isOptDisabled = debugger.isOptimizationDisabled();
    EXPECT_TRUE(isOptDisabled);
}

TEST(SourceLevelDebugger, givenActiveDebuggerAndOptDisabledWhenGetDebuggerOptionReturnsNonZeroAndZeroInValueThenIsOptimizationDisabledReturnsFalse) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    char value = '0';
    GfxDbgOption optionArgOut;
    interceptor.optionArgOut = &optionArgOut;
    interceptor.optionArgOut->value = &value;
    interceptor.optionArgOut->valueLen = sizeof(value);
    interceptor.optionRetVal = 1;

    MockSourceLevelDebugger debugger;
    debugger.callBaseIsOptimizationDisabled = true;
    bool isOptDisabled = debugger.isOptimizationDisabled();
    EXPECT_FALSE(isOptDisabled);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenNotifyKernelDebugDataIsCalledThenDebuggerLibraryFunctionIsCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    char isa[8];
    char dbgIsa[10];
    char visa[12];

    KernelInfo info;
    info.debugData.genIsa = dbgIsa;
    info.debugData.vIsa = visa;
    info.debugData.genIsaSize = sizeof(dbgIsa);
    info.debugData.vIsaSize = sizeof(visa);

    info.kernelDescriptor.kernelMetadata.kernelName = "debugKernel";

    info.heapInfo.kernelHeapSize = sizeof(isa);
    info.heapInfo.pKernelHeap = isa;

    debugger.callBaseNotifyKernelDebugData = true;
    debugger.notifyKernelDebugData(&info.debugData, info.kernelDescriptor.kernelMetadata.kernelName, info.heapInfo.pKernelHeap, info.heapInfo.kernelHeapSize);

    EXPECT_TRUE(interceptor.kernelDebugDataCalled);

    EXPECT_EQ(static_cast<uint32_t>(IGFXDBG_CURRENT_VERSION), interceptor.kernelDebugDataArgIn.version);
    EXPECT_EQ(reinterpret_cast<GfxDeviceHandle>(static_cast<uint64_t>(MockSourceLevelDebugger::mockDeviceHandle)), interceptor.kernelDebugDataArgIn.hDevice);
    EXPECT_EQ(reinterpret_cast<GenRtProgramHandle>(0), interceptor.kernelDebugDataArgIn.hProgram);

    EXPECT_EQ(dbgIsa, interceptor.kernelDebugDataArgIn.dbgGenIsaBuffer);
    EXPECT_EQ(sizeof(dbgIsa), interceptor.kernelDebugDataArgIn.dbgGenIsaSize);
    EXPECT_EQ(visa, interceptor.kernelDebugDataArgIn.dbgVisaBuffer);
    EXPECT_EQ(sizeof(visa), interceptor.kernelDebugDataArgIn.dbgVisaSize);

    EXPECT_EQ(info.heapInfo.kernelHeapSize, interceptor.kernelDebugDataArgIn.KernelBinSize);
    EXPECT_EQ(isa, interceptor.kernelDebugDataArgIn.kernelBinBuffer);
    EXPECT_STREQ(info.kernelDescriptor.kernelMetadata.kernelName.c_str(), interceptor.kernelDebugDataArgIn.kernelName);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenNullptrDebugDataIsPassedToNotifyThenDebuggerNotifiedWithNullPointersAndZeroSizes) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    char isa[8];

    KernelInfo info;
    info.kernelDescriptor.kernelMetadata.kernelName = "debugKernel";

    info.heapInfo.kernelHeapSize = sizeof(isa);
    info.heapInfo.pKernelHeap = isa;

    debugger.callBaseNotifyKernelDebugData = true;
    debugger.notifyKernelDebugData(nullptr, info.kernelDescriptor.kernelMetadata.kernelName, info.heapInfo.pKernelHeap, info.heapInfo.kernelHeapSize);

    EXPECT_TRUE(interceptor.kernelDebugDataCalled);

    EXPECT_EQ(static_cast<uint32_t>(IGFXDBG_CURRENT_VERSION), interceptor.kernelDebugDataArgIn.version);
    EXPECT_EQ(reinterpret_cast<GfxDeviceHandle>(static_cast<uint64_t>(MockSourceLevelDebugger::mockDeviceHandle)), interceptor.kernelDebugDataArgIn.hDevice);
    EXPECT_EQ(reinterpret_cast<GenRtProgramHandle>(0), interceptor.kernelDebugDataArgIn.hProgram);

    EXPECT_EQ(nullptr, interceptor.kernelDebugDataArgIn.dbgGenIsaBuffer);
    EXPECT_EQ(0u, interceptor.kernelDebugDataArgIn.dbgGenIsaSize);
    EXPECT_EQ(nullptr, interceptor.kernelDebugDataArgIn.dbgVisaBuffer);
    EXPECT_EQ(0u, interceptor.kernelDebugDataArgIn.dbgVisaSize);

    EXPECT_EQ(info.heapInfo.kernelHeapSize, interceptor.kernelDebugDataArgIn.KernelBinSize);
    EXPECT_EQ(isa, interceptor.kernelDebugDataArgIn.kernelBinBuffer);
    EXPECT_STREQ(info.kernelDescriptor.kernelMetadata.kernelName.c_str(), interceptor.kernelDebugDataArgIn.kernelName);
}

TEST(SourceLevelDebugger, givenNoVisaWhenNotifyKernelDebugDataIsCalledThenDebuggerLibraryFunctionIsCalledWithIsa) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    char isa[8];
    char dbgIsa[10];

    KernelInfo info;
    info.debugData.genIsa = dbgIsa;
    info.debugData.vIsa = nullptr;
    info.debugData.genIsaSize = sizeof(dbgIsa);
    info.debugData.vIsaSize = 0;

    info.kernelDescriptor.kernelMetadata.kernelName = "debugKernel";

    info.heapInfo.kernelHeapSize = sizeof(isa);
    info.heapInfo.pKernelHeap = isa;

    debugger.callBaseNotifyKernelDebugData = true;
    debugger.notifyKernelDebugData(&info.debugData, info.kernelDescriptor.kernelMetadata.kernelName, info.heapInfo.pKernelHeap, info.heapInfo.kernelHeapSize);
    EXPECT_TRUE(interceptor.kernelDebugDataCalled);
    EXPECT_EQ(isa, interceptor.kernelDebugDataArgIn.kernelBinBuffer);
}

TEST(SourceLevelDebugger, givenNoGenIsaWhenNotifyKernelDebugDataIsCalledThenDebuggerLibraryFunctionIsCalledWithIsa) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    char isa[8];
    char visa[12];

    KernelInfo info;
    info.debugData.genIsa = nullptr;
    info.debugData.vIsa = visa;
    info.debugData.genIsaSize = 0;
    info.debugData.vIsaSize = sizeof(visa);

    info.kernelDescriptor.kernelMetadata.kernelName = "debugKernel";

    info.heapInfo.kernelHeapSize = sizeof(isa);
    info.heapInfo.pKernelHeap = isa;

    debugger.callBaseNotifyKernelDebugData = true;
    debugger.notifyKernelDebugData(&info.debugData, info.kernelDescriptor.kernelMetadata.kernelName, isa, sizeof(isa));
    EXPECT_TRUE(interceptor.kernelDebugDataCalled);
    EXPECT_EQ(isa, interceptor.kernelDebugDataArgIn.kernelBinBuffer);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotActiveWhenNotifyKernelDebugDataIsCalledThenDebuggerLibraryFunctionIsNotCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    debugger.setActive(false);
    KernelInfo info;
    debugger.callBaseNotifyKernelDebugData = true;
    debugger.notifyKernelDebugData(&info.debugData, info.kernelDescriptor.kernelMetadata.kernelName, nullptr, 0);
    EXPECT_FALSE(interceptor.kernelDebugDataCalled);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenInitializeIsCalledWithLocalMemoryUsageFalseThenDebuggerFunctionIsCalledWithCorrectArg) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    debugger.callBaseInitialize = true;
    debugger.initialize(false);
    EXPECT_TRUE(interceptor.initCalled);
    EXPECT_FALSE(interceptor.targetCapsArgIn.supportsLocalMemory);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenInitializeReturnsErrorThenIsActiveIsSetToFalse) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    interceptor.initRetVal = IgfxdbgRetVal::IGFXDBG_FAILURE;
    debugger.callBaseInitialize = true;
    debugger.initialize(false);
    EXPECT_TRUE(interceptor.initCalled);
    EXPECT_FALSE(debugger.isDebuggerActive());
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryActiveWhenInitializeIsCalledWithLocalMemoryUsageTrueThenDebuggerFunctionIsCalledWithCorrectArg) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    debugger.callBaseInitialize = true;
    debugger.initialize(true);
    EXPECT_TRUE(interceptor.initCalled);
    EXPECT_TRUE(interceptor.targetCapsArgIn.supportsLocalMemory);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotActiveWhenInitializeIsCalledThenDebuggerFunctionIsNotCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;

    debugger.callBaseInitialize = true;
    debugger.initialize(false);
    EXPECT_FALSE(interceptor.initCalled);
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenKernelDebuggerLibraryActiveWhenDeviceIsConstructedThenDebuggerIsInitialized, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    auto executionEnvironment = MockDevice::prepareExecutionEnvironment(&hwInfo, 0u);
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithExecutionEnvironment<MockDevice>(&hwInfo, executionEnvironment, 0u));
    EXPECT_TRUE(interceptor.initCalled);
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenKernelDebuggerLibraryActiveWhenDeviceImplIsCreatedThenDebuggerIsNotified, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    auto executionEnvironment = MockDevice::prepareExecutionEnvironment(&hwInfo, 0u);
    unique_ptr<MockDevice> device(MockDevice::createWithExecutionEnvironment<MockDevice>(&hwInfo, executionEnvironment, 0u));
    unique_ptr<MockClDevice> pClDevice(new MockClDevice{device.get()});
    EXPECT_TRUE(interceptor.newDeviceCalled);
    uint32_t deviceHandleExpected = device->getGpgpuCommandStreamReceiver().getOSInterface() != nullptr ? device->getGpgpuCommandStreamReceiver().getOSInterface()->getDriverModel()->getDeviceHandle() : 0;
    EXPECT_EQ(reinterpret_cast<GfxDeviceHandle>(static_cast<uint64_t>(deviceHandleExpected)), interceptor.newDeviceArgIn.dh);
    pClDevice.reset();
    device.release();
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenKernelDebuggerLibraryActiveWhenDeviceImplIsCreatedWithOsCsrThenDebuggerIsNotifiedWithCorrectDeviceHandle, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    VariableBackup<UltHwConfig> backup(&ultHwConfig);
    ultHwConfig.useHwCsr = true;

    HardwareInfo *hwInfo = nullptr;
    ExecutionEnvironment *executionEnvironment = getExecutionEnvironmentImpl(hwInfo, 1);

    hwInfo->capabilityTable.debuggerSupported = true;
    hwInfo->capabilityTable.instrumentationEnabled = true;

    unique_ptr<MockDevice> device(Device::create<MockDevice>(executionEnvironment, 0));
    unique_ptr<MockClDevice> pClDevice(new MockClDevice{device.get()});

    ASSERT_NE(nullptr, device->getGpgpuCommandStreamReceiver().getOSInterface());

    EXPECT_TRUE(interceptor.newDeviceCalled);
    uint32_t deviceHandleExpected = device->getGpgpuCommandStreamReceiver().getOSInterface()->getDriverModel()->getDeviceHandle();
    EXPECT_EQ(reinterpret_cast<GfxDeviceHandle>(static_cast<uint64_t>(deviceHandleExpected)), interceptor.newDeviceArgIn.dh);
    device.release();
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotActiveWhenDeviceIsCreatedThenDebuggerIsNotCreatedInitializedAndNotNotified) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    EXPECT_EQ(nullptr, device->getDebugger());
    EXPECT_FALSE(interceptor.initCalled);
    EXPECT_FALSE(interceptor.newDeviceCalled);
}

TEST(SourceLevelDebugger, givenDefaultStateWhenDeviceIsCreatedThenLoadDebuggerLibraryIsNotCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    EXPECT_FALSE(interceptor.loadCalled);
}

TEST(SourceLevelDebugger, givenKernelDebuggerLibraryNotActiveWhenGettingSourceLevelDebuggerThenNullptrIsReturned) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    EXPECT_EQ(nullptr, device->getSourceLevelDebugger());
}

TEST(SourceLevelDebugger, givenDeviceWithDebuggerActiveSetWhenSourceLevelDebuggerIsNotCreatedThenNotificationsAreNotCalled) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(false);
    DebuggerLibrary::setDebuggerActive(false);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDeviceWithDebuggerActive>(nullptr));

    EXPECT_TRUE(device->isDebuggerActive());
    EXPECT_EQ(nullptr, device->getDebugger());
    EXPECT_FALSE(interceptor.newDeviceCalled);
    EXPECT_FALSE(interceptor.deviceDestructionCalled);
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenTwoRootDevicesWhenSecondIsCreatedThenCreatingNewSourceLevelDebugger, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    ExecutionEnvironment *executionEnvironment = platform()->peekExecutionEnvironment();
    executionEnvironment->prepareRootDeviceEnvironments(2);
    for (auto i = 0u; i < executionEnvironment->rootDeviceEnvironments.size(); i++) {
        executionEnvironment->rootDeviceEnvironments[i]->setHwInfoAndInitHelpers(&hwInfo);
        executionEnvironment->rootDeviceEnvironments[i]->initGmm();
    }
    auto device1 = std::make_unique<MockClDevice>(Device::create<MockDevice>(executionEnvironment, 0u));
    EXPECT_NE(nullptr, executionEnvironment->memoryManager);
    EXPECT_TRUE(interceptor.initCalled);

    interceptor.initCalled = false;
    auto device2 = std::make_unique<MockClDevice>(Device::create<MockDevice>(executionEnvironment, 1u));
    EXPECT_NE(nullptr, executionEnvironment->memoryManager);
    EXPECT_TRUE(interceptor.initCalled);
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenMultipleRootDevicesWhenCreatedThenUseDedicatedSourceLevelDebugger, HasSourceLevelDebuggerSupport) {
    DebuggerLibraryRestore restore;

    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);

    ExecutionEnvironment *executionEnvironment = platform()->peekExecutionEnvironment();
    executionEnvironment->prepareRootDeviceEnvironments(2);
    for (auto i = 0u; i < executionEnvironment->rootDeviceEnvironments.size(); i++) {
        executionEnvironment->rootDeviceEnvironments[i]->setHwInfoAndInitHelpers(&hwInfo);
        executionEnvironment->rootDeviceEnvironments[i]->initGmm();
    }
    auto device1 = std::make_unique<MockClDevice>(Device::create<MockDevice>(executionEnvironment, 0u));
    auto sourceLevelDebugger = device1->getDebugger();
    auto device2 = std::make_unique<MockClDevice>(Device::create<MockDevice>(executionEnvironment, 1u));
    EXPECT_NE(sourceLevelDebugger, device2->getDebugger());
}

TEST(SourceLevelDebugger, whenCaptureSBACalledThenNoCommandsAreAddedToStream) {
    ExecutionEnvironment *executionEnvironment = platform()->peekExecutionEnvironment();
    auto device = std::unique_ptr<Device>(Device::create<MockDevice>(executionEnvironment, 0u));
    MockSourceLevelDebugger debugger;

    CommandContainer container;
    container.initialize(device.get(), nullptr, HeapSize::defaultHeapSize, true, false);

    NEO::Debugger::SbaAddresses sbaAddresses = {};
    debugger.captureStateBaseAddress(*container.getCommandStream(), sbaAddresses, false);
    EXPECT_EQ(0u, container.getCommandStream()->getUsed());
}

TEST(SourceLevelDebugger, whenGetSbaTrackingCommandsSizeQueriedThenZeroIsReturned) {
    auto debugger = std::make_unique<SourceLevelDebugger>(new DebuggerLibrary);
    auto size = debugger->getSbaTrackingCommandsSize(3);
    EXPECT_EQ(0u, size);
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenEnableMockSourceLevelDebuggerWhenInitializingExecEnvThenActiveDebuggerWithEmptyInterfaceIsCreated, HasSourceLevelDebuggerSupport) {
    DebugManagerStateRestore stateRestore;
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(false);

    DebugManager.flags.EnableMockSourceLevelDebugger.set(1);
    auto executionEnvironment = MockDevice::prepareExecutionEnvironment(&hwInfo, 0u);
    MockPlatform platform(*executionEnvironment);
    platform.initializeWithNewDevices();

    auto debugger = static_cast<SourceLevelDebugger *>(executionEnvironment->rootDeviceEnvironments[0]->debugger.get());
    ASSERT_NE(nullptr, debugger);

    EXPECT_TRUE(debugger->isDebuggerActive());
    EXPECT_FALSE(debugger->initialize(false));
    debugger->notifyNewDevice(4);

    EXPECT_TRUE(debugger->isOptimizationDisabled());

    const char source[] = "sourceCode";
    string file;
    debugger->notifySourceCode(source, sizeof(source), file);

    char isa[8];
    char dbgIsa[10];
    char visa[12];

    KernelInfo info;
    info.debugData.genIsa = dbgIsa;
    info.debugData.vIsa = visa;
    info.debugData.genIsaSize = sizeof(dbgIsa);
    info.debugData.vIsaSize = sizeof(visa);

    info.kernelDescriptor.kernelMetadata.kernelName = "debugKernel";

    info.heapInfo.kernelHeapSize = sizeof(isa);
    info.heapInfo.pKernelHeap = isa;

    debugger->notifyKernelDebugData(&info.debugData, info.kernelDescriptor.kernelMetadata.kernelName, info.heapInfo.pKernelHeap, info.heapInfo.kernelHeapSize);
    debugger->notifyKernelDebugData(nullptr, info.kernelDescriptor.kernelMetadata.kernelName, info.heapInfo.pKernelHeap, info.heapInfo.kernelHeapSize);
    debugger->notifyKernelDebugData(nullptr, info.kernelDescriptor.kernelMetadata.kernelName, nullptr, 0);

    EXPECT_TRUE(debugger->notifyDeviceDestruction());
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenMode1InEnableMockSourceLevelDebuggerWhenDebuggerCreatedThenIsOptimizationDisabledReturnsTrue, HasSourceLevelDebuggerSupport) {
    DebugManagerStateRestore stateRestore;
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(false);

    DebugManager.flags.EnableMockSourceLevelDebugger.set(1);

    auto sld = std::unique_ptr<SourceLevelDebugger>(SourceLevelDebugger::create());
    EXPECT_TRUE(sld->isOptimizationDisabled());
}

HWTEST2_F(SourceLevelDebuggerSupportedFixture, givenMode2InEnableMockSourceLevelDebuggerWhenDebuggerCreatedThenIsOptimizationDisabledReturnsFalse, HasSourceLevelDebuggerSupport) {
    DebugManagerStateRestore stateRestore;
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setLibraryAvailable(false);

    DebugManager.flags.EnableMockSourceLevelDebugger.set(2);

    auto sld = std::unique_ptr<SourceLevelDebugger>(SourceLevelDebugger::create());
    EXPECT_FALSE(sld->isOptimizationDisabled());
}

TEST(SourceLevelDebugger, givenDebugVarDumpElfWhenNotifyKernelDebugDataIsCalledThenElfFileIsCreated) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.DebuggerLogBitmask.set(NEO::DebugVariables::DEBUGGER_LOG_BITMASK::DUMP_ELF);

    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    char isa[8];
    char dbgIsa[10];
    char visa[12];

    KernelInfo info;
    info.debugData.genIsa = dbgIsa;
    info.debugData.vIsa = visa;
    info.debugData.genIsaSize = sizeof(dbgIsa);
    info.debugData.vIsaSize = sizeof(visa);

    info.kernelDescriptor.kernelMetadata.kernelName = "debugKernel";

    info.heapInfo.kernelHeapSize = sizeof(isa);
    info.heapInfo.pKernelHeap = isa;

    std::string fileName = info.kernelDescriptor.kernelMetadata.kernelName + ".elf";
    EXPECT_FALSE(fileExists(fileName));

    debugger.callBaseNotifyKernelDebugData = true;
    debugger.notifyKernelDebugData(&info.debugData, info.kernelDescriptor.kernelMetadata.kernelName, info.heapInfo.pKernelHeap, info.heapInfo.kernelHeapSize);
    EXPECT_TRUE(fileExists(fileName));
    std::remove(fileName.c_str());
}

TEST(SourceLevelDebugger, givenDebugVarDumpElfWhenElfFileExistsWhileNotifyingDebugDataThenSuffixIsAppendedToFileName) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.DebuggerLogBitmask.set(NEO::DebugVariables::DEBUGGER_LOG_BITMASK::DUMP_ELF);

    DebuggerLibraryRestore restore;

    DebuggerLibraryInterceptor interceptor;
    DebuggerLibrary::setLibraryAvailable(true);
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::injectDebuggerLibraryInterceptor(&interceptor);

    MockSourceLevelDebugger debugger;
    char isa[8];
    char dbgIsa[10];
    char visa[12];

    KernelInfo info;
    info.debugData.genIsa = dbgIsa;
    info.debugData.vIsa = visa;
    info.debugData.genIsaSize = sizeof(dbgIsa);
    info.debugData.vIsaSize = sizeof(visa);

    info.kernelDescriptor.kernelMetadata.kernelName = "debugKernel";

    info.heapInfo.kernelHeapSize = sizeof(isa);
    info.heapInfo.pKernelHeap = isa;

    std::string fileName = info.kernelDescriptor.kernelMetadata.kernelName + ".elf";
    char data[4];
    writeDataToFile(fileName.c_str(), data, 4);
    EXPECT_TRUE(fileExists(fileName));

    std::string fileName2 = info.kernelDescriptor.kernelMetadata.kernelName + "_0.elf";
    debugger.callBaseNotifyKernelDebugData = true;
    debugger.notifyKernelDebugData(&info.debugData, info.kernelDescriptor.kernelMetadata.kernelName, info.heapInfo.pKernelHeap, info.heapInfo.kernelHeapSize);

    EXPECT_TRUE(fileExists(fileName2));

    std::remove(fileName.c_str());
    std::remove(fileName2.c_str());
}

TEST(SourceLevelDebugger, givenDebuggerLibraryAvailableAndExperimentalEnableSourceLevelDebuggerThenDebuggerIsCreated) {
    DebugManagerStateRestore stateRestore;
    DebuggerLibraryRestore restore;
    DebuggerLibrary::setDebuggerActive(true);
    DebuggerLibrary::setLibraryAvailable(true);

    DebugManager.flags.ExperimentalEnableSourceLevelDebugger.set(1);

    auto executionEnvironment = new ExecutionEnvironment();
    MockPlatform platform(*executionEnvironment);
    platform.initializeWithNewDevices();

    auto debugger = std::unique_ptr<Debugger>(Debugger::create(*executionEnvironment->rootDeviceEnvironments[0].get()));
    ASSERT_NE(nullptr, debugger.get());
    EXPECT_TRUE(debugger->isLegacy());
}

using LegacyDebuggerTest = ::testing::Test;

HWTEST2_F(LegacyDebuggerTest, givenNotXeHpOrXeHpgCoreAndDebugIsActiveThenDisableL3CacheInGmmHelperIsNotSet, IsNotXeHpOrXeHpgCore) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.EnableMockSourceLevelDebugger.set(1);
    auto executionEnvironment = new ExecutionEnvironment();
    MockPlatform platform(*executionEnvironment);
    platform.initializeWithNewDevices();

    EXPECT_FALSE(static_cast<MockGmmHelper *>(platform.getClDevice(0)->getDevice().getGmmHelper())->allResourcesUncached);
}

HWTEST2_F(LegacyDebuggerTest, givenXeHpOrXeHpgCoreAndDebugIsActiveThenDisableL3CacheInGmmHelperIsSet, IsXeHpOrXeHpgCore) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.EnableMockSourceLevelDebugger.set(1);

    auto hwInfo = *NEO::defaultHwInfo;
    hwInfo.capabilityTable.debuggerSupported = true;
    auto executionEnvironment = MockDevice::prepareExecutionEnvironment(&hwInfo, 0u);

    MockPlatform platform(*executionEnvironment);
    platform.initializeWithNewDevices();

    EXPECT_TRUE(static_cast<MockGmmHelper *>(platform.getClDevice(0)->getDevice().getGmmHelper())->allResourcesUncached);
}
