/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/compiler_interface/compiler_interface.h"

#include "shared/source/built_ins/sip_kernel_type.h"
#include "shared/source/compiler_interface/compiler_cache.h"
#include "shared/source/compiler_interface/compiler_interface.inl"
#include "shared/source/compiler_interface/compiler_options.h"
#include "shared/source/compiler_interface/igc_platform_helper.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/device/device.h"
#include "shared/source/helpers/compiler_product_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/os_interface/os_inc_base.h"

#include "cif/common/cif_main.h"
#include "cif/helpers/error.h"
#include "cif/import/library_api.h"
#include "ocl_igc_interface/code_type.h"
#include "ocl_igc_interface/fcl_ocl_device_ctx.h"
#include "ocl_igc_interface/igc_ocl_device_ctx.h"
#include "ocl_igc_interface/platform_helper.h"

#include <fstream>

namespace NEO {
SpinLock CompilerInterface::spinlock;

enum CachingMode {
    None,
    Direct,
    PreProcess
};

void TranslationOutput::makeCopy(MemAndSize &dst, CIF::Builtins::BufferSimple *src) {
    if ((nullptr == src) || (src->GetSizeRaw() == 0)) {
        dst.mem.reset();
        dst.size = 0U;
        return;
    }

    dst.size = src->GetSize<char>();
    dst.mem = ::makeCopy(src->GetMemory<void>(), src->GetSize<char>());
}

CompilerInterface::CompilerInterface()
    : cache() {
}
CompilerInterface::~CompilerInterface() = default;

TranslationOutput::ErrorCode CompilerInterface::build(
    const NEO::Device &device,
    const TranslationInput &input,
    TranslationOutput &output) {
    if (false == isCompilerAvailable(input.srcType, input.outType)) {
        return TranslationOutput::ErrorCode::CompilerNotAvailable;
    }

    IGC::CodeType::CodeType_t srcCodeType = input.srcType;
    IGC::CodeType::CodeType_t intermediateCodeType = IGC::CodeType::undefined;

    if (input.preferredIntermediateType != IGC::CodeType::undefined) {
        intermediateCodeType = input.preferredIntermediateType;
    }

    CachingMode cachingMode = None;

    if (cache != nullptr && cache->getConfig().enabled) {
        if ((srcCodeType == IGC::CodeType::oclC) && (std::strstr(input.src.begin(), "#include") == nullptr)) {
            cachingMode = CachingMode::Direct;
        } else {
            cachingMode = CachingMode::PreProcess;
        }
    }

    std::string kernelFileHash;
    if (cachingMode == CachingMode::Direct) {
        kernelFileHash = cache->getCachedFileName(device.getHardwareInfo(),
                                                  input.src,
                                                  input.apiOptions,
                                                  input.internalOptions);
        output.deviceBinary.mem = cache->loadCachedBinary(kernelFileHash, output.deviceBinary.size);
        if (output.deviceBinary.mem) {
            return TranslationOutput::ErrorCode::Success;
        }
    }

    auto inSrc = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.src.begin(), input.src.size());
    auto fclOptions = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.apiOptions.begin(), input.apiOptions.size());
    auto fclInternalOptions = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.internalOptions.begin(), input.internalOptions.size());

    auto idsBuffer = CIF::Builtins::CreateConstBuffer(igcMain.get(), nullptr, 0);
    auto valuesBuffer = CIF::Builtins::CreateConstBuffer(igcMain.get(), nullptr, 0);
    for (const auto &specConst : input.specializedValues) {
        idsBuffer->PushBackRawCopy(specConst.first);
        valuesBuffer->PushBackRawCopy(specConst.second);
    }

    CIF::RAII::UPtr_t<CIF::Builtins::BufferSimple> intermediateRepresentation;

    if (srcCodeType == IGC::CodeType::oclC) {
        if (intermediateCodeType == IGC::CodeType::undefined) {
            intermediateCodeType = getPreferredIntermediateRepresentation(device);
        }

        auto fclTranslationCtx = createFclTranslationCtx(device, srcCodeType, intermediateCodeType);
        auto fclOutput = translate(fclTranslationCtx.get(), inSrc.get(),
                                   fclOptions.get(), fclInternalOptions.get());

        if (fclOutput == nullptr) {
            return TranslationOutput::ErrorCode::UnknownError;
        }

        TranslationOutput::makeCopy(output.frontendCompilerLog, fclOutput->GetBuildLog());

        if (fclOutput->Successful() == false) {
            return TranslationOutput::ErrorCode::BuildFailure;
        }

        output.intermediateCodeType = intermediateCodeType;
        TranslationOutput::makeCopy(output.intermediateRepresentation, fclOutput->GetOutput());

        fclOutput->GetOutput()->Retain(); // will be used as input to compiler
        intermediateRepresentation.reset(fclOutput->GetOutput());
    } else {
        inSrc->Retain(); // will be used as input to compiler directly
        intermediateRepresentation.reset(inSrc.get());
        intermediateCodeType = srcCodeType;
    }

    if (cachingMode == CachingMode::PreProcess) {
        kernelFileHash = cache->getCachedFileName(device.getHardwareInfo(), ArrayRef<const char>(intermediateRepresentation->GetMemory<char>(), intermediateRepresentation->GetSize<char>()),
                                                  input.apiOptions,
                                                  input.internalOptions);
        output.deviceBinary.mem = cache->loadCachedBinary(kernelFileHash, output.deviceBinary.size);
        if (output.deviceBinary.mem) {
            return TranslationOutput::ErrorCode::Success;
        }
    }

    auto igcTranslationCtx = createIgcTranslationCtx(device, intermediateCodeType, IGC::CodeType::oclGenBin);

    auto igcOutput = translate(igcTranslationCtx.get(), intermediateRepresentation.get(), idsBuffer.get(), valuesBuffer.get(),
                               fclOptions.get(), fclInternalOptions.get(), input.gtPinInput);

    if (igcOutput == nullptr) {
        return TranslationOutput::ErrorCode::UnknownError;
    }

    TranslationOutput::makeCopy(output.backendCompilerLog, igcOutput->GetBuildLog());

    if (igcOutput->Successful() == false) {
        return TranslationOutput::ErrorCode::BuildFailure;
    }

    if (cache != nullptr && cache->getConfig().enabled) {
        cache->cacheBinary(kernelFileHash, igcOutput->GetOutput()->GetMemory<char>(), static_cast<uint32_t>(igcOutput->GetOutput()->GetSize<char>()));
    }

    TranslationOutput::makeCopy(output.deviceBinary, igcOutput->GetOutput());
    TranslationOutput::makeCopy(output.debugData, igcOutput->GetDebugData());

    return TranslationOutput::ErrorCode::Success;
}

