/*
 * Copyright (C) 2022-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/helpers/non_copyable_or_moveable.h"

#include "level_zero/sysman/source/linux/fs_access.h"
#include "level_zero/sysman/source/linux/pmt/pmt.h"
#include "level_zero/sysman/source/temperature/os_temperature.h"

#include "igfxfmid.h"

#include <memory>

namespace L0 {
namespace Sysman {

class SysfsAccess;
class PlatformMonitoringTech;
class LinuxTemperatureImp : public OsTemperature, NEO::NonCopyableOrMovableClass {
  public:
    ze_result_t getProperties(zes_temp_properties_t *pProperties) override;
    ze_result_t getSensorTemperature(double *pTemperature) override;
    bool isTempModuleSupported() override;
    void setSensorType(zes_temp_sensors_t sensorType);
    LinuxTemperatureImp(OsSysman *pOsSysman, ze_bool_t onSubdevice, uint32_t subdeviceId);
    LinuxTemperatureImp() = default;
    ~LinuxTemperatureImp() override = default;

  protected:
    PlatformMonitoringTech *pPmt = nullptr;
    zes_temp_sensors_t type = ZES_TEMP_SENSORS_GLOBAL;

  private:
    ze_result_t getGlobalMaxTemperature(double *pTemperature);
    ze_result_t getGlobalMinTemperature(double *pTemperature);
    ze_result_t getGpuMaxTemperature(double *pTemperature);
    ze_result_t getGpuMinTemperature(double *pTemperature);
    ze_result_t getMemoryMaxTemperature(double *pTemperature);
    ze_result_t getGlobalMaxTemperatureNoSubDevice(double *pTemperature);
    ze_result_t getGpuMaxTemperatureNoSubDevice(double *pTemperature);
    uint32_t subdeviceId = 0;
    ze_bool_t isSubdevice = 0;
    PRODUCT_FAMILY productFamily = IGFX_UNKNOWN;
};

} // namespace Sysman
} // namespace L0
