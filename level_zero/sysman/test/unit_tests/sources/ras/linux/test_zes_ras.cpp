/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "level_zero/sysman/source/sysman_const.h"
#include "level_zero/sysman/test/unit_tests/sources/linux/mock_sysman_fixture.h"

#include "mock_fs_ras.h"

namespace L0 {
namespace ult {

constexpr uint32_t mockHandleCount = 0;
struct SysmanRasFixture : public SysmanDeviceFixture {
  protected:
    std::unique_ptr<MockRasFsAccess> pFsAccess;
    L0::Sysman::FsAccess *pFsAccessOriginal = nullptr;
    L0::Sysman::SysmanDevice *device = nullptr;
    void SetUp() override {
        SysmanDeviceFixture::SetUp();
        pFsAccess = std::make_unique<MockRasFsAccess>();
        pFsAccessOriginal = pLinuxSysmanImp->pFsAccess;
        pLinuxSysmanImp->pFsAccess = pFsAccess.get();
        pFsAccess->mockRootUser = true;
        pSysmanDeviceImp->pRasHandleContext->handleList.clear();
        pSysmanDeviceImp->pRasHandleContext->init(pOsSysman->getSubDeviceCount());
        device = pSysmanDevice;
    }
    void TearDown() override {
        pLinuxSysmanImp->pFsAccess = pFsAccessOriginal;
        SysmanDeviceFixture::TearDown();
    }

