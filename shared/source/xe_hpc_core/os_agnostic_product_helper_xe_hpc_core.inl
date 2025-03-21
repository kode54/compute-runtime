/*
 * Copyright (C) 2022-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

namespace NEO {
template <>
std::string ProductHelperHw<gfxProduct>::getDeviceMemoryName() const {
    return "HBM";
}

template <>
bool ProductHelperHw<gfxProduct>::isDirectSubmissionSupported(const HardwareInfo &hwInfo) const {
    return true;
}

template <>
bool ProductHelperHw<gfxProduct>::isDcFlushAllowed() const {
    return false;
}

template <>
bool ProductHelperHw<gfxProduct>::isTimestampWaitSupportedForEvents() const {
    return true;
}

template <>
std::pair<bool, bool> ProductHelperHw<gfxProduct>::isPipeControlPriorToNonPipelinedStateCommandsWARequired(const HardwareInfo &hwInfo, bool isRcs, const ReleaseHelper *releaseHelper) const {
    auto isBasicWARequired = false;
    auto isExtendedWARequired = false;

    if (DebugManager.flags.ProgramExtendedPipeControlPriorToNonPipelinedStateCommand.get() != -1) {
        isExtendedWARequired = DebugManager.flags.ProgramExtendedPipeControlPriorToNonPipelinedStateCommand.get();
    }

    return {isBasicWARequired, isExtendedWARequired};
}

template <>
void ProductHelperHw<gfxProduct>::adjustSamplerState(void *sampler, const HardwareInfo &hwInfo) const {
    using SAMPLER_STATE = typename XeHpcCoreFamily::SAMPLER_STATE;

    auto samplerState = reinterpret_cast<SAMPLER_STATE *>(sampler);
    if (DebugManager.flags.ForceSamplerLowFilteringPrecision.get()) {
        samplerState->setLowQualityFilter(SAMPLER_STATE::LOW_QUALITY_FILTER_ENABLE);
    }
}

template <>
bool ProductHelperHw<gfxProduct>::isPrefetcherDisablingInDirectSubmissionRequired() const {
    return false;
}

template <>
bool ProductHelperHw<gfxProduct>::isLinearStoragePreferred(bool isSharedContext, bool isImage1d, bool forceLinearStorage) const {
    return true;
}

template <>
uint32_t ProductHelperHw<gfxProduct>::getMaxNumSamplers() const {
    return 0u;
}

} // namespace NEO
