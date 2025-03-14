/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/built_ins/sip.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/xe_hp_core/hw_cmds.h"
#include "shared/test/common/cmd_parse/hw_parse.h"
#include "shared/test/common/mocks/mock_debugger.h"
#include "shared/test/common/mocks/mock_device.h"
#include "shared/test/common/test_macros/header/per_product_test_definitions.h"
#include "shared/test/common/test_macros/test.h"

#include <array>

using namespace NEO;

using PreemptionXeHPTest = ::testing::Test;

XEHPTEST_F(PreemptionXeHPTest, givenRevisionA0toBWhenProgrammingSipThenGlobalSipIsSet) {
    using PIPE_CONTROL = XeHpFamily::PIPE_CONTROL;
    using MI_LOAD_REGISTER_IMM = XeHpFamily::MI_LOAD_REGISTER_IMM;
    using STATE_SIP = XeHpFamily::STATE_SIP;
    HardwareInfo hwInfo = *NEO::defaultHwInfo.get();

    const auto &productHelper = *ProductHelper::get(hwInfo.platform.eProductFamily);

    std::array<uint32_t, 2> revisions = {productHelper.getHwRevIdFromStepping(REVID::REVISION_A0, hwInfo),
                                         productHelper.getHwRevIdFromStepping(REVID::REVISION_B, hwInfo)};

    for (auto revision : revisions) {
        hwInfo.platform.usRevId = revision;

        std::unique_ptr<MockDevice> mockDevice(NEO::MockDevice::createWithNewExecutionEnvironment<NEO::MockDevice>(&hwInfo, 0));
        mockDevice->getExecutionEnvironment()->rootDeviceEnvironments[0]->debugger.reset(new MockDebugger);
        auto sipAllocation = SipKernel::getSipKernel(*mockDevice).getSipAllocation();

        size_t requiredSize = PreemptionHelper::getRequiredStateSipCmdSize<FamilyType>(*mockDevice, false);
        StackVec<char, 1024> streamStorage(1024);
        LinearStream cmdStream{streamStorage.begin(), streamStorage.size()};

        auto expectedGlobalSipWaSize = sizeof(PIPE_CONTROL) + 2 * sizeof(MI_LOAD_REGISTER_IMM);
        EXPECT_EQ(expectedGlobalSipWaSize, requiredSize);
        PreemptionHelper::programStateSip<FamilyType>(cmdStream, *mockDevice, nullptr, nullptr);
        EXPECT_NE(0U, cmdStream.getUsed());

        GenCmdList cmdList;
        ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
            cmdList, ptrOffset(cmdStream.getCpuBase(), 0), cmdStream.getUsed()));

        auto itorLRI = findMmio<FamilyType>(cmdList.begin(), cmdList.end(), 0xE42C);
        EXPECT_NE(cmdList.end(), itorLRI);

        auto cmdLRI = genCmdCast<MI_LOAD_REGISTER_IMM *>(*itorLRI);
        auto sipAddress = cmdLRI->getDataDword() & 0xfffffff8;
        EXPECT_EQ(sipAllocation->getGpuAddressToPatch(), sipAddress);
    }
}

XEHPTEST_F(PreemptionXeHPTest, givenRevisionA0toBWhenProgrammingSipEndWaThenGlobalSipIsRestored) {
    using PIPE_CONTROL = XeHpFamily::PIPE_CONTROL;
    using MI_LOAD_REGISTER_IMM = XeHpFamily::MI_LOAD_REGISTER_IMM;
    using STATE_SIP = XeHpFamily::STATE_SIP;
    HardwareInfo hwInfo = *NEO::defaultHwInfo.get();

    const auto &productHelper = *ProductHelper::get(hwInfo.platform.eProductFamily);

    std::array<uint32_t, 2> revisions = {productHelper.getHwRevIdFromStepping(REVID::REVISION_A0, hwInfo),
                                         productHelper.getHwRevIdFromStepping(REVID::REVISION_B, hwInfo)};

    for (auto revision : revisions) {
        hwInfo.platform.usRevId = revision;

        std::unique_ptr<MockDevice> mockDevice(NEO::MockDevice::createWithNewExecutionEnvironment<NEO::MockDevice>(&hwInfo, 0));
        mockDevice->getExecutionEnvironment()->rootDeviceEnvironments[0]->debugger.reset(new MockDebugger);

        StackVec<char, 1024> streamStorage(1024);
        LinearStream cmdStream{streamStorage.begin(), streamStorage.size()};

        PreemptionHelper::programStateSipEndWa<FamilyType>(cmdStream, hwInfo, true);
        EXPECT_NE(0U, cmdStream.getUsed());

        GenCmdList cmdList;
        ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(
            cmdList, ptrOffset(cmdStream.getCpuBase(), 0), cmdStream.getUsed()));

        auto itorPC = find<PIPE_CONTROL *>(cmdList.begin(), cmdList.end());
        EXPECT_NE(cmdList.end(), itorPC);

        auto itorLRI = findMmio<FamilyType>(itorPC, cmdList.end(), 0xE42C);
        EXPECT_NE(cmdList.end(), itorLRI);

        auto cmdLRI = genCmdCast<MI_LOAD_REGISTER_IMM *>(*itorLRI);
        auto sipAddress = cmdLRI->getDataDword() & 0xfffffff8;
        EXPECT_EQ(0u, sipAddress);
    }
}
