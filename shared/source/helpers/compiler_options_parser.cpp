/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/helpers/compiler_options_parser.h"

#include "shared/source/compiler_interface/compiler_options.h"
#include "shared/source/compiler_interface/oclc_extensions.h"
#include "shared/source/helpers/compiler_product_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/release_helper/release_helper.h"

#include <cstdint>
#include <sstream>

namespace NEO {

const std::string clStdOptionName = "-cl-std=CL";

uint32_t getMajorVersion(const std::string &compileOptions) {
    auto clStdValuePosition = compileOptions.find(clStdOptionName);
    if (clStdValuePosition == std::string::npos) {
        return 0;
    }
    std::stringstream ss{compileOptions.c_str() + clStdValuePosition + clStdOptionName.size()};
    uint32_t majorVersion;
    ss >> majorVersion;
    return majorVersion;
}

bool requiresOpenClCFeatures(const std::string &compileOptions) {
    return (getMajorVersion(compileOptions) >= 3);
}

bool requiresAdditionalExtensions(const std::string &compileOptions) {
    return (getMajorVersion(compileOptions) == 2);
}
void appendExtensionsToInternalOptions(const HardwareInfo &hwInfo, const std::string &options, std::string &internalOptions) {
    auto compilerProductHelper = CompilerProductHelper::create(hwInfo.platform.eProductFamily);
    auto releaseHelper = ReleaseHelper::create(hwInfo.ipVersion);
    std::string extensionsList = compilerProductHelper->getDeviceExtensions(hwInfo, releaseHelper.get());

    if (requiresAdditionalExtensions(options)) {
        extensionsList += "cl_khr_3d_image_writes ";
    }
    OpenClCFeaturesContainer openclCFeatures;
    if (requiresOpenClCFeatures(options)) {
        getOpenclCFeaturesList(hwInfo, openclCFeatures, *compilerProductHelper.get());
    }

    auto compilerExtensions = convertEnabledExtensionsToCompilerInternalOptions(extensionsList.c_str(), openclCFeatures);
    auto oclVersion = getOclVersionCompilerInternalOption(hwInfo.capabilityTable.clVersionSupport);
    internalOptions = CompilerOptions::concatenate(oclVersion, compilerExtensions, internalOptions);
    if (hwInfo.capabilityTable.supportsImages) {
        CompilerOptions::concatenateAppend(internalOptions, CompilerOptions::enableImageSupport);
    }
}

} // namespace NEO