TranslationOutput::ErrorCode CompilerInterface::compile(
    const NEO::Device &device,
    const TranslationInput &input,
    TranslationOutput &output) {

    if ((IGC::CodeType::oclC != input.srcType) && (IGC::CodeType::elf != input.srcType)) {
        return TranslationOutput::ErrorCode::AlreadyCompiled;
    }

    if (false == isCompilerAvailable(input.srcType, input.outType)) {
        return TranslationOutput::ErrorCode::CompilerNotAvailable;
    }

    auto outType = input.outType;

    if (outType == IGC::CodeType::undefined) {
        outType = getPreferredIntermediateRepresentation(device);
    }

    auto fclSrc = CIF::Builtins::CreateConstBuffer(fclMain.get(), input.src.begin(), input.src.size());
    auto fclOptions = CIF::Builtins::CreateConstBuffer(fclMain.get(), input.apiOptions.begin(), input.apiOptions.size());
    auto fclInternalOptions = CIF::Builtins::CreateConstBuffer(fclMain.get(), input.internalOptions.begin(), input.internalOptions.size());

    auto fclTranslationCtx = createFclTranslationCtx(device, input.srcType, outType);

    auto fclOutput = translate(fclTranslationCtx.get(), fclSrc.get(),
                               fclOptions.get(), fclInternalOptions.get());

    if (fclOutput == nullptr) {
        return TranslationOutput::ErrorCode::UnknownError;
    }

    TranslationOutput::makeCopy(output.frontendCompilerLog, fclOutput->GetBuildLog());

    if (fclOutput->Successful() == false) {
        return TranslationOutput::ErrorCode::CompilationFailure;
    }

    output.intermediateCodeType = outType;
    TranslationOutput::makeCopy(output.intermediateRepresentation, fclOutput->GetOutput());

    return TranslationOutput::ErrorCode::Success;
}

