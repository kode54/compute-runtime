/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/helpers/gfx_core_helper.h"

#include "level_zero/sysman/source/firmware_util/firmware_util.h"
#include "level_zero/sysman/source/linux/os_sysman_imp.h"
#include "level_zero/sysman/source/ras/linux/os_ras_imp_prelim.h"

namespace L0 {
namespace Sysman {

void LinuxRasSourceHbm::getSupportedRasErrorTypes(std::set<zes_ras_error_type_t> &errorType, OsSysman *pOsSysman, ze_bool_t isSubDevice, uint32_t subDeviceId) {
    LinuxSysmanImp *pLinuxSysmanImp = static_cast<LinuxSysmanImp *>(pOsSysman);
    FirmwareUtil *pFwInterface = pLinuxSysmanImp->getFwUtilInterface();
    if (pFwInterface != nullptr) {
        errorType.insert(ZES_RAS_ERROR_TYPE_CORRECTABLE);
        errorType.insert(ZES_RAS_ERROR_TYPE_UNCORRECTABLE);
    }
}

ze_result_t LinuxRasSourceHbm::osRasGetState(zes_ras_state_t &state, ze_bool_t clear) {
    if (pFwInterface == nullptr) {
        return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
    }
    uint32_t subDeviceCount = 0;
    subDeviceCount = NEO::GfxCoreHelper::getSubDevicesCount(&pDevice->getHardwareInfo());
    if (clear == true) {
        uint64_t errorCount = 0;
        ze_result_t result = pFwInterface->fwGetMemoryErrorCount(osRasErrorType, subDeviceCount, subdeviceId, errorCount);
        if (result != ZE_RESULT_SUCCESS) {
            return result;
        }
        errorBaseline = errorCount; // during clear update the error baseline value
    }
    uint64_t errorCount = 0;
    ze_result_t result = pFwInterface->fwGetMemoryErrorCount(osRasErrorType, subDeviceCount, subdeviceId, errorCount);
    if (result != ZE_RESULT_SUCCESS) {
        return result;
    }
    state.category[ZES_RAS_ERROR_CAT_NON_COMPUTE_ERRORS] = errorCount - errorBaseline;
    return ZE_RESULT_SUCCESS;
}

LinuxRasSourceHbm::LinuxRasSourceHbm(LinuxSysmanImp *pLinuxSysmanImp, zes_ras_error_type_t type, uint32_t subdeviceId) : pLinuxSysmanImp(pLinuxSysmanImp), osRasErrorType(type), subdeviceId(subdeviceId) {
    pFwInterface = pLinuxSysmanImp->getFwUtilInterface();
    pDevice = pLinuxSysmanImp->getSysmanDeviceImp();
}

} // namespace Sysman
} // namespace L0
