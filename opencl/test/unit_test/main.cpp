/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/gmm_helper/gmm_interface.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/utilities/debug_settings_reader.h"
#include "shared/test/common/helpers/custom_event_listener.h"
#include "shared/test/common/helpers/default_hw_info.inl"
#include "shared/test/common/helpers/kernel_binary_helper.h"
#include "shared/test/common/helpers/memory_leak_listener.h"
#include "shared/test/common/helpers/test_files.h"
#include "shared/test/common/helpers/ult_hw_config.inl"
#include "shared/test/common/libult/global_environment.h"
#include "shared/test/common/libult/signal_utils.h"
#include "shared/test/common/mocks/mock_gmm_client_context.h"
#include "shared/test/common/mocks/mock_sip.h"
#include "shared/test/common/test_macros/test_checks_shared.h"
#include "shared/test/common/test_stats.h"
#include "shared/test/common/tests_configuration.h"

#include "opencl/source/os_interface/ocl_reg_path.h"
#include "opencl/source/platform/platform.h"
#include "opencl/test/unit_test/ult_config_listener.h"

#include "hw_cmds_default.h"
#include "test_files_setup.h"

#include <algorithm>
#include <fstream>
#include <limits.h>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef WIN32
const char *fSeparator = "\\";
#else
const char *fSeparator = "/";
#endif

namespace NEO {
extern const char *hardwarePrefix[];
extern const HardwareInfo *hardwareInfoTable[IGFX_MAX_PRODUCT];
extern const char *executionName;

extern bool useMockGmm;
extern TestMode testMode;
extern const char *executionDirectorySuffix;

std::thread::id tempThreadID;

namespace PagaFaultManagerTestConfig {
bool disabled = false;
}
} // namespace NEO

using namespace NEO;

extern PRODUCT_FAMILY productFamily;
extern GFXCORE_FAMILY renderCoreFamily;

extern std::string lastTest;
bool generateRandomInput = false;

void applyWorkarounds() {
    platformsImpl = new std::vector<std::unique_ptr<Platform>>;
    platformsImpl->reserve(1);
    {
        std::ofstream f;
        const std::string fileName("_tmp_");
        f.open(fileName, std::ofstream::binary);
        f.close();
    }
    {
        std::mutex mtx;
        std::unique_lock<std::mutex> stateLock(mtx);
    }
    {
        std::stringstream ss("1");
        int val;
        ss >> val;
    }

    // intialize rand
    srand(static_cast<unsigned int>(time(nullptr)));

    // Create at least on thread to prevent false memory leaks in tests using threads
    std::thread t([&]() {
    });
    tempThreadID = t.get_id();
    t.join();

    // Create FileLogger to prevent false memory leaks
    {
        NEO::fileLoggerInstance();
    }
}

void initializeTestHelpers(TestMode currentTestmode) {
    MockSipData::mockSipKernel.reset(new MockSipKernel());
    if (currentTestmode == TestMode::AubTests || currentTestmode == TestMode::AubTestsWithTbx) {
        MockSipData::useMockSip = false;
    }
}

void cleanTestHelpers() {
    delete platformsImpl;
}

std::string getRunPath(char *argv0) {
    std::string res(argv0);

    auto pos = res.rfind(fSeparator);
    if (pos != std::string::npos)
        res = res.substr(0, pos);

    if (res == "." || pos == std::string::npos) {
        char *cwd;
#if defined(__linux__)
        cwd = getcwd(nullptr, 0);
#else
        cwd = _getcwd(nullptr, 0);
#endif
        res = cwd;
        free(cwd);
    }

    return res;
}

