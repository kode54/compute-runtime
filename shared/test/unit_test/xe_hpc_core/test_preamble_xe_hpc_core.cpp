/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_stream/stream_properties.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/os_interface/device_factory.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/test/common/fixtures/preamble_fixture.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"

using namespace NEO;

using PreambleCfeState = PreambleFixture;

XE_HPC_CORETEST_F(PreambleCfeState, givenXeHpcCoreAndSetDebugFlagWhenPreambleCfeStateIsProgrammedThenCFEStateParamsHaveSetValue) {
    using CFE_STATE = typename FamilyType::CFE_STATE;
    DebugManagerStateRestore dbgRestore;
    uint32_t expectedValue = 1u;

    DebugManager.flags.CFEComputeDispatchAllWalkerEnable.set(expectedValue);

    uint64_t expectedAddress = 1 << CFE_STATE::SCRATCHSPACEBUFFER_BIT_SHIFT;
    auto pVfeCmd = PreambleHelper<FamilyType>::getSpaceForVfeState(&linearStream, *defaultHwInfo, EngineGroupType::RenderCompute);
    StreamProperties emptyProperties{};
    PreambleHelper<FamilyType>::programVfeState(pVfeCmd, pDevice->getRootDeviceEnvironment(), 0u, expectedAddress, 16u, emptyProperties, nullptr);

    parseCommands<FamilyType>(linearStream);
    auto cfeStateIt = find<CFE_STATE *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), cfeStateIt);

    auto cfeState = reinterpret_cast<CFE_STATE *>(*cfeStateIt);

    EXPECT_EQ(expectedValue, cfeState->getComputeDispatchAllWalkerEnable());
    EXPECT_FALSE(cfeState->getSingleSliceDispatchCcsMode());
}

XE_HPC_CORETEST_F(PreambleCfeState, givenKernelExecutionTypeConcurrentAndRevisionBWhenCallingProgramVFEStateThenAllWalkerProperlyProgrammed) {
    using CFE_STATE = typename FamilyType::CFE_STATE;
    auto hwInfo = *defaultHwInfo;

    const auto &productHelper = pDevice->getProductHelper();
    hwInfo.platform.usRevId = productHelper.getHwRevIdFromStepping(REVISION_B, hwInfo);

    auto pVfeCmd = PreambleHelper<FamilyType>::getSpaceForVfeState(&linearStream, hwInfo, EngineGroupType::RenderCompute);
    StreamProperties streamProperties{};
    streamProperties.initSupport(pDevice->getRootDeviceEnvironment());
    streamProperties.frontEndState.setPropertiesAll(true, false, false, false);

    PreambleHelper<FamilyType>::programVfeState(pVfeCmd, pDevice->getRootDeviceEnvironment(), 0u, 0, 0, streamProperties, nullptr);
    parseCommands<FamilyType>(linearStream);
    auto cfeStateIt = find<CFE_STATE *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), cfeStateIt);
    auto cfeState = reinterpret_cast<CFE_STATE *>(*cfeStateIt);

    uint32_t expectedValue = streamProperties.frontEndState.computeDispatchAllWalkerEnable.isDirty ? streamProperties.frontEndState.computeDispatchAllWalkerEnable.value : 0;
    EXPECT_EQ(expectedValue, cfeState->getComputeDispatchAllWalkerEnable());
    EXPECT_FALSE(cfeState->getSingleSliceDispatchCcsMode());
}

XE_HPC_CORETEST_F(PreambleCfeState, givenNotSetDebugFlagWhenPreambleCfeStateIsProgrammedThenCFEStateParamsHaveNotSetValue) {
    using CFE_STATE = typename FamilyType::CFE_STATE;

    auto cfeState = reinterpret_cast<CFE_STATE *>(linearStream.getSpace(sizeof(CFE_STATE)));
    *cfeState = FamilyType::cmdInitCfeState;

    uint32_t numberOfWalkers = cfeState->getNumberOfWalkers();
    uint32_t overDispatchControl = static_cast<uint32_t>(cfeState->getOverDispatchControl());
    uint32_t singleDispatchCcsMode = cfeState->getSingleSliceDispatchCcsMode();

    uint64_t expectedAddress = 1 << CFE_STATE::SCRATCHSPACEBUFFER_BIT_SHIFT;
    uint32_t expectedMaxThreads = GfxCoreHelper::getMaxThreadsForVfe(*defaultHwInfo);
    auto pVfeCmd = PreambleHelper<FamilyType>::getSpaceForVfeState(&linearStream, *defaultHwInfo, EngineGroupType::RenderCompute);
    StreamProperties emptyProperties{};
    PreambleHelper<FamilyType>::programVfeState(pVfeCmd, pDevice->getRootDeviceEnvironment(), 0u, expectedAddress, expectedMaxThreads, emptyProperties, nullptr);
    uint32_t maximumNumberOfThreads = cfeState->getMaximumNumberOfThreads();

    EXPECT_EQ(numberOfWalkers, cfeState->getNumberOfWalkers());
    EXPECT_NE(expectedMaxThreads, maximumNumberOfThreads);
    EXPECT_EQ(overDispatchControl, static_cast<uint32_t>(cfeState->getOverDispatchControl()));
    EXPECT_EQ(singleDispatchCcsMode, cfeState->getSingleSliceDispatchCcsMode());
}

XE_HPC_CORETEST_F(PreambleCfeState, givenSetDebugFlagWhenPreambleCfeStateIsProgrammedThenCFEStateParamsHaveSetValue) {
    using CFE_STATE = typename FamilyType::CFE_STATE;

    uint32_t expectedValue1 = 1u;
    uint32_t expectedValue2 = 2u;

    DebugManagerStateRestore dbgRestore;

    DebugManager.flags.CFEFusedEUDispatch.set(expectedValue1);
    DebugManager.flags.CFEOverDispatchControl.set(expectedValue1);
    DebugManager.flags.CFESingleSliceDispatchCCSMode.set(expectedValue1);
    DebugManager.flags.CFELargeGRFThreadAdjustDisable.set(expectedValue1);
    DebugManager.flags.CFENumberOfWalkers.set(expectedValue2);
    DebugManager.flags.CFEMaximumNumberOfThreads.set(expectedValue2);

    uint64_t expectedAddress = 1 << CFE_STATE::SCRATCHSPACEBUFFER_BIT_SHIFT;
    auto pVfeCmd = PreambleHelper<FamilyType>::getSpaceForVfeState(&linearStream, *defaultHwInfo, EngineGroupType::RenderCompute);
    StreamProperties emptyProperties{};
    PreambleHelper<FamilyType>::programVfeState(pVfeCmd, pDevice->getRootDeviceEnvironment(), 0u, expectedAddress, 16u, emptyProperties, nullptr);

    parseCommands<FamilyType>(linearStream);
    auto cfeStateIt = find<CFE_STATE *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), cfeStateIt);

    auto cfeState = reinterpret_cast<CFE_STATE *>(*cfeStateIt);

    EXPECT_EQ(expectedValue1, cfeState->getSingleSliceDispatchCcsMode());
    EXPECT_EQ(expectedValue1, static_cast<uint32_t>(cfeState->getOverDispatchControl()));
    EXPECT_EQ(expectedValue1, cfeState->getLargeGRFThreadAdjustDisable());
    EXPECT_EQ(expectedValue2, cfeState->getNumberOfWalkers());
    EXPECT_EQ(expectedValue2, cfeState->getMaximumNumberOfThreads());
}
