/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "level_zero/sysman/source/power/power.h"

#include "shared/source/helpers/basic_math.h"

#include "level_zero/sysman/source/os_sysman.h"
#include "level_zero/sysman/source/power/power_imp.h"

namespace L0 {
namespace Sysman {

PowerHandleContext::~PowerHandleContext() {
    for (Power *pPower : handleList) {
        delete pPower;
    }
}

void PowerHandleContext::createHandle(ze_bool_t isSubDevice, uint32_t subDeviceId) {
    Power *pPower = new PowerImp(pOsSysman, isSubDevice, subDeviceId);
    if (pPower->initSuccess == true) {
        handleList.push_back(pPower);
    } else {
        delete pPower;
    }
}
ze_result_t PowerHandleContext::init(uint32_t subDeviceCount) {
    // Create Handle for card level power
    createHandle(false, 0);

    for (uint32_t subDeviceId = 0; subDeviceId < subDeviceCount; subDeviceId++) {
        createHandle(true, subDeviceId);
    }

    return ZE_RESULT_SUCCESS;
}

void PowerHandleContext::initPower() {
    std::call_once(initPowerOnce, [this]() {
        this->init(pOsSysman->getSubDeviceCount());
    });
}

ze_result_t PowerHandleContext::powerGet(uint32_t *pCount, zes_pwr_handle_t *phPower) {
    initPower();
    uint32_t handleListSize = static_cast<uint32_t>(handleList.size());
    uint32_t numToCopy = std::min(*pCount, handleListSize);
    if (0 == *pCount || *pCount > handleListSize) {
        *pCount = handleListSize;
    }
    if (nullptr != phPower) {
        for (uint32_t i = 0; i < numToCopy; i++) {
            phPower[i] = handleList[i]->toHandle();
        }
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t PowerHandleContext::powerGetCardDomain(zes_pwr_handle_t *phPower) {
    initPower();
    if (nullptr == phPower) {
        return ZE_RESULT_ERROR_INVALID_NULL_POINTER;
    }

    for (uint32_t i = 0; i < handleList.size(); i++) {
        if (handleList[i]->isCardPower) {
            *phPower = handleList[i]->toHandle();
            return ZE_RESULT_SUCCESS;
        }
    }
    return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
}

} // namespace Sysman
} // namespace L0