    std::vector<zes_ras_handle_t> getRasHandles(uint32_t count) {
        std::vector<zes_ras_handle_t> handles(count, nullptr);
        EXPECT_EQ(zesDeviceEnumRasErrorSets(device->toHandle(), &count, handles.data()), ZE_RESULT_SUCCESS);
        return handles;
    }
};

TEST_F(SysmanRasFixture, GivenValidRasContextWhenRetrievingRasHandlesThenSuccessIsReturned) {
    uint32_t count = 0;
    L0::Sysman::RasHandleContext *pRasHandleContext = new L0::Sysman::RasHandleContext(pSysmanDeviceImp->pOsSysman);
    ze_result_t result = pRasHandleContext->rasGet(&count, nullptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(count, mockHandleCount);
    delete pRasHandleContext;
}

TEST_F(SysmanRasFixture, GivenValidSysmanHandleWhenRasErrorSetsThenCorrectCountIsReported) {
    uint32_t count = 0;
    ze_result_t result = zesDeviceEnumRasErrorSets(device->toHandle(), &count, NULL);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(count, mockHandleCount);

    uint32_t testcount = count + 1;
    result = zesDeviceEnumRasErrorSets(device->toHandle(), &testcount, NULL);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(testcount, mockHandleCount);

    count = 0;
    std::vector<zes_ras_handle_t> handles(count, nullptr);
    EXPECT_EQ(zesDeviceEnumRasErrorSets(device->toHandle(), &count, handles.data()), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, mockHandleCount);

    bool isSubDevice = false;
    uint32_t subDeviceId = 0u;
    L0::Sysman::RasImp *pTestRasImp = new L0::Sysman::RasImp(pSysmanDeviceImp->pRasHandleContext->pOsSysman, ZES_RAS_ERROR_TYPE_CORRECTABLE, isSubDevice, subDeviceId);
    pSysmanDeviceImp->pRasHandleContext->handleList.push_back(pTestRasImp);
    EXPECT_EQ(zesDeviceEnumRasErrorSets(device->toHandle(), &count, nullptr), ZE_RESULT_SUCCESS);
    EXPECT_EQ(count, mockHandleCount + 1);

    testcount = count;

    handles.resize(testcount);
    EXPECT_EQ(zesDeviceEnumRasErrorSets(device->toHandle(), &testcount, handles.data()), ZE_RESULT_SUCCESS);
    EXPECT_EQ(testcount, mockHandleCount + 1);
    EXPECT_NE(nullptr, handles.data());

    pSysmanDeviceImp->pRasHandleContext->handleList.pop_back();
    delete pTestRasImp;
}

TEST_F(SysmanRasFixture, GivenValidRasHandleWhenGettingRasPropertiesThenSuccessIsReturned) {
    bool isSubDevice = false;
    uint32_t subDeviceId = 0u;
    L0::Sysman::RasImp *pTestRasImp = new L0::Sysman::RasImp(pSysmanDeviceImp->pRasHandleContext->pOsSysman, ZES_RAS_ERROR_TYPE_CORRECTABLE, isSubDevice, subDeviceId);
    pSysmanDeviceImp->pRasHandleContext->handleList.push_back(pTestRasImp);

    auto handles = getRasHandles(mockHandleCount + 1);

    for (auto handle : handles) {
        zes_ras_properties_t properties = {};
        EXPECT_EQ(ZE_RESULT_SUCCESS, zesRasGetProperties(handle, &properties));
        EXPECT_EQ(properties.pNext, nullptr);
        EXPECT_EQ(properties.onSubdevice, false);
        EXPECT_EQ(properties.subdeviceId, 0u);
        EXPECT_EQ(properties.type, ZES_RAS_ERROR_TYPE_CORRECTABLE);
    }
    pSysmanDeviceImp->pRasHandleContext->handleList.pop_back();
    delete pTestRasImp;
}

TEST_F(SysmanRasFixture, GivenValidRasHandleWhileCallingZesRasGetStateThenFailureIsReturned) {
    bool isSubDevice = false;
    uint32_t subDeviceId = 0u;
    L0::Sysman::RasImp *pTestRasImp = new L0::Sysman::RasImp(pSysmanDeviceImp->pRasHandleContext->pOsSysman, ZES_RAS_ERROR_TYPE_CORRECTABLE, isSubDevice, subDeviceId);
    pSysmanDeviceImp->pRasHandleContext->handleList.push_back(pTestRasImp);

    auto handles = getRasHandles(mockHandleCount + 1);

    for (auto handle : handles) {
        zes_ras_state_t state = {};
        EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_FEATURE, zesRasGetState(handle, 0, &state));
    }
    pSysmanDeviceImp->pRasHandleContext->handleList.pop_back();
    delete pTestRasImp;
}

TEST_F(SysmanRasFixture, GivenValidRasHandleWhenCallingzesRasGetConfigAfterzesRasSetConfigThenSuccessIsReturned) {
    bool isSubDevice = false;
    uint32_t subDeviceId = 0u;
    L0::Sysman::RasImp *pTestRasImp = new L0::Sysman::RasImp(pSysmanDeviceImp->pRasHandleContext->pOsSysman, ZES_RAS_ERROR_TYPE_CORRECTABLE, isSubDevice, subDeviceId);
    pSysmanDeviceImp->pRasHandleContext->handleList.push_back(pTestRasImp);

    auto handles = getRasHandles(mockHandleCount + 1);

    for (auto handle : handles) {
        zes_ras_config_t setConfig = {};
        zes_ras_config_t getConfig = {};
        setConfig.totalThreshold = 50;
        memset(setConfig.detailedThresholds.category, 1, maxRasErrorCategoryCount * sizeof(uint64_t));
        EXPECT_EQ(ZE_RESULT_SUCCESS, zesRasSetConfig(handle, &setConfig));
        EXPECT_EQ(ZE_RESULT_SUCCESS, zesRasGetConfig(handle, &getConfig));
        EXPECT_EQ(setConfig.totalThreshold, getConfig.totalThreshold);
        int compare = std::memcmp(setConfig.detailedThresholds.category, getConfig.detailedThresholds.category, maxRasErrorCategoryCount * sizeof(uint64_t));
        EXPECT_EQ(0, compare);
    }
    pSysmanDeviceImp->pRasHandleContext->handleList.pop_back();
    delete pTestRasImp;
}

TEST_F(SysmanRasFixture, GivenValidRasHandleWhenCallingzesRasSetConfigWithoutPermissionThenFailureIsReturned) {
    pFsAccess->mockRootUser = false;
    bool isSubDevice = false;
    uint32_t subDeviceId = 0u;
    L0::Sysman::RasImp *pTestRasImp = new L0::Sysman::RasImp(pSysmanDeviceImp->pRasHandleContext->pOsSysman, ZES_RAS_ERROR_TYPE_CORRECTABLE, isSubDevice, subDeviceId);
    pSysmanDeviceImp->pRasHandleContext->handleList.push_back(pTestRasImp);

    auto handles = getRasHandles(mockHandleCount + 1);

    for (auto handle : handles) {
        zes_ras_config_t setConfig = {};
        setConfig.totalThreshold = 50;
        memset(setConfig.detailedThresholds.category, 1, maxRasErrorCategoryCount * sizeof(uint64_t));
        EXPECT_EQ(ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS, zesRasSetConfig(handle, &setConfig));
    }
    pSysmanDeviceImp->pRasHandleContext->releaseRasHandles();
}

TEST_F(SysmanRasFixture, GivenValidInstanceWhenOsRasImplementationIsNullThenDestructorIsCalledWithoutException) {

    L0::Sysman::RasImp *pTestRasImp = new L0::Sysman::RasImp();
    pTestRasImp->pOsRas = nullptr;
    EXPECT_NO_THROW(delete pTestRasImp;); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
}

} // namespace ult
} // namespace L0