int main(int argc, char **argv) {
    int retVal = 0;
    bool useDefaultListener = false;
    bool enableAbrt = true;
    bool enableAlarm = true;
    bool enableSegv = true;
    bool showTestStats = false;
    bool dumpTestStats = false;
    std::string dumpTestStatsFileName = "";
    applyWorkarounds();

#if defined(__linux__)
    if (getenv("IGDRCL_TEST_SELF_EXEC") == nullptr) {
        std::string wd = getRunPath(argv[0]);
        char *ldLibraryPath = getenv("LD_LIBRARY_PATH");

        if (ldLibraryPath == nullptr) {
            setenv("LD_LIBRARY_PATH", wd.c_str(), 1);
        } else {
            std::string ldLibraryPathConcat = wd + ":" + std::string(ldLibraryPath);
            setenv("LD_LIBRARY_PATH", ldLibraryPathConcat.c_str(), 1);
        }

        setenv("IGDRCL_TEST_SELF_EXEC", wd.c_str(), 1);
        execv(argv[0], argv);
        printf("FATAL ERROR: cannot self-exec test: %s!, errno: %d\n", argv[0], errno);
        return -1;
    }
#endif

    ::testing::InitGoogleTest(&argc, argv);
    HardwareInfo hwInfoForTests = DEFAULT_TEST_PLATFORM::hwInfo;

    uint32_t euPerSubSlice = 0;
    uint32_t sliceCount = 0;
    uint32_t subSlicePerSliceCount = 0;
    int32_t revId = -1;
    int dieRecovery = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp("--disable_default_listener", argv[i])) {
            useDefaultListener = false;
        } else if (!strcmp("--enable_default_listener", argv[i])) {
            useDefaultListener = true;
        } else if (!strcmp("--disable_alarm", argv[i])) {
            enableAlarm = false;
        } else if (!strcmp("--show_test_stats", argv[i])) {
            showTestStats = true;
        } else if (!strcmp("--dump_test_stats", argv[i])) {
            dumpTestStats = true;
            ++i;
            dumpTestStatsFileName = std::string(argv[i]);
        } else if (!strcmp("--disable_pagefaulting_tests", argv[i])) { // disable tests which raise page fault signal during execution
            NEO::PagaFaultManagerTestConfig::disabled = true;
        } else if (!strcmp("--tbx", argv[i])) {
            if (testMode == TestMode::AubTests) {
                testMode = TestMode::AubTestsWithTbx;
            }
            initialHardwareTag = 0;
        } else if (!strcmp("--rev_id", argv[i])) {
            ++i;
            if (i < argc) {
                revId = atoi(argv[i]);
            }
        } else if (!strcmp("--product", argv[i])) {
            ++i;
            if (i < argc) {
                if (::isdigit(argv[i][0])) {
                    int productValue = atoi(argv[i]);
                    if (productValue > 0 && productValue < IGFX_MAX_PRODUCT && hardwarePrefix[productValue] != nullptr) {
                        productFamily = static_cast<PRODUCT_FAMILY>(productValue);
                    } else {
                        productFamily = IGFX_UNKNOWN;
                    }
                } else {
                    productFamily = IGFX_UNKNOWN;
                    for (int j = 0; j < IGFX_MAX_PRODUCT; j++) {
                        if (hardwarePrefix[j] == nullptr)
                            continue;
                        if (strcmp(hardwarePrefix[j], argv[i]) == 0) {
                            productFamily = static_cast<PRODUCT_FAMILY>(j);
                            break;
                        }
                    }
                }
                if (productFamily == IGFX_UNKNOWN) {
                    std::cout << "unknown or unsupported product family has been set: " << argv[i] << std::endl;
                    return -1;
                } else {
                    std::cout << "product family: " << hardwarePrefix[productFamily] << " (" << productFamily << ")" << std::endl;
                }
                hwInfoForTests = *hardwareInfoTable[productFamily];
            }
        } else if (!strcmp("--slices", argv[i])) {
            ++i;
            if (i < argc) {
                sliceCount = atoi(argv[i]);
            }
        } else if (!strcmp("--subslices", argv[i])) {
            ++i;
            if (i < argc) {
                subSlicePerSliceCount = atoi(argv[i]);
            }
        } else if (!strcmp("--eu_per_ss", argv[i])) {
            ++i;
            if (i < argc) {
                euPerSubSlice = atoi(argv[i]);
            }
        } else if (!strcmp("--die_recovery", argv[i])) {
            ++i;
            if (i < argc) {
                dieRecovery = atoi(argv[i]) ? 1 : 0;
            }
        } else if (!strcmp("--generate_random_inputs", argv[i])) {
            generateRandomInput = true;
        } else if (!strcmp("--read-config", argv[i]) && (testMode == TestMode::AubTests || testMode == TestMode::AubTestsWithTbx)) {
            if (DebugManager.registryReadAvailable()) {
                DebugManager.setReaderImpl(SettingsReader::create(oclRegPath));
                DebugManager.injectSettingsFromReader();
            }
        } else if (!strcmp("--dump_buffer_format", argv[i]) && testMode == TestMode::AubTests) {
            ++i;
            std::string dumpBufferFormat(argv[i]);
            std::transform(dumpBufferFormat.begin(), dumpBufferFormat.end(), dumpBufferFormat.begin(), ::toupper);
            DebugManager.flags.AUBDumpBufferFormat.set(dumpBufferFormat);
        } else if (!strcmp("--dump_image_format", argv[i]) && testMode == TestMode::AubTests) {
            ++i;
            std::string dumpImageFormat(argv[i]);
            std::transform(dumpImageFormat.begin(), dumpImageFormat.end(), dumpImageFormat.begin(), ::toupper);
            DebugManager.flags.AUBDumpImageFormat.set(dumpImageFormat);
        }
    }

    productFamily = hwInfoForTests.platform.eProductFamily;
    renderCoreFamily = hwInfoForTests.platform.eRenderCoreFamily;
    PLATFORM &platform = hwInfoForTests.platform;
    if (revId != -1) {
        platform.usRevId = revId;
    } else {
        revId = platform.usRevId;
    }

    adjustHwInfoForTests(hwInfoForTests, euPerSubSlice, sliceCount, subSlicePerSliceCount, dieRecovery);

    binaryNameSuffix.append(hardwarePrefix[hwInfoForTests.platform.eProductFamily]);

    std::string nBinaryKernelFiles = getRunPath(argv[0]);
    nBinaryKernelFiles.append("/");
    nBinaryKernelFiles.append(binaryNameSuffix);
    nBinaryKernelFiles.append("/");
    nBinaryKernelFiles.append(std::to_string(revId));
    nBinaryKernelFiles.append("/");
    nBinaryKernelFiles.append(testFiles);
    testFiles = nBinaryKernelFiles;

    std::string nClFiles = NEO_OPENCL_TEST_FILES_DIR;
    nClFiles.append("/");
    clFiles = nClFiles;

    std::string executionDirectory("");
    if (testMode != TestMode::AubTests) {
        executionDirectory += "opencl/";
    }
    executionDirectory += hardwarePrefix[productFamily];
    executionDirectory += NEO::executionDirectorySuffix; // _aub for aub_tests, empty otherwise
    executionDirectory += "/";
    executionDirectory += std::to_string(revId);