TranslationOutput::ErrorCode CompilerInterface::link(
    const NEO::Device &device,
    const TranslationInput &input,
    TranslationOutput &output) {
    if (false == isCompilerAvailable(input.srcType, input.outType)) {
        return TranslationOutput::ErrorCode::CompilerNotAvailable;
    }

    auto inSrc = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.src.begin(), input.src.size());
    auto igcOptions = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.apiOptions.begin(), input.apiOptions.size());
    auto igcInternalOptions = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.internalOptions.begin(), input.internalOptions.size());

    if (inSrc == nullptr) {
        return TranslationOutput::ErrorCode::UnknownError;
    }

    CIF::RAII::UPtr_t<IGC::OclTranslationOutputTagOCL> currOut;
    inSrc->Retain(); // shared with currSrc
    CIF::RAII::UPtr_t<CIF::Builtins::BufferSimple> currSrc(inSrc.get());
    IGC::CodeType::CodeType_t translationChain[] = {IGC::CodeType::elf, IGC::CodeType::oclGenBin};
    constexpr size_t numTranslations = sizeof(translationChain) / sizeof(translationChain[0]);
    for (size_t ti = 1; ti < numTranslations; ti++) {
        IGC::CodeType::CodeType_t inType = translationChain[ti - 1];
        IGC::CodeType::CodeType_t outType = translationChain[ti];

        auto igcTranslationCtx = createIgcTranslationCtx(device, inType, outType);
        currOut = translate(igcTranslationCtx.get(), currSrc.get(),
                            igcOptions.get(), igcInternalOptions.get(), input.gtPinInput);

        if (currOut == nullptr) {
            return TranslationOutput::ErrorCode::UnknownError;
        }

        if (currOut->Successful() == false) {
            TranslationOutput::makeCopy(output.backendCompilerLog, currOut->GetBuildLog());
            return TranslationOutput::ErrorCode::LinkFailure;
        }

        currOut->GetOutput()->Retain(); // shared with currSrc
        currSrc.reset(currOut->GetOutput());
    }

    TranslationOutput::makeCopy(output.backendCompilerLog, currOut->GetBuildLog());
    TranslationOutput::makeCopy(output.deviceBinary, currOut->GetOutput());
    TranslationOutput::makeCopy(output.debugData, currOut->GetDebugData());

    return TranslationOutput::ErrorCode::Success;
}

TranslationOutput::ErrorCode CompilerInterface::getSpecConstantsInfo(const NEO::Device &device, ArrayRef<const char> srcSpirV, SpecConstantInfo &output) {
    if (false == isIgcAvailable()) {
        return TranslationOutput::ErrorCode::CompilerNotAvailable;
    }

    auto igcTranslationCtx = createIgcTranslationCtx(device, IGC::CodeType::spirV, IGC::CodeType::oclGenBin);

    auto inSrc = CIF::Builtins::CreateConstBuffer(igcMain.get(), srcSpirV.begin(), srcSpirV.size());
    output.idsBuffer = CIF::Builtins::CreateConstBuffer(igcMain.get(), nullptr, 0);
    output.sizesBuffer = CIF::Builtins::CreateConstBuffer(igcMain.get(), nullptr, 0);

    auto retVal = getSpecConstantsInfoImpl(igcTranslationCtx.get(), inSrc.get(), output.idsBuffer.get(), output.sizesBuffer.get());

    if (!retVal) {
        return TranslationOutput::ErrorCode::UnknownError;
    }

    return TranslationOutput::ErrorCode::Success;
}

