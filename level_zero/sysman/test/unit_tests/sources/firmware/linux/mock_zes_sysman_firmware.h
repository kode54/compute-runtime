/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "level_zero/sysman/source/firmware/linux/os_firmware_imp.h"
#include "level_zero/sysman/test/unit_tests/sources/linux/mock_sysman_fixture.h"

namespace L0 {
namespace ult {

constexpr uint32_t mockHandleCount = 2;
const std::string mockFwVersion("DG01->0->2026");
const std::string mockOpromVersion("OPROM CODE VERSION:123_OPROM DATA VERSION:456");
std::vector<std::string> mockSupportedFwTypes = {"GSC", "OptionROM"};
std::vector<std::string> mockUnsupportedFwTypes = {"unknown"};
std::string mockEmpty = {};
class FirmwareInterface : public L0::Sysman::FirmwareUtil {};
class FirmwareFsAccess : public L0::Sysman::FsAccess {};

struct MockFirmwareFsAccess : public FirmwareFsAccess {
    ze_bool_t isReadFwTypes = true;
    ze_result_t read(const std::string file, std::vector<std::string> &val) override {
        if (isReadFwTypes) {
            val.push_back("mtd3: 005ef000 00001000 \"i915-spi.42.auto.GSC\"");
            val.push_back("mtd5: 00200000 00001000 \"i915-spi.42.auto.OptionROM\"");
        } else {
            val.push_back("mtd3: 005ef000 00001000 \"i915-spi.42.auto.GSC\"");
            val.push_back("mtd3: 005ef000 00001000 \"i915-spi.42.auto.GSC\"");
        }
        return ZE_RESULT_SUCCESS;
    }
};

struct MockFirmwareInterface : public L0::Sysman::FirmwareUtil {

    ze_result_t getFwVersionResult = ZE_RESULT_SUCCESS;

    ze_result_t mockFwGetVersion(std::string &fwVersion) {
        fwVersion = mockFwVersion;
        return ZE_RESULT_SUCCESS;
    }
    ze_result_t mockOpromGetVersion(std::string &fwVersion) {
        fwVersion = mockOpromVersion;
        return ZE_RESULT_SUCCESS;
    }
    ze_result_t getFwVersion(std::string fwType, std::string &firmwareVersion) override {

        if (getFwVersionResult != ZE_RESULT_SUCCESS) {
            return getFwVersionResult;
        }

        if (fwType == "GSC") {
            firmwareVersion = mockFwVersion;
        } else if (fwType == "OptionROM") {
            firmwareVersion = mockOpromVersion;
        }
        return ZE_RESULT_SUCCESS;
    }

    void getDeviceSupportedFwTypes(std::vector<std::string> &fwTypes) override {
        fwTypes = mockSupportedFwTypes;
    }

    MockFirmwareInterface() = default;

    ADDMETHOD_NOBASE(fwDeviceInit, ze_result_t, ZE_RESULT_SUCCESS, ());
    ADDMETHOD_NOBASE(getFirstDevice, ze_result_t, ZE_RESULT_SUCCESS, (igsc_device_info * info));
    ADDMETHOD_NOBASE(flashFirmware, ze_result_t, ZE_RESULT_SUCCESS, (std::string fwType, void *pImage, uint32_t size));
    ADDMETHOD_NOBASE(fwIfrApplied, ze_result_t, ZE_RESULT_SUCCESS, (bool &ifrStatus));
    ADDMETHOD_NOBASE(fwSupportedDiagTests, ze_result_t, ZE_RESULT_SUCCESS, (std::vector<std::string> & supportedDiagTests));
    ADDMETHOD_NOBASE(fwRunDiagTests, ze_result_t, ZE_RESULT_SUCCESS, (std::string & osDiagType, zes_diag_result_t *pResult));
    ADDMETHOD_NOBASE(fwGetMemoryErrorCount, ze_result_t, ZE_RESULT_SUCCESS, (zes_ras_error_type_t category, uint32_t subDeviceCount, uint32_t subDeviceId, uint64_t &count));
    ADDMETHOD_NOBASE(fwGetEccConfig, ze_result_t, ZE_RESULT_SUCCESS, (uint8_t * currentState, uint8_t *pendingState));
    ADDMETHOD_NOBASE(fwSetEccConfig, ze_result_t, ZE_RESULT_SUCCESS, (uint8_t newState, uint8_t *currentState, uint8_t *pendingState));
    ADDMETHOD_NOBASE_VOIDRETURN(fwGetMemoryHealthIndicator, (zes_mem_health_t * health));
};

class PublicLinuxFirmwareImp : public L0::Sysman::LinuxFirmwareImp {
  public:
    using LinuxFirmwareImp::pFwInterface;
};
} // namespace ult
} // namespace L0
