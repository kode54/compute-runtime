/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/helpers/non_copyable_or_moveable.h"

#include "level_zero/sysman/source/linux/fs_access.h"
#include "level_zero/sysman/source/linux/pmu/pmu_imp.h"
#include "level_zero/sysman/source/ras/os_ras.h"
#include "level_zero/sysman/source/sysman_const.h"
#include "level_zero/sysman/source/sysman_device_imp.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace L0 {
namespace Sysman {

class LinuxSysmanImp;
class FirmwareUtil;

class LinuxRasSources : NEO::NonCopyableOrMovableClass {
  public:
    virtual ze_result_t osRasGetState(zes_ras_state_t &state, ze_bool_t clear) = 0;
    virtual ~LinuxRasSources() = default;
};

class LinuxRasImp : public OsRas, NEO::NonCopyableOrMovableClass {
  public:
    ze_result_t osRasGetProperties(zes_ras_properties_t &properties) override;
    ze_result_t osRasGetState(zes_ras_state_t &state, ze_bool_t clear) override;
    ze_result_t osRasGetConfig(zes_ras_config_t *config) override;
    ze_result_t osRasSetConfig(const zes_ras_config_t *config) override;
    LinuxRasImp(OsSysman *pOsSysman, zes_ras_error_type_t type, ze_bool_t onSubdevice, uint32_t subdeviceId);
    LinuxRasImp() = default;
    ~LinuxRasImp() override = default;

  protected:
    zes_ras_error_type_t osRasErrorType = {};
    FsAccess *pFsAccess = nullptr;
    LinuxSysmanImp *pLinuxSysmanImp = nullptr;
    std::vector<std::unique_ptr<L0::Sysman::LinuxRasSources>> rasSources = {};

  private:
    void initSources();
    bool isSubdevice = false;
    uint32_t subdeviceId = 0;
    uint64_t totalThreshold = 0;
    uint64_t categoryThreshold[maxRasErrorCategoryCount] = {0};
};

class LinuxRasSourceGt : public LinuxRasSources {
  public:
    ze_result_t osRasGetState(zes_ras_state_t &state, ze_bool_t clear) override;
    static void getSupportedRasErrorTypes(std::set<zes_ras_error_type_t> &errorType, OsSysman *pOsSysman, ze_bool_t isSubDevice, uint32_t subDeviceId);
    LinuxRasSourceGt(LinuxSysmanImp *pLinuxSysmanImp, zes_ras_error_type_t type, ze_bool_t onSubdevice, uint32_t subdeviceId);
    LinuxRasSourceGt() = default;
    ~LinuxRasSourceGt() override;

  protected:
    LinuxSysmanImp *pLinuxSysmanImp = nullptr;
    zes_ras_error_type_t osRasErrorType = {};
    PmuInterface *pPmuInterface = nullptr;
    FsAccess *pFsAccess = nullptr;
    SysfsAccess *pSysfsAccess = nullptr;

  private:
    void initRasErrors(ze_bool_t clear);
    ze_result_t getPmuConfig(
        const std::string &eventDirectory,
        const std::vector<std::string> &listOfEvents,
        const std::string &errorFileToGetConfig,
        std::string &pmuConfig);
    ze_result_t getBootUpErrorCountFromSysfs(
        std::string nameOfError,
        const std::string &errorCounterDir,
        uint64_t &errorVal);
    void closeFds();
    int64_t groupFd = -1;
    std::vector<int64_t> memberFds = {};
    uint64_t initialErrorCount[maxRasErrorCategoryCount] = {0};
    std::map<zes_ras_error_cat_t, uint64_t> errorCategoryToEventCount;
    uint64_t totalEventCount = 0;
    bool isSubdevice = false;
    uint32_t subdeviceId = 0;
};

class LinuxRasSourceHbm : public LinuxRasSources {
  public:
    ze_result_t osRasGetState(zes_ras_state_t &state, ze_bool_t clear) override;
    static void getSupportedRasErrorTypes(std::set<zes_ras_error_type_t> &errorType, OsSysman *pOsSysman, ze_bool_t isSubDevice, uint32_t subDeviceId);
    LinuxRasSourceHbm(LinuxSysmanImp *pLinuxSysmanImp, zes_ras_error_type_t type, uint32_t subdeviceId);
    LinuxRasSourceHbm() = default;
    ~LinuxRasSourceHbm() override{};

  protected:
    LinuxSysmanImp *pLinuxSysmanImp = nullptr;
    zes_ras_error_type_t osRasErrorType = {};
    FirmwareUtil *pFwInterface = nullptr;
    SysmanDeviceImp *pDevice = nullptr;

  private:
    uint64_t errorBaseline = 0;
    uint32_t subdeviceId = 0;
};

} // namespace Sysman
} // namespace L0
