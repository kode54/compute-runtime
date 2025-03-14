/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "level_zero/sysman/source/linux/fs_access.h"
#include "level_zero/sysman/source/scheduler/linux/os_scheduler_imp.h"

namespace L0 {
namespace ult {

const std::string preemptTimeoutMilliSecs("preempt_timeout_ms");
const std::string defaultPreemptTimeoutMilliSecs(".defaults/preempt_timeout_ms");
const std::string timesliceDurationMilliSecs("timeslice_duration_ms");
const std::string defaultTimesliceDurationMilliSecs(".defaults/timeslice_duration_ms");
const std::string heartbeatIntervalMilliSecs("heartbeat_interval_ms");
const std::string defaultHeartbeatIntervalMilliSecs(".defaults/heartbeat_interval_ms");
const std::string engineDir("engine");
const std::vector<std::string> listOfMockedEngines = {"rcs0", "bcs0", "vcs0", "vcs1", "vecs0"};

typedef struct SchedulerConfigValues {
    uint64_t defaultVal;
    uint64_t actualVal;
} SchedulerConfigValues_t;

typedef struct SchedulerConfig {
    SchedulerConfigValues_t timeOut;
    SchedulerConfigValues_t timeSclice;
    SchedulerConfigValues_t heartBeat;
} SchedulerConfig_t;

class SchedulerFileProperties {
    bool isAvailable = false;
    ::mode_t mode = 0;

  public:
    SchedulerFileProperties() = default;
    SchedulerFileProperties(bool isAvailable, ::mode_t mode) : isAvailable(isAvailable), mode(mode) {}

    bool getAvailability() {
        return isAvailable;
    }

    bool hasMode(::mode_t mode) {
        return mode & this->mode;
    }
};

struct MockSchedulerSysfsAccess : public L0::Sysman::SysfsAccess {

    ze_result_t mockReadFileFailureError = ZE_RESULT_SUCCESS;
    ze_result_t mockWriteFileStatus = ZE_RESULT_SUCCESS;
    ze_result_t mockGetScanDirEntryError = ZE_RESULT_SUCCESS;

    std::vector<ze_result_t> mockReadReturnValues{ZE_RESULT_SUCCESS, ZE_RESULT_SUCCESS, ZE_RESULT_SUCCESS, ZE_RESULT_ERROR_NOT_AVAILABLE};

    uint32_t mockReadCount = 0;

    bool mockReadReturnStatus = false;

    std::map<std::string, SchedulerConfig_t *> engineSchedMap;
    std::map<std::string, SchedulerFileProperties> engineSchedFilePropertiesMap;

    ze_result_t getValForError(const std::string file, uint64_t &val) {
        return ZE_RESULT_ERROR_NOT_AVAILABLE;
    }

    ze_result_t getValForErrorWhileWrite(const std::string file, const uint64_t val) {
        return ZE_RESULT_ERROR_NOT_AVAILABLE;
    }

    void cleanUpMap() {
        for (std::string mappedEngine : listOfMockedEngines) {
            auto it = engineSchedMap.find(mappedEngine);
            if (it != engineSchedMap.end()) {
                delete it->second;
            }
        }
    }