TranslationOutput::ErrorCode CompilerInterface::createLibrary(
    NEO::Device &device,
    const TranslationInput &input,
    TranslationOutput &output) {
    if (false == isIgcAvailable()) {
        return TranslationOutput::ErrorCode::CompilerNotAvailable;
    }

    auto igcSrc = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.src.begin(), input.src.size());
    auto igcOptions = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.apiOptions.begin(), input.apiOptions.size());
    auto igcInternalOptions = CIF::Builtins::CreateConstBuffer(igcMain.get(), input.internalOptions.begin(), input.internalOptions.size());

    auto intermediateRepresentation = IGC::CodeType::llvmBc;
    auto igcTranslationCtx = createIgcTranslationCtx(device, IGC::CodeType::elf, intermediateRepresentation);

    auto igcOutput = translate(igcTranslationCtx.get(), igcSrc.get(),
                               igcOptions.get(), igcInternalOptions.get());

    if (igcOutput == nullptr) {
        return TranslationOutput::ErrorCode::UnknownError;
    }

    TranslationOutput::makeCopy(output.backendCompilerLog, igcOutput->GetBuildLog());

    if (igcOutput->Successful() == false) {
        return TranslationOutput::ErrorCode::LinkFailure;
    }

    output.intermediateCodeType = intermediateRepresentation;
    TranslationOutput::makeCopy(output.intermediateRepresentation, igcOutput->GetOutput());

    return TranslationOutput::ErrorCode::Success;
}

TranslationOutput::ErrorCode CompilerInterface::getSipKernelBinary(NEO::Device &device, SipKernelType type, std::vector<char> &retBinary,
                                                                   std::vector<char> &stateSaveAreaHeader) {
    if (false == isIgcAvailable()) {
        return TranslationOutput::ErrorCode::CompilerNotAvailable;
    }

    bool bindlessSip = false;
    IGC::SystemRoutineType::SystemRoutineType_t typeOfSystemRoutine = IGC::SystemRoutineType::undefined;
    switch (type) {
    case SipKernelType::Csr:
        typeOfSystemRoutine = IGC::SystemRoutineType::contextSaveRestore;
        break;
    case SipKernelType::DbgCsr:
        typeOfSystemRoutine = IGC::SystemRoutineType::debug;
        break;
    case SipKernelType::DbgCsrLocal:
        typeOfSystemRoutine = IGC::SystemRoutineType::debugSlm;
        break;
    case SipKernelType::DbgBindless:
        typeOfSystemRoutine = IGC::SystemRoutineType::debug;
        bindlessSip = true;
        break;
    default:
        break;
    }

    auto deviceCtx = getIgcDeviceCtx(device);

    if (deviceCtx == nullptr) {
        return TranslationOutput::ErrorCode::UnknownError;
    }

    auto systemRoutineBuffer = igcMain->CreateBuiltin<CIF::Builtins::BufferLatest>();
    auto stateSaveAreaBuffer = igcMain->CreateBuiltin<CIF::Builtins::BufferLatest>();

    auto result = deviceCtx->GetSystemRoutine(typeOfSystemRoutine,
                                              bindlessSip,
                                              systemRoutineBuffer.get(),
                                              stateSaveAreaBuffer.get());

    if (!result) {
        return TranslationOutput::ErrorCode::UnknownError;
    }

    retBinary.assign(systemRoutineBuffer->GetMemory<char>(), systemRoutineBuffer->GetMemory<char>() + systemRoutineBuffer->GetSizeRaw());
    stateSaveAreaHeader.assign(stateSaveAreaBuffer->GetMemory<char>(), stateSaveAreaBuffer->GetMemory<char>() + stateSaveAreaBuffer->GetSizeRaw());

    return TranslationOutput::ErrorCode::Success;
}

CIF::RAII::UPtr_t<IGC::IgcFeaturesAndWorkaroundsTagOCL> CompilerInterface::getIgcFeaturesAndWorkarounds(NEO::Device const &device) {
    return getIgcDeviceCtx(device)->GetIgcFeaturesAndWorkaroundsHandle();
}

bool CompilerInterface::loadFcl() {
    return NEO::loadCompiler(Os::frontEndDllName, fclLib, fclMain);
}

bool CompilerInterface::loadIgc() {
    return NEO::loadCompiler(Os::igcDllName, igcLib, igcMain);
}

