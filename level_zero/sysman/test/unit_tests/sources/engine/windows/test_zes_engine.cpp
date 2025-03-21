/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "level_zero/sysman/source/engine/windows/os_engine_imp.h"
#include "level_zero/sysman/test/unit_tests/sources/engine/windows/mock_engine.h"
#include "level_zero/sysman/test/unit_tests/sources/windows/mock_sysman_fixture.h"

namespace L0 {
namespace ult {

constexpr uint32_t engineHandleComponentCount = 3u;
class SysmanDeviceEngineFixture : public SysmanDeviceFixture {
  protected:
    std::unique_ptr<MockEngineKmdSysManager> pKmdSysManager = nullptr;
    L0::Sysman::KmdSysManager *pOriginalKmdSysManager = nullptr;
    void SetUp() override {
        SysmanDeviceFixture::SetUp();

        pKmdSysManager.reset(new MockEngineKmdSysManager);

        pOriginalKmdSysManager = pWddmSysmanImp->pKmdSysManager;
        pWddmSysmanImp->pKmdSysManager = pKmdSysManager.get();

        pSysmanDeviceImp->pEngineHandleContext->handleList.clear();
    }

    void TearDown() override {
        pWddmSysmanImp->pKmdSysManager = pOriginalKmdSysManager;
        SysmanDeviceFixture::TearDown();
    }

    std::vector<zes_engine_handle_t> get_engine_handles(uint32_t count) {
        std::vector<zes_engine_handle_t> handles(count, nullptr);
        EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, handles.data()), ZE_RESULT_SUCCESS);
        return handles;
    }
};

TEST_F(SysmanDeviceEngineFixture, GivenComponentCountZeroWhenEnumeratingEngineGroupsThenValidCountIsReturnedAndVerifySysmanPowerGetCallSucceeds) {
    uint32_t count = 0;
    EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, nullptr), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, engineHandleComponentCount);
}

TEST_F(SysmanDeviceEngineFixture, GivenInvalidComponentCountWhenEnumeratingEngineGroupsThenValidCountIsReturnedAndVerifySysmanPowerGetCallSucceeds) {
    uint32_t count = 0;
    EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, nullptr), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, engineHandleComponentCount);

    count = count + 1;
    EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, nullptr), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, engineHandleComponentCount);
}

TEST_F(SysmanDeviceEngineFixture, GivenComponentCountZeroWhenEnumeratingEngineGroupsThenValidPowerHandlesIsReturned) {
    uint32_t count = 0;
    EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, nullptr), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, engineHandleComponentCount);

    std::vector<zes_engine_handle_t> handles(count, nullptr);
    EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, handles.data()), ZE_RESULT_SUCCESS);
    for (auto handle : handles) {
        EXPECT_NE(handle, nullptr);
    }
}

TEST_F(SysmanDeviceEngineFixture, GivenComponentCountZeroWhenEnumeratingEngineGroupsAndRequestSingleFailsThenZeroHandlesAreReturned) {
    pKmdSysManager->mockRequestSingle = true;
    pKmdSysManager->mockRequestSingleResult = ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
    uint32_t count = 0;
    EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, nullptr), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, 0);
}

TEST_F(SysmanDeviceEngineFixture, GivenComponentCountZeroWhenEnumeratingEngineGroupsIfEngineSupportIsAbsentThenZeroHandlesAreReturned) {
    pKmdSysManager->mockNumSupportedEngineGroups = 0;
    uint32_t count = 0;
    EXPECT_EQ(zesDeviceEnumEngineGroups(pSysmanDevice->toHandle(), &count, nullptr), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, 0);
}

TEST_F(SysmanDeviceEngineFixture, GivenUnsupportedEngineHandleWhenGettingEngineActivityThenFailureIsReturned) {
    std::unique_ptr<L0::Sysman::WddmEngineImp> pEngineImp = std::make_unique<L0::Sysman::WddmEngineImp>(pOsSysman, ZES_ENGINE_GROUP_3D_ALL, 0, 0);
    zes_engine_stats_t pStats = {};
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_FEATURE, pEngineImp->getActivity(&pStats));
}

TEST_F(SysmanDeviceEngineFixture, GivenValidHandleGetPropertiesThenCorrectEngineGroupIsReturned) {
    auto handles = get_engine_handles(engineHandleComponentCount);
    uint32_t engineGroupIndex = 0;
    for (auto handle : handles) {
        zes_engine_properties_t properties = {};

        ze_result_t result = zesEngineGetProperties(handle, &properties);

        EXPECT_EQ(ZE_RESULT_SUCCESS, result);
        EXPECT_FALSE(properties.onSubdevice);
        EXPECT_EQ(properties.subdeviceId, 0);
        EXPECT_EQ(properties.type, pKmdSysManager->mockEngineTypes[engineGroupIndex++]);
    }
}

TEST_F(SysmanDeviceEngineFixture, GivenValidHandleGetAvtivityThenCorrectValuesAreReturned) {
    auto handles = get_engine_handles(engineHandleComponentCount);
    uint32_t engineGroupIndex = 0;
    for (auto handle : handles) {
        zes_engine_stats_t stats;

        ze_result_t result = zesEngineGetActivity(handle, &stats);

        EXPECT_EQ(ZE_RESULT_SUCCESS, result);
        EXPECT_EQ(stats.activeTime, pKmdSysManager->mockActivityCounters[engineGroupIndex]);
        EXPECT_EQ(stats.timestamp, pKmdSysManager->mockActivityTimeStamps[engineGroupIndex]);
        engineGroupIndex++;
    }
}

TEST_F(SysmanDeviceEngineFixture, GivenValidHandleWhenGettingEngineActivityAndRequestSingleFailsThenFailureIsReturned) {
    auto handles = get_engine_handles(engineHandleComponentCount);
    for (auto handle : handles) {
        pKmdSysManager->mockRequestSingle = true;
        pKmdSysManager->mockRequestSingleResult = ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
        zes_engine_stats_t stats;
        EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_FEATURE, zesEngineGetActivity(handle, &stats));
    }
}

} // namespace ult
} // namespace L0