    ze_result_t getFileProperties(const std::string file, SchedulerFileProperties &fileProps) {
        auto iterator = engineSchedFilePropertiesMap.find(file);
        if (iterator != engineSchedFilePropertiesMap.end()) {
            fileProps = static_cast<SchedulerFileProperties>(iterator->second);
            return ZE_RESULT_SUCCESS;
        }
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_result_t setFileProperties(const std::string &engine, const std::string file, bool isAvailable, ::mode_t mode) {
        auto iterator = std::find(listOfMockedEngines.begin(), listOfMockedEngines.end(), engine);
        if (iterator != listOfMockedEngines.end()) {
            engineSchedFilePropertiesMap[engineDir + "/" + engine + "/" + file] = SchedulerFileProperties(isAvailable, mode);
            return ZE_RESULT_SUCCESS;
        }
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_result_t read(const std::string file, uint64_t &val) override {

        if (mockReadReturnStatus == true) {

            if (mockReadCount < mockReadReturnValues.size()) {
                ze_result_t returnValue = mockReadReturnValues[mockReadCount];
                mockReadCount++;
                return returnValue;
            }
        }

        if (mockReadFileFailureError != ZE_RESULT_SUCCESS) {
            return mockReadFileFailureError;
        }

        SchedulerFileProperties fileProperties;
        ze_result_t result = getFileProperties(file, fileProperties);
        if (ZE_RESULT_SUCCESS == result) {
            if (!fileProperties.getAvailability()) {
                return ZE_RESULT_ERROR_NOT_AVAILABLE;
            }
            if (!fileProperties.hasMode(S_IRUSR)) {
                return ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS;
            }
        } else {
            return ZE_RESULT_ERROR_UNKNOWN;
        }

        for (std::string mappedEngine : listOfMockedEngines) {
            if (file.find(mappedEngine) == std::string::npos) {
                continue;
            }
            auto it = engineSchedMap.find(mappedEngine);
            if (it == engineSchedMap.end()) {
                return ZE_RESULT_ERROR_NOT_AVAILABLE;
            }
            if (file.compare((file.length() - preemptTimeoutMilliSecs.length()),
                             preemptTimeoutMilliSecs.length(),
                             preemptTimeoutMilliSecs) == 0) {
                if (file.find(".defaults") != std::string::npos) {
                    val = it->second->timeOut.defaultVal;
                } else {
                    val = it->second->timeOut.actualVal;
                }
                return ZE_RESULT_SUCCESS;
            }
            if (file.compare((file.length() - timesliceDurationMilliSecs.length()),
                             timesliceDurationMilliSecs.length(),
                             timesliceDurationMilliSecs) == 0) {
                if (file.find(".defaults") != std::string::npos) {
                    val = it->second->timeSclice.defaultVal;
                } else {
                    val = it->second->timeSclice.actualVal;
                }
                return ZE_RESULT_SUCCESS;
            }
            if (file.compare((file.length() - heartbeatIntervalMilliSecs.length()),
                             heartbeatIntervalMilliSecs.length(),
                             heartbeatIntervalMilliSecs) == 0) {
                if (file.find(".defaults") != std::string::npos) {
                    val = it->second->heartBeat.defaultVal;
                } else {
                    val = it->second->heartBeat.actualVal;
                }
                return ZE_RESULT_SUCCESS;
            }
        }
        return ZE_RESULT_ERROR_NOT_AVAILABLE;
    }

    ze_result_t write(const std::string file, const uint64_t val) override {

        if (mockWriteFileStatus != ZE_RESULT_SUCCESS) {
            return mockWriteFileStatus;
        }

        SchedulerFileProperties fileProperties;
        ze_result_t result = getFileProperties(file, fileProperties);
        if (ZE_RESULT_SUCCESS == result) {
            if (!fileProperties.getAvailability()) {
                return ZE_RESULT_ERROR_NOT_AVAILABLE;
            }
            if (!fileProperties.hasMode(S_IWUSR)) {
                return ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS;
            }
        } else {
            return ZE_RESULT_ERROR_UNKNOWN;
        }

        for (std::string mappedEngine : listOfMockedEngines) { // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
            if (file.find(mappedEngine) == std::string::npos) {
                continue;
            }
            SchedulerConfig_t *schedConfig = new SchedulerConfig_t();
            if (file.compare((file.length() - preemptTimeoutMilliSecs.length()),
                             preemptTimeoutMilliSecs.length(),
                             preemptTimeoutMilliSecs) == 0) {
                if (file.find(".defaults") != std::string::npos) {
                    schedConfig->timeOut.defaultVal = val;
                } else {
                    schedConfig->timeOut.actualVal = val;
                }
                auto ret = engineSchedMap.emplace(mappedEngine, schedConfig);
                if (ret.second == false) {
                    auto itr = engineSchedMap.find(mappedEngine);
                    if (file.find(".defaults") != std::string::npos) {
                        itr->second->timeOut.defaultVal = val;
                    } else {
                        itr->second->timeOut.actualVal = val;
                    }
                    delete schedConfig;
                }
                return ZE_RESULT_SUCCESS;
            }
            if (file.compare((file.length() - timesliceDurationMilliSecs.length()),
                             timesliceDurationMilliSecs.length(),
                             timesliceDurationMilliSecs) == 0) {
                if (file.find(".defaults") != std::string::npos) {
                    schedConfig->timeSclice.defaultVal = val;
                } else {
                    schedConfig->timeSclice.actualVal = val;
                }
                auto ret = engineSchedMap.emplace(mappedEngine, schedConfig);
                if (ret.second == false) {
                    auto itr = engineSchedMap.find(mappedEngine);
                    if (file.find(".defaults") != std::string::npos) {
                        itr->second->timeSclice.defaultVal = val;
                    } else {
                        itr->second->timeSclice.actualVal = val;
                    }
                    delete schedConfig;
                }
                return ZE_RESULT_SUCCESS;
            }
            if (file.compare((file.length() - heartbeatIntervalMilliSecs.length()),
                             heartbeatIntervalMilliSecs.length(),
                             heartbeatIntervalMilliSecs) == 0) {
                if (file.find(".defaults") != std::string::npos) {
                    schedConfig->heartBeat.defaultVal = val;
                } else {
                    schedConfig->heartBeat.actualVal = val;
                }
                auto ret = engineSchedMap.emplace(mappedEngine, schedConfig);
                if (ret.second == false) {
                    auto itr = engineSchedMap.find(mappedEngine);
                    if (file.find(".defaults") != std::string::npos) {
                        itr->second->heartBeat.defaultVal = val;
                    } else {
                        itr->second->heartBeat.actualVal = val;
                    }
                    delete schedConfig;
                }
                return ZE_RESULT_SUCCESS;
            }
        }
        return ZE_RESULT_ERROR_NOT_AVAILABLE;
    }

    ze_result_t scanDirEntries(const std::string file, std::vector<std::string> &listOfEntries) override {

        if (mockGetScanDirEntryError != ZE_RESULT_SUCCESS) {
            return mockGetScanDirEntryError;
        }

        if (!isDirectoryAccessible(engineDir)) {
            return ZE_RESULT_ERROR_NOT_AVAILABLE;
        }
        if (!(engineDirectoryPermissions & S_IRUSR)) {
            return ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS;
        }
        listOfEntries = listOfMockedEngines;
        return ZE_RESULT_SUCCESS;
    }
    ze_result_t getscanDirEntriesStatusReturnError(const std::string file, std::vector<std::string> &listOfEntries) {
        return ZE_RESULT_ERROR_NOT_AVAILABLE;
    }

    void setEngineDirectoryPermission(::mode_t permission) {
        engineDirectoryPermissions = permission;
    }

    MockSchedulerSysfsAccess() = default;

  private:
    ::mode_t engineDirectoryPermissions = S_IRUSR | S_IWUSR;
    bool isDirectoryAccessible(const std::string dir) {
        if (dir.compare(engineDir) == 0) {
            return true;
        }
        return false;
    }
};

class PublicLinuxSchedulerImp : public L0::Sysman::LinuxSchedulerImp {
  public:
    using LinuxSchedulerImp::pSysfsAccess;
};

} // namespace ult
} // namespace L0
