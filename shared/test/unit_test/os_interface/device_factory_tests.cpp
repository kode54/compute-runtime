/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/helpers/product_config_helper.h"
#include "shared/source/os_interface/device_factory.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/helpers/default_hw_info.h"
#include "shared/test/common/mocks/mock_execution_environment.h"
#include "shared/test/common/test_macros/hw_test.h"

using namespace NEO;

struct DeviceFactoryTests : ::testing::Test {
    void SetUp() override {
        ProductConfigHelper productConfigHelper{};
        auto &aotInfos = productConfigHelper.getDeviceAotInfo();

        for (const auto &aotInfo : aotInfos) {
            if (aotInfo.hwInfo->platform.eProductFamily == productFamily) {
                productConfig = aotInfo.aotConfig.value;
                if (!aotInfo.deviceAcronyms.empty()) {
                    productAcronym = aotInfo.deviceAcronyms.front().str();
                } else if (!aotInfo.rtlIdAcronyms.empty()) {
                    productAcronym = aotInfo.rtlIdAcronyms.front().str();
                }
                break;
            }
        }
    }

    DebugManagerStateRestore restore;
    uint32_t productConfig;
    std::string productAcronym;
};

TEST_F(DeviceFactoryTests, givenHwIpVersionOverrideWhenPrepareDeviceEnvironmentsForProductFamilyOverrideIsCalledThenCorrectValueIsSet) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    auto config = defaultHwInfo.get()->ipVersion.value;
    DebugManager.flags.OverrideHwIpVersion.set(config);

    bool success = DeviceFactory::prepareDeviceEnvironmentsForProductFamilyOverride(executionEnvironment);
    EXPECT_TRUE(success);
    EXPECT_EQ(config, executionEnvironment.rootDeviceEnvironments[0]->getHardwareInfo()->ipVersion.value);
    EXPECT_NE(0u, executionEnvironment.rootDeviceEnvironments[0]->getHardwareInfo()->platform.usDeviceID);
}

TEST_F(DeviceFactoryTests, givenHwIpVersionAndDeviceIdOverrideWhenPrepareDeviceEnvironmentsForProductFamilyOverrideIsCalledThenCorrectValueIsSet) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    auto config = defaultHwInfo.get()->ipVersion.value;
    DebugManager.flags.OverrideHwIpVersion.set(config);
    DebugManager.flags.ForceDeviceId.set("0x1234");

    bool success = DeviceFactory::prepareDeviceEnvironmentsForProductFamilyOverride(executionEnvironment);
    EXPECT_TRUE(success);
    EXPECT_EQ(config, executionEnvironment.rootDeviceEnvironments[0]->getHardwareInfo()->ipVersion.value);
    EXPECT_EQ(0x1234, executionEnvironment.rootDeviceEnvironments[0]->getHardwareInfo()->platform.usDeviceID);
}

TEST_F(DeviceFactoryTests, givenProductFamilyOverrideWhenPrepareDeviceEnvironmentsIsCalledThenCorrectValueIsSet) {
    if (productAcronym.empty()) {
        GTEST_SKIP();
    }
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    DebugManager.flags.ProductFamilyOverride.set(productAcronym);

    bool success = DeviceFactory::prepareDeviceEnvironmentsForProductFamilyOverride(executionEnvironment);
    EXPECT_TRUE(success);
    EXPECT_EQ(productConfig, executionEnvironment.rootDeviceEnvironments[0]->getHardwareInfo()->ipVersion.value);
}

TEST_F(DeviceFactoryTests, givenHwIpVersionAndProductFamilyOverrideWhenPrepareDeviceEnvironmentsIsCalledThenCorrectValueIsSet) {
    if (productAcronym.empty()) {
        GTEST_SKIP();
    }
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    DebugManager.flags.OverrideHwIpVersion.set(0x1234u);
    DebugManager.flags.ProductFamilyOverride.set(productAcronym);

    bool success = DeviceFactory::prepareDeviceEnvironmentsForProductFamilyOverride(executionEnvironment);
    EXPECT_TRUE(success);
    EXPECT_EQ(0x1234u, executionEnvironment.rootDeviceEnvironments[0]->getHardwareInfo()->ipVersion.value);
}