bool CompilerInterface::initialize(std::unique_ptr<CompilerCache> &&cache, bool requireFcl) {
    bool fclAvailable = requireFcl ? this->loadFcl() : false;
    bool igcAvailable = this->loadIgc();
    bool compilerVersionCorrect = true;
    if (!DebugManager.flags.ZebinIgnoreIcbeVersion.get()) {
        compilerVersionCorrect = verifyIcbeVersion();
    }

    this->cache.swap(cache);

    return this->cache && igcAvailable && (fclAvailable || (false == requireFcl)) && compilerVersionCorrect;
}

IGC::FclOclDeviceCtxTagOCL *CompilerInterface::getFclDeviceCtx(const Device &device) {
    auto ulock = this->lock();
    auto it = fclDeviceContexts.find(&device);
    if (it != fclDeviceContexts.end()) {
        return it->second.get();
    }

    if (fclMain == nullptr) {
        DEBUG_BREAK_IF(true); // compiler not available
        return nullptr;
    }

    auto newDeviceCtx = fclMain->CreateInterface<IGC::FclOclDeviceCtxTagOCL>();
    if (newDeviceCtx == nullptr) {
        DEBUG_BREAK_IF(true); // could not create device context
        return nullptr;
    }
    newDeviceCtx->SetOclApiVersion(device.getHardwareInfo().capabilityTable.clVersionSupport * 10);
    if (newDeviceCtx->GetUnderlyingVersion() > 4U) {
        auto igcPlatform = newDeviceCtx->GetPlatformHandle();
        if (nullptr == igcPlatform.get()) {
            DEBUG_BREAK_IF(true); // could not acquire handles to platform descriptor
            return nullptr;
        }
        const auto &hwInfo = device.getHardwareInfo();
        populateIgcPlatform(*igcPlatform, hwInfo);
    }
    fclDeviceContexts[&device] = std::move(newDeviceCtx);

    return fclDeviceContexts[&device].get();
}

IGC::IgcOclDeviceCtxTagOCL *CompilerInterface::getIgcDeviceCtx(const Device &device) {
    auto ulock = this->lock();
    auto it = igcDeviceContexts.find(&device);
    if (it != igcDeviceContexts.end()) {
        return it->second.get();
    }

    if (igcMain == nullptr) {
        DEBUG_BREAK_IF(true); // compiler not available
        return nullptr;
    }

    auto newDeviceCtx = igcMain->CreateInterface<IGC::IgcOclDeviceCtxTagOCL>();
    if (newDeviceCtx == nullptr) {
        DEBUG_BREAK_IF(true); // could not create device context
        return nullptr;
    }

    newDeviceCtx->SetProfilingTimerResolution(static_cast<float>(device.getDeviceInfo().outProfilingTimerResolution));
    auto igcPlatform = newDeviceCtx->GetPlatformHandle();
    auto igcGtSystemInfo = newDeviceCtx->GetGTSystemInfoHandle();
    auto igcFtrWa = newDeviceCtx->GetIgcFeaturesAndWorkaroundsHandle();
    if (false == NEO::areNotNullptr(igcPlatform.get(), igcGtSystemInfo.get(), igcFtrWa.get())) {
        DEBUG_BREAK_IF(true); // could not acquire handles to device descriptors
        return nullptr;
    }
    const HardwareInfo *hwInfo = &device.getHardwareInfo();
    auto productFamily = DebugManager.flags.ForceCompilerUsePlatform.get();
    if (productFamily != "unk") {
        getHwInfoForPlatformString(productFamily, hwInfo);
    }

    populateIgcPlatform(*igcPlatform, *hwInfo);
    IGC::GtSysInfoHelper::PopulateInterfaceWith(*igcGtSystemInfo, hwInfo->gtSystemInfo);

    auto &compilerProductHelper = device.getCompilerProductHelper();
    igcFtrWa->SetFtrGpGpuMidThreadLevelPreempt(compilerProductHelper.isMidThreadPreemptionSupported(*hwInfo));
    igcFtrWa->SetFtrWddm2Svm(device.getHardwareInfo().featureTable.flags.ftrWddm2Svm);
    igcFtrWa->SetFtrPooledEuEnabled(device.getHardwareInfo().featureTable.flags.ftrPooledEuEnabled);

    igcDeviceContexts[&device] = std::move(newDeviceCtx);
    return igcDeviceContexts[&device].get();
}