#ifdef WIN32
#include <direct.h>
    if (_chdir(executionDirectory.c_str())) {
        std::cout << "chdir into " << executionDirectory << " directory failed.\nThis might cause test failures." << std::endl;
    }
#elif defined(__linux__)
#include <unistd.h>
    if (chdir(executionDirectory.c_str()) != 0) {
        std::cout << "chdir into " << executionDirectory << " directory failed.\nThis might cause test failures." << std::endl;
    }
#endif

    defaultHwInfo = std::make_unique<HardwareInfo>();
    *defaultHwInfo = hwInfoForTests;

    auto &listeners = ::testing::UnitTest::GetInstance()->listeners();
    if (useDefaultListener == false) {
        auto defaultListener = listeners.default_result_printer();

        auto customEventListener = new CCustomEventListener(defaultListener, hardwarePrefix[productFamily]);

        listeners.Release(defaultListener);
        listeners.Append(customEventListener);
    }

    listeners.Append(new MemoryLeakListener);
    listeners.Append(new UltConfigListener);

    gEnvironment = reinterpret_cast<TestEnvironment *>(::testing::AddGlobalTestEnvironment(new TestEnvironment));

    MockCompilerDebugVars fclDebugVars;
    MockCompilerDebugVars igcDebugVars;

    std::string builtInsFileName;
    if (TestChecks::supportsImages(defaultHwInfo)) {
        builtInsFileName = KernelBinaryHelper::BUILT_INS_WITH_IMAGES;
    } else {
        builtInsFileName = KernelBinaryHelper::BUILT_INS;
    }
    retrieveBinaryKernelFilename(fclDebugVars.fileName, builtInsFileName + "_", ".bc");
    retrieveBinaryKernelFilename(igcDebugVars.fileName, builtInsFileName + "_", ".bin");

    gEnvironment->setMockFileNames(fclDebugVars.fileName, igcDebugVars.fileName);
    gEnvironment->setDefaultDebugVars(fclDebugVars, igcDebugVars, hwInfoForTests);

    int sigOut = setAlarm(enableAlarm);
    if (sigOut != 0) {
        return sigOut;
    }

    sigOut = setSegv(enableSegv);
    if (sigOut != 0) {
        return sigOut;
    }

    sigOut = setAbrt(enableAbrt);
    if (sigOut != 0) {
        return sigOut;
    }

    if (useMockGmm) {
        GmmHelper::createGmmContextWrapperFunc = GmmClientContext::create<MockGmmClientContext>;
    } else {
        GmmInterface::initialize(nullptr, nullptr);
    }
    initializeTestHelpers(testMode);

    retVal = RUN_ALL_TESTS();

    if (showTestStats) {
        std::cout << getTestStats() << std::endl;
    }

    if (dumpTestStats) {
        std::ofstream dumpTestStatsFile;
        dumpTestStatsFile.open(dumpTestStatsFileName);
        dumpTestStatsFile << getTestStatsJson();
        dumpTestStatsFile.close();
    }

    cleanTestHelpers();
    return retVal;
}
