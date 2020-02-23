/*
 * Copyright (C) 2017-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "opencl/test/unit_test/aub_tests/command_stream/aub_command_stream_fixture.h"

#include "core/command_stream/command_stream_receiver.h"
#include "core/device/device.h"
#include "core/helpers/hw_helper.h"
#include "core/os_interface/os_context.h"
#include "core/unit_tests/helpers/memory_management.h"
#include "opencl/source/command_queue/command_queue.h"
#include "opencl/source/command_stream/tbx_command_stream_receiver.h"
#include "opencl/test/unit_test/gen_common/gen_cmd_parse.h"
#include "opencl/test/unit_test/mocks/mock_device.h"
#include "opencl/test/unit_test/tests_configuration.h"

#include "gtest/gtest.h"

namespace NEO {

void AUBCommandStreamFixture::SetUp(CommandQueue *pCmdQ) {
    ASSERT_NE(pCmdQ, nullptr);
    auto &device = reinterpret_cast<MockDevice &>(pCmdQ->getDevice());
    const auto &hwInfo = device.getHardwareInfo();
    auto &hwHelper = HwHelper::get(hwInfo.platform.eRenderCoreFamily);

    const ::testing::TestInfo *const testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
    std::stringstream strfilename;
    auto engineType = pCmdQ->getGpgpuCommandStreamReceiver().getOsContext().getEngineType();
    strfilename << testInfo->test_case_name() << "_" << testInfo->name() << "_" << hwHelper.getCsTraits(engineType).name;

    if (testMode == TestMode::AubTestsWithTbx) {
        pCommandStreamReceiver = TbxCommandStreamReceiver::create(strfilename.str(), true, *device.executionEnvironment, device.getRootDeviceIndex());
    } else {
        pCommandStreamReceiver = AUBCommandStreamReceiver::create(strfilename.str(), true, *device.executionEnvironment, device.getRootDeviceIndex());
    }
    ASSERT_NE(nullptr, pCommandStreamReceiver);

    device.resetCommandStreamReceiver(pCommandStreamReceiver);

    CommandStreamFixture::SetUp(pCmdQ);

    pTagMemory = pCommandStreamReceiver->getTagAddress();
    this->commandQueue = pCmdQ;
}

void AUBCommandStreamFixture::TearDown() {
    CommandStreamFixture::TearDown();
}
} // namespace NEO