IGC::CodeType::CodeType_t CompilerInterface::getPreferredIntermediateRepresentation(const Device &device) {
    return getFclDeviceCtx(device)->GetPreferredIntermediateRepresentation();
}

CIF::RAII::UPtr_t<IGC::FclOclTranslationCtxTagOCL> CompilerInterface::createFclTranslationCtx(const Device &device, IGC::CodeType::CodeType_t inType, IGC::CodeType::CodeType_t outType) {
    auto deviceCtx = getFclDeviceCtx(device);
    if (deviceCtx == nullptr) {
        DEBUG_BREAK_IF(true); // could not create device context
        return nullptr;
    }

    if (fclBaseTranslationCtx == nullptr) {
        auto ulock = this->lock();
        if (fclBaseTranslationCtx == nullptr) {
            fclBaseTranslationCtx = deviceCtx->CreateTranslationCtx(inType, outType);
        }
    }

    return deviceCtx->CreateTranslationCtx(inType, outType);
}

CIF::RAII::UPtr_t<IGC::IgcOclTranslationCtxTagOCL> CompilerInterface::createIgcTranslationCtx(const Device &device, IGC::CodeType::CodeType_t inType, IGC::CodeType::CodeType_t outType) {
    auto deviceCtx = getIgcDeviceCtx(device);
    if (deviceCtx == nullptr) {
        DEBUG_BREAK_IF(true); // could not create device context
        return nullptr;
    }

    return deviceCtx->CreateTranslationCtx(inType, outType);
}

template <template <CIF::Version_t> class EntryPointT>
void checkIcbeVersion(CIF::CIFMain *main, const char *libName, bool &ret) {
    if (false == main->IsCompatible<EntryPointT>()) {
        NEO::printDebugString(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Installed Compiler Library %s is incompatible\n", libName);
        DEBUG_BREAK_IF(true); // given compiler library is not compatible
        ret = false;
        return;
    }
    ret = true;
}

template <>
std::once_flag &CompilerInterface::getIcbeVersionCallOnceFlag<IGC::IgcOclDeviceCtx>() {
    return igcIcbeCheckVersionCallOnce;
}

template <>
std::once_flag &CompilerInterface::getIcbeVersionCallOnceFlag<IGC::FclOclDeviceCtx>() {
    return fclIcbeCheckVersionCallOnce;
}

template <template <CIF::Version_t> class EntryPointT>
bool CompilerInterface::checkIcbeVersionOnce(CIF::CIFMain *main, const char *libName) {
    bool ret = true;
    std::call_once(getIcbeVersionCallOnceFlag<EntryPointT>(), checkIcbeVersion<EntryPointT>, main, libName, ret);
    return ret;
}

bool CompilerInterface::verifyIcbeVersion() {
    bool versionIsCorrect = true;
    if (isFclAvailable()) {
        versionIsCorrect = checkIcbeVersionOnce<IGC::FclOclDeviceCtx>(fclMain.get(), Os::frontEndDllName);
    }
    if (isIgcAvailable()) {
        versionIsCorrect &= checkIcbeVersionOnce<IGC::IgcOclDeviceCtx>(igcMain.get(), Os::igcDllName);
    }
    return versionIsCorrect;
}

bool CompilerInterface::addOptionDisableZebin(std::string &options, std::string &internalOptions) {
    CompilerOptions::concatenateAppend(internalOptions, CompilerOptions::disableZebin);
    auto pos = options.find(NEO::CompilerOptions::enableZebin.str());
    if (pos != std::string::npos || !verifyIcbeVersion()) {
        return false;
    }
    return true;
}

bool CompilerInterface::disableZebin(std::string &options, std::string &internalOptions) {
    auto pos = options.find(NEO::CompilerOptions::enableZebin.str());
    if (pos != std::string::npos) {
        options.erase(pos, pos + CompilerOptions::enableZebin.length());
    }
    return addOptionDisableZebin(options, internalOptions);
}

template bool CompilerInterface::checkIcbeVersionOnce<IGC::FclOclDeviceCtx>(CIF::CIFMain *main, const char *libName);
template bool CompilerInterface::checkIcbeVersionOnce<IGC::IgcOclDeviceCtx>(CIF::CIFMain *main, const char *libName);
} // namespace NEO