/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "level_zero/core/source/module/module_imp.h"

#include "shared/source/compiler_interface/compiler_options.h"
#include "shared/source/compiler_interface/compiler_warnings/compiler_warnings.h"
#include "shared/source/compiler_interface/external_functions.h"
#include "shared/source/compiler_interface/intermediate_representations.h"
#include "shared/source/compiler_interface/linker.h"
#include "shared/source/debugger/debugger_l0.h"
#include "shared/source/device/device.h"
#include "shared/source/device_binary_format/device_binary_formats.h"
#include "shared/source/device_binary_format/elf/elf.h"
#include "shared/source/device_binary_format/elf/elf_encoder.h"
#include "shared/source/device_binary_format/elf/ocl_elf.h"
#include "shared/source/device_binary_format/zebin/debug_zebin.h"
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/addressing_mode_helper.h"
#include "shared/source/helpers/aligned_memory.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/helpers/compiler_product_helper.h"
#include "shared/source/helpers/constants.h"
#include "shared/source/helpers/file_io.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/helpers/kernel_helpers.h"
#include "shared/source/helpers/string.h"
#include "shared/source/kernel/kernel_descriptor.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/memory_operations_handler.h"
#include "shared/source/memory_manager/unified_memory_manager.h"
#include "shared/source/program/kernel_info.h"
#include "shared/source/program/program_initialization.h"
#include "shared/source/source_level_debugger/source_level_debugger.h"

#include "level_zero/core/source/device/device.h"
#include "level_zero/core/source/driver/driver_handle.h"
#include "level_zero/core/source/gfx_core_helpers/l0_gfx_core_helper.h"
#include "level_zero/core/source/kernel/kernel.h"
#include "level_zero/core/source/module/module_build_log.h"

#include "program_debug_data.h"

#include <list>
#include <memory>
#include <unordered_map>
namespace L0 {

namespace BuildOptions {
NEO::ConstStringRef optDisable = "-ze-opt-disable";
NEO::ConstStringRef optLevel = "-ze-opt-level";
NEO::ConstStringRef greaterThan4GbRequired = "-ze-opt-greater-than-4GB-buffer-required";
NEO::ConstStringRef hasBufferOffsetArg = "-ze-intel-has-buffer-offset-arg";
NEO::ConstStringRef debugKernelEnable = "-ze-kernel-debug-enable";
NEO::ConstStringRef profileFlags = "-zet-profile-flags";
NEO::ConstStringRef optLargeRegisterFile = "-ze-opt-large-register-file";
NEO::ConstStringRef optAutoGrf = "-ze-intel-enable-auto-large-GRF-mode";
NEO::ConstStringRef enableLibraryCompile = "-library-compilation";
NEO::ConstStringRef enableGlobalVariableSymbols = "-ze-take-global-address";
NEO::ConstStringRef enableFP64GenEmu = "-ze-fp64-gen-emu";
} // namespace BuildOptions

ModuleTranslationUnit::ModuleTranslationUnit(L0::Device *device)
    : device(device) {
}

ModuleTranslationUnit::~ModuleTranslationUnit() {
    if (globalConstBuffer) {
        auto svmAllocsManager = device->getDriverHandle()->getSvmAllocsManager();

        if (svmAllocsManager->getSVMAlloc(reinterpret_cast<void *>(globalConstBuffer->getGpuAddress()))) {
            svmAllocsManager->freeSVMAlloc(reinterpret_cast<void *>(globalConstBuffer->getGpuAddress()));
        } else {
            this->device->getNEODevice()->getExecutionEnvironment()->memoryManager->checkGpuUsageAndDestroyGraphicsAllocations(globalConstBuffer);
        }
    }

    if (globalVarBuffer) {
        auto svmAllocsManager = device->getDriverHandle()->getSvmAllocsManager();

        if (svmAllocsManager->getSVMAlloc(reinterpret_cast<void *>(globalVarBuffer->getGpuAddress()))) {
            svmAllocsManager->freeSVMAlloc(reinterpret_cast<void *>(globalVarBuffer->getGpuAddress()));
        } else {
            this->device->getNEODevice()->getExecutionEnvironment()->memoryManager->checkGpuUsageAndDestroyGraphicsAllocations(globalVarBuffer);
        }
    }

    if (this->debugData != nullptr) {
        for (std::vector<char *>::iterator iter = alignedvIsas.begin(); iter != alignedvIsas.end(); ++iter) {
            alignedFree(static_cast<void *>(*iter));
        }
    }
}

std::vector<uint8_t> ModuleTranslationUnit::generateElfFromSpirV(std::vector<const char *> inputSpirVs, std::vector<uint32_t> inputModuleSizes) {
    NEO::Elf::ElfEncoder<> elfEncoder(true, false, 1U);
    elfEncoder.getElfFileHeader().type = NEO::Elf::ET_OPENCL_OBJECTS;

    StackVec<uint32_t, 64> specConstIds;
    StackVec<uint64_t, 64> specConstValues;
    for (uint32_t i = 0; i < static_cast<uint32_t>(inputSpirVs.size()); i++) {
        if (specConstantsValues.size() > 0) {
            specConstIds.clear();
            specConstValues.clear();
            specConstIds.reserve(specConstantsValues.size());
            specConstValues.reserve(specConstantsValues.size());
            for (const auto &specConst : specConstantsValues) {
                specConstIds.push_back(specConst.first);
                specConstValues.push_back(specConst.second);
            }
            elfEncoder.appendSection(NEO::Elf::SHT_OPENCL_SPIRV_SC_IDS, NEO::Elf::SectionNamesOpenCl::spirvSpecConstIds,
                                     ArrayRef<const uint8_t>::fromAny(specConstIds.begin(), specConstIds.size()));
            elfEncoder.appendSection(NEO::Elf::SHT_OPENCL_SPIRV_SC_VALUES, NEO::Elf::SectionNamesOpenCl::spirvSpecConstValues,
                                     ArrayRef<const uint8_t>::fromAny(specConstValues.begin(), specConstValues.size()));
        }

        auto sectionType = NEO::Elf::SHT_OPENCL_SPIRV;
        NEO::ConstStringRef sectionName = NEO::Elf::SectionNamesOpenCl::spirvObject;
        elfEncoder.appendSection(sectionType, sectionName, ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(inputSpirVs[i]), inputModuleSizes[i]));
    }

    return elfEncoder.encode();
}

std::string ModuleTranslationUnit::generateCompilerOptions(const char *buildOptions, const char *internalBuildOptions) {
    if (nullptr != buildOptions) {
        options = buildOptions;
    }
    std::string internalOptions = NEO::CompilerOptions::concatenate(internalBuildOptions, BuildOptions::hasBufferOffsetArg);
    auto &neoDevice = *device->getNEODevice();
    auto isDebuggerActive = neoDevice.getDeviceInfo().debuggerActive;
    if (isDebuggerActive) {
        if (NEO::SourceLevelDebugger::shouldAppendOptDisable(*device->getSourceLevelDebugger())) {
            NEO::CompilerOptions::concatenateAppend(options, BuildOptions::optDisable);
        }

        options = NEO::CompilerOptions::concatenate(options, NEO::CompilerOptions::generateDebugInfo);
        internalOptions = NEO::CompilerOptions::concatenate(internalOptions, BuildOptions::debugKernelEnable);
    }

    if (neoDevice.getExecutionEnvironment()->isFP64EmulationEnabled()) {
        internalOptions = NEO::CompilerOptions::concatenate(internalOptions, BuildOptions::enableFP64GenEmu);
    }

    const auto &compilerProductHelper = neoDevice.getRootDeviceEnvironment().getHelper<NEO::CompilerProductHelper>();
    auto forceToStatelessRequired = compilerProductHelper.isForceToStatelessRequired();
    auto statelessToStatefulOptimizationDisabled = NEO::DebugManager.flags.DisableStatelessToStatefulOptimization.get();

    if (forceToStatelessRequired || statelessToStatefulOptimizationDisabled) {
        internalOptions = NEO::CompilerOptions::concatenate(internalOptions, NEO::CompilerOptions::greaterThan4gbBuffersRequired);
    }
    isDebuggerActive |= neoDevice.getDebugger() != nullptr;
    NEO::CompilerOptions::concatenateAppend(internalOptions, compilerProductHelper.getCachingPolicyOptions(isDebuggerActive));
    return internalOptions;
}

bool ModuleTranslationUnit::processSpecConstantInfo(NEO::CompilerInterface *compilerInterface, const ze_module_constants_t *pConstants, const char *input, uint32_t inputSize) {
    if (pConstants) {
        NEO::SpecConstantInfo specConstInfo;
        auto retVal = compilerInterface->getSpecConstantsInfo(*device->getNEODevice(), ArrayRef<const char>(input, inputSize), specConstInfo);
        if (retVal != NEO::TranslationOutput::ErrorCode::Success) {
            return false;
        }
        for (uint32_t i = 0; i < pConstants->numConstants; i++) {
            uint64_t specConstantValue = 0;
            uint32_t specConstantId = pConstants->pConstantIds[i];
            auto atributeSize = 0u;
            uint32_t j;
            for (j = 0; j < specConstInfo.sizesBuffer->GetSize<uint32_t>(); j++) {
                if (specConstantId == specConstInfo.idsBuffer->GetMemory<uint32_t>()[j]) {
                    atributeSize = specConstInfo.sizesBuffer->GetMemory<uint32_t>()[j];
                    break;
                }
            }
            if (j == specConstInfo.sizesBuffer->GetSize<uint32_t>()) {
                return false;
            }
            memcpy_s(&specConstantValue, sizeof(uint64_t),
                     const_cast<void *>(pConstants->pConstantValues[i]), atributeSize);
            specConstantsValues[specConstantId] = specConstantValue;
        }
    }
    return true;
}

ze_result_t ModuleTranslationUnit::compileGenBinary(NEO::TranslationInput inputArgs, bool staticLink) {
    auto compilerInterface = device->getNEODevice()->getCompilerInterface();
    if (!compilerInterface) {
        return ZE_RESULT_ERROR_DEPENDENCY_UNAVAILABLE;
    }

    inputArgs.specializedValues = this->specConstantsValues;

    NEO::TranslationOutput compilerOuput = {};
    NEO::TranslationOutput::ErrorCode compilerErr;

    if (staticLink) {
        compilerErr = compilerInterface->link(*device->getNEODevice(), inputArgs, compilerOuput);
    } else {
        compilerErr = compilerInterface->build(*device->getNEODevice(), inputArgs, compilerOuput);
    }

    this->updateBuildLog(compilerOuput.frontendCompilerLog);
    this->updateBuildLog(compilerOuput.backendCompilerLog);

    if (NEO::TranslationOutput::ErrorCode::Success != compilerErr) {
        return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    }

    this->irBinary = std::move(compilerOuput.intermediateRepresentation.mem);
    this->irBinarySize = compilerOuput.intermediateRepresentation.size;
    this->unpackedDeviceBinary = std::move(compilerOuput.deviceBinary.mem);
    this->unpackedDeviceBinarySize = compilerOuput.deviceBinary.size;
    this->debugData = std::move(compilerOuput.debugData.mem);
    this->debugDataSize = compilerOuput.debugData.size;

    return processUnpackedBinary();
}

ze_result_t ModuleTranslationUnit::staticLinkSpirV(std::vector<const char *> inputSpirVs, std::vector<uint32_t> inputModuleSizes, const char *buildOptions, const char *internalBuildOptions,
                                                   std::vector<const ze_module_constants_t *> specConstants) {
    auto compilerInterface = device->getNEODevice()->getCompilerInterface();
    if (!compilerInterface) {
        return ZE_RESULT_ERROR_DEPENDENCY_UNAVAILABLE;
    }

    std::string internalOptions = this->generateCompilerOptions(buildOptions, internalBuildOptions);

    for (uint32_t i = 0; i < static_cast<uint32_t>(specConstants.size()); i++) {
        auto specConstantResult = this->processSpecConstantInfo(compilerInterface, specConstants[i], inputSpirVs[i], inputModuleSizes[i]);
        if (!specConstantResult) {
            return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
        }
    }

    NEO::TranslationInput linkInputArgs = {IGC::CodeType::elf, IGC::CodeType::oclGenBin};

    auto spirvElfSource = generateElfFromSpirV(inputSpirVs, inputModuleSizes);

    linkInputArgs.src = ArrayRef<const char>(reinterpret_cast<const char *>(spirvElfSource.data()), spirvElfSource.size());
    linkInputArgs.apiOptions = ArrayRef<const char>(options.c_str(), options.length());
    linkInputArgs.internalOptions = ArrayRef<const char>(internalOptions.c_str(), internalOptions.length());
    return this->compileGenBinary(linkInputArgs, true);
}

ze_result_t ModuleTranslationUnit::buildFromSpirV(const char *input, uint32_t inputSize, const char *buildOptions, const char *internalBuildOptions,
                                                  const ze_module_constants_t *pConstants) {
    const auto &neoDevice = device->getNEODevice();
    auto compilerInterface = neoDevice->getCompilerInterface();
    if (!compilerInterface) {
        return ZE_RESULT_ERROR_DEPENDENCY_UNAVAILABLE;
    }

    auto specConstantResult = this->processSpecConstantInfo(compilerInterface, pConstants, input, inputSize);
    if (!specConstantResult) {
        return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    }

    auto &l0GfxCoreHelper = neoDevice->getRootDeviceEnvironment().getHelper<L0GfxCoreHelper>();
    std::string internalOptions = this->generateCompilerOptions(buildOptions, internalBuildOptions);
    auto isZebinAllowed = l0GfxCoreHelper.isZebinAllowed(neoDevice->getDebugger());
    if (isZebinAllowed == false) {
        const auto &rootDevice = neoDevice->getRootDevice();
        if (!rootDevice->getCompilerInterface()->addOptionDisableZebin(this->options, internalOptions)) {
            updateBuildLog("Cannot build zebinary for this device with debugger enabled. Remove \"-ze-intel-enable-zebin\" build flag");
            return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
        }
    }

    NEO::TranslationInput inputArgs = {IGC::CodeType::spirV, IGC::CodeType::oclGenBin};

    inputArgs.src = ArrayRef<const char>(input, inputSize);
    inputArgs.apiOptions = ArrayRef<const char>(this->options.c_str(), this->options.length());
    inputArgs.internalOptions = ArrayRef<const char>(internalOptions.c_str(), internalOptions.length());
    return this->compileGenBinary(inputArgs, false);
}

ze_result_t ModuleTranslationUnit::createFromNativeBinary(const char *input, size_t inputSize) {
    UNRECOVERABLE_IF((nullptr == device) || (nullptr == device->getNEODevice()));
    auto productAbbreviation = NEO::hardwarePrefix[device->getNEODevice()->getHardwareInfo().platform.eProductFamily];

    NEO::TargetDevice targetDevice = NEO::getTargetDevice(device->getNEODevice()->getRootDeviceEnvironment());
    std::string decodeErrors;
    std::string decodeWarnings;
    ArrayRef<const uint8_t> archive(reinterpret_cast<const uint8_t *>(input), inputSize);
    auto singleDeviceBinary = unpackSingleDeviceBinary(archive, NEO::ConstStringRef(productAbbreviation, strlen(productAbbreviation)), targetDevice,
                                                       decodeErrors, decodeWarnings);
    if (decodeWarnings.empty() == false) {
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "%s\n", decodeWarnings.c_str());
    }
    if (singleDeviceBinary.intermediateRepresentation.empty() && singleDeviceBinary.deviceBinary.empty()) {
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "%s\n", decodeErrors.c_str());
        return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    } else {
        this->irBinary = makeCopy(reinterpret_cast<const char *>(singleDeviceBinary.intermediateRepresentation.begin()), singleDeviceBinary.intermediateRepresentation.size());
        this->irBinarySize = singleDeviceBinary.intermediateRepresentation.size();
        this->options = singleDeviceBinary.buildOptions.str();
        if (singleDeviceBinary.format == NEO::DeviceBinaryFormat::Zebin) {
            this->options += " " + NEO::CompilerOptions::enableZebin.str();
        }

        if (false == singleDeviceBinary.debugData.empty()) {
            this->debugData = makeCopy(reinterpret_cast<const char *>(singleDeviceBinary.debugData.begin()), singleDeviceBinary.debugData.size());
            this->debugDataSize = singleDeviceBinary.debugData.size();
        }

        bool rebuild = NEO::DebugManager.flags.RebuildPrecompiledKernels.get() && irBinarySize != 0;
        rebuild |= NEO::isRebuiltToPatchtokensRequired(device->getNEODevice(), archive, this->options, this->isBuiltIn, false);
        if (rebuild && irBinarySize == 0) {
            return ZE_RESULT_ERROR_INVALID_NATIVE_BINARY;
        }
        if ((false == singleDeviceBinary.deviceBinary.empty()) && (false == rebuild)) {
            this->unpackedDeviceBinary = makeCopy<char>(reinterpret_cast<const char *>(singleDeviceBinary.deviceBinary.begin()), singleDeviceBinary.deviceBinary.size());
            this->unpackedDeviceBinarySize = singleDeviceBinary.deviceBinary.size();
            // If the Native Binary was an Archive, then packedTargetDeviceBinary will be the packed Binary for the Target Device.
            if (singleDeviceBinary.packedTargetDeviceBinary.size() > 0) {
                this->packedDeviceBinary = makeCopy<char>(reinterpret_cast<const char *>(singleDeviceBinary.packedTargetDeviceBinary.begin()), singleDeviceBinary.packedTargetDeviceBinary.size());
                this->packedDeviceBinarySize = singleDeviceBinary.packedTargetDeviceBinary.size();
            } else {
                this->packedDeviceBinary = makeCopy<char>(reinterpret_cast<const char *>(archive.begin()), archive.size());
                this->packedDeviceBinarySize = archive.size();
            }
        }
    }

    if (nullptr == this->unpackedDeviceBinary) {
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "%s\n", NEO::CompilerWarnings::recompiledFromIr.data());
        if (!shouldSuppressRebuildWarning) {
            updateBuildLog(NEO::CompilerWarnings::recompiledFromIr.str());
        }

        return buildFromSpirV(this->irBinary.get(), static_cast<uint32_t>(this->irBinarySize), this->options.c_str(), "", nullptr);
    } else {
        if (processUnpackedBinary() != ZE_RESULT_SUCCESS) {
            return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
        }
        return ZE_RESULT_SUCCESS;
    }
}

ze_result_t ModuleTranslationUnit::processUnpackedBinary() {
    if (0 == unpackedDeviceBinarySize) {
        return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    }
    auto blob = ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(this->unpackedDeviceBinary.get()), this->unpackedDeviceBinarySize);
    NEO::SingleDeviceBinary binary = {};
    binary.deviceBinary = blob;
    binary.targetDevice = NEO::getTargetDevice(device->getNEODevice()->getRootDeviceEnvironment());
    std::string decodeErrors;
    std::string decodeWarnings;

    NEO::DecodeError decodeError;
    NEO::DeviceBinaryFormat singleDeviceBinaryFormat;
    auto &gfxCoreHelper = device->getGfxCoreHelper();
    std::tie(decodeError, singleDeviceBinaryFormat) = NEO::decodeSingleDeviceBinary(programInfo, binary, decodeErrors, decodeWarnings, gfxCoreHelper);
    if (decodeWarnings.empty() == false) {
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "%s\n", decodeWarnings.c_str());
    }

    if (NEO::DecodeError::Success != decodeError) {
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "%s\n", decodeErrors.c_str());
        return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    }

    processDebugData();

    size_t slmNeeded = NEO::getMaxInlineSlmNeeded(programInfo);
    size_t slmAvailable = 0U;
    NEO::DeviceInfoKernelPayloadConstants deviceInfoConstants;
    slmAvailable = static_cast<size_t>(device->getDeviceInfo().localMemSize);
    deviceInfoConstants.computeUnitsUsedForScratch = static_cast<uint32_t>(device->getDeviceInfo().computeUnitsUsedForScratch);
    deviceInfoConstants.slmWindowSize = static_cast<uint32_t>(device->getDeviceInfo().localMemSize);
    if (NEO::requiresLocalMemoryWindowVA(programInfo)) {
        deviceInfoConstants.slmWindow = device->getNEODevice()->getExecutionEnvironment()->memoryManager->getReservedMemory(MemoryConstants::slmWindowSize, MemoryConstants::slmWindowAlignment);
    }

    if (slmNeeded > slmAvailable) {
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Size of SLM (%u) larger than available (%u)\n",
                           static_cast<uint32_t>(slmNeeded), static_cast<uint32_t>(slmAvailable));
        return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    }

    auto svmAllocsManager = device->getDriverHandle()->getSvmAllocsManager();
    auto globalConstDataSize = programInfo.globalConstants.size + programInfo.globalConstants.zeroInitSize;
    if (globalConstDataSize != 0) {
        this->globalConstBuffer = NEO::allocateGlobalsSurface(svmAllocsManager, *device->getNEODevice(), globalConstDataSize,
                                                              programInfo.globalConstants.zeroInitSize, true, programInfo.linkerInput.get(), programInfo.globalConstants.initData);
    }

    auto globalVariablesDataSize = programInfo.globalVariables.size + programInfo.globalVariables.zeroInitSize;
    if (globalVariablesDataSize != 0) {
        this->globalVarBuffer = NEO::allocateGlobalsSurface(svmAllocsManager, *device->getNEODevice(), globalVariablesDataSize,
                                                            programInfo.globalVariables.zeroInitSize, false, programInfo.linkerInput.get(), programInfo.globalVariables.initData);
    }

    for (auto &kernelInfo : this->programInfo.kernelInfos) {
        deviceInfoConstants.maxWorkGroupSize = gfxCoreHelper.calculateMaxWorkGroupSize(kernelInfo->kernelDescriptor, static_cast<uint32_t>(device->getDeviceInfo().maxWorkGroupSize));
        kernelInfo->apply(deviceInfoConstants);
    }

    if (this->packedDeviceBinary != nullptr) {
        return ZE_RESULT_SUCCESS;
    }

    NEO::SingleDeviceBinary singleDeviceBinary = {};
    singleDeviceBinary.targetDevice = NEO::getTargetDevice(device->getNEODevice()->getRootDeviceEnvironment());
    singleDeviceBinary.buildOptions = this->options;
    singleDeviceBinary.deviceBinary = ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(this->unpackedDeviceBinary.get()), this->unpackedDeviceBinarySize);
    singleDeviceBinary.intermediateRepresentation = ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(this->irBinary.get()), this->irBinarySize);
    singleDeviceBinary.debugData = ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(this->debugData.get()), this->debugDataSize);
    std::string packWarnings;
    std::string packErrors;
    auto packedDeviceBinary = NEO::packDeviceBinary(singleDeviceBinary, packErrors, packWarnings);
    if (packedDeviceBinary.empty()) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    }
    this->packedDeviceBinary = makeCopy(packedDeviceBinary.data(), packedDeviceBinary.size());
    this->packedDeviceBinarySize = packedDeviceBinary.size();

    return ZE_RESULT_SUCCESS;
}

void ModuleTranslationUnit::updateBuildLog(const std::string &newLogEntry) {
    if (newLogEntry.empty() || ('\0' == newLogEntry[0])) {
        return;
    }

    buildLog += newLogEntry.c_str();
    if ('\n' != *buildLog.rbegin()) {
        buildLog.append("\n");
    }
}

void ModuleTranslationUnit::processDebugData() {
    if (this->debugData != nullptr) {
        iOpenCL::SProgramDebugDataHeaderIGC *programDebugHeader = reinterpret_cast<iOpenCL::SProgramDebugDataHeaderIGC *>(debugData.get());

        DEBUG_BREAK_IF(programDebugHeader->NumberOfKernels != programInfo.kernelInfos.size());

        const iOpenCL::SKernelDebugDataHeaderIGC *kernelDebugHeader = reinterpret_cast<iOpenCL::SKernelDebugDataHeaderIGC *>(
            ptrOffset(programDebugHeader, sizeof(iOpenCL::SProgramDebugDataHeaderIGC)));

        const char *kernelName = nullptr;
        const char *kernelDebugData = nullptr;

        for (uint32_t i = 0; i < programDebugHeader->NumberOfKernels; i++) {
            kernelName = reinterpret_cast<const char *>(ptrOffset(kernelDebugHeader, sizeof(iOpenCL::SKernelDebugDataHeaderIGC)));

            auto kernelInfo = programInfo.kernelInfos[i];
            UNRECOVERABLE_IF(kernelInfo->kernelDescriptor.kernelMetadata.kernelName.compare(0, kernelInfo->kernelDescriptor.kernelMetadata.kernelName.size(), kernelName) != 0);

            kernelDebugData = ptrOffset(kernelName, kernelDebugHeader->KernelNameSize);

            kernelInfo->kernelDescriptor.external.debugData = std::make_unique<NEO::DebugData>();

            char *alignedAlloc = static_cast<char *>(alignedMalloc(kernelDebugHeader->SizeVisaDbgInBytes, MemoryConstants::pageSize));
            memcpy_s(static_cast<void *>(alignedAlloc), kernelDebugHeader->SizeVisaDbgInBytes, kernelDebugData, kernelDebugHeader->SizeVisaDbgInBytes);

            kernelInfo->kernelDescriptor.external.debugData->vIsa = alignedAlloc;
            kernelInfo->kernelDescriptor.external.debugData->genIsa = ptrOffset(kernelDebugData, kernelDebugHeader->SizeVisaDbgInBytes);
            kernelInfo->kernelDescriptor.external.debugData->vIsaSize = kernelDebugHeader->SizeVisaDbgInBytes;
            kernelInfo->kernelDescriptor.external.debugData->genIsaSize = kernelDebugHeader->SizeGenIsaDbgInBytes;

            kernelDebugData = ptrOffset(kernelDebugData, static_cast<size_t>(kernelDebugHeader->SizeVisaDbgInBytes) + kernelDebugHeader->SizeGenIsaDbgInBytes);
            kernelDebugHeader = reinterpret_cast<const iOpenCL::SKernelDebugDataHeaderIGC *>(kernelDebugData);
            alignedvIsas.push_back(alignedAlloc);
        }
    }
}

ModuleImp::ModuleImp(Device *device, ModuleBuildLog *moduleBuildLog, ModuleType type)
    : device(device), translationUnit(std::make_unique<ModuleTranslationUnit>(device)),
      moduleBuildLog(moduleBuildLog), type(type) {
    productFamily = device->getHwInfo().platform.eProductFamily;
}

ModuleImp::~ModuleImp() {
    kernelImmDatas.clear();
}

NEO::Zebin::Debug::Segments ModuleImp::getZebinSegments() {
    std::vector<std::pair<std::string_view, NEO::GraphicsAllocation *>> kernels;
    for (const auto &kernelImmData : kernelImmDatas)
        kernels.push_back({kernelImmData->getDescriptor().kernelMetadata.kernelName, kernelImmData->getIsaGraphicsAllocation()});
    ArrayRef<const uint8_t> strings = {reinterpret_cast<const uint8_t *>(translationUnit->programInfo.globalStrings.initData),
                                       translationUnit->programInfo.globalStrings.size};
    return NEO::Zebin::Debug::Segments(translationUnit->globalVarBuffer, translationUnit->globalConstBuffer, strings, kernels);
}

ze_result_t ModuleImp::initialize(const ze_module_desc_t *desc, NEO::Device *neoDevice) {
    bool linkageSuccessful = true;
    ze_result_t result = ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;

    std::string buildOptions;
    std::string internalBuildOptions;

    if (desc->pNext) {
        const ze_base_desc_t *expDesc = reinterpret_cast<const ze_base_desc_t *>(desc->pNext);
        if (expDesc->stype == ZE_STRUCTURE_TYPE_MODULE_PROGRAM_EXP_DESC) {
            if (desc->format != ZE_MODULE_FORMAT_IL_SPIRV) {
                return ZE_RESULT_ERROR_INVALID_ENUMERATION;
            }
            this->builtFromSPIRv = true;
            const ze_module_program_exp_desc_t *programExpDesc =
                reinterpret_cast<const ze_module_program_exp_desc_t *>(expDesc);
            std::vector<const char *> inputSpirVs;
            std::vector<uint32_t> inputModuleSizes;
            std::vector<const ze_module_constants_t *> specConstants;
            const ze_module_constants_t *firstSpecConstants = nullptr;

            this->createBuildOptions(nullptr, buildOptions, internalBuildOptions);

            for (uint32_t i = 0; i < static_cast<uint32_t>(programExpDesc->count); i++) {
                std::string tmpBuildOptions;
                std::string tmpInternalBuildOptions;
                inputSpirVs.push_back(reinterpret_cast<const char *>(programExpDesc->pInputModules[i]));
                auto inputSizesInfo = const_cast<size_t *>(programExpDesc->inputSizes);
                uint32_t inputSize = static_cast<uint32_t>(inputSizesInfo[i]);
                inputModuleSizes.push_back(inputSize);
                if (programExpDesc->pConstants) {
                    specConstants.push_back(programExpDesc->pConstants[i]);
                    if (i == 0) {
                        firstSpecConstants = specConstants[0];
                    }
                }
                if (programExpDesc->pBuildFlags) {
                    this->createBuildOptions(programExpDesc->pBuildFlags[i], tmpBuildOptions, tmpInternalBuildOptions);
                    buildOptions = buildOptions + tmpBuildOptions;
                    internalBuildOptions = internalBuildOptions + tmpInternalBuildOptions;
                }
            }
            // If the user passed in only 1 SPIRV, then fallback to standard build
            if (inputSpirVs.size() > 1) {
                result = this->translationUnit->staticLinkSpirV(inputSpirVs,
                                                                inputModuleSizes,
                                                                buildOptions.c_str(),
                                                                internalBuildOptions.c_str(),
                                                                specConstants);
            } else {
                result = this->translationUnit->buildFromSpirV(reinterpret_cast<const char *>(programExpDesc->pInputModules[0]),
                                                               inputModuleSizes[0],
                                                               buildOptions.c_str(),
                                                               internalBuildOptions.c_str(),
                                                               firstSpecConstants);
            }
        } else {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
    } else {
        std::string buildFlagsInput{desc->pBuildFlags != nullptr ? desc->pBuildFlags : ""};
        if (!this->verifyBuildOptions(buildFlagsInput)) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }

        this->translationUnit->shouldSuppressRebuildWarning = NEO::CompilerOptions::extract(NEO::CompilerOptions::noRecompiledFromIr, buildFlagsInput);
        this->translationUnit->isBuiltIn = this->type == ModuleType::Builtin ? true : false;
        this->createBuildOptions(buildFlagsInput.c_str(), buildOptions, internalBuildOptions);

        if (type == ModuleType::User) {
            if (NEO::DebugManager.flags.InjectInternalBuildOptions.get() != "unk") {
                NEO::CompilerOptions::concatenateAppend(internalBuildOptions, NEO::DebugManager.flags.InjectInternalBuildOptions.get());
            }

            if (NEO::DebugManager.flags.InjectApiBuildOptions.get() != "unk") {
                NEO::CompilerOptions::concatenateAppend(buildOptions, NEO::DebugManager.flags.InjectApiBuildOptions.get());
            }
        }

        if (desc->format == ZE_MODULE_FORMAT_NATIVE) {
            // Assume Symbol Generation Given Prebuilt Binary
            this->isFunctionSymbolExportEnabled = true;
            this->isGlobalSymbolExportEnabled = true;
            result = this->translationUnit->createFromNativeBinary(
                reinterpret_cast<const char *>(desc->pInputModule), desc->inputSize);
        } else if (desc->format == ZE_MODULE_FORMAT_IL_SPIRV) {
            this->builtFromSPIRv = true;
            result = this->translationUnit->buildFromSpirV(reinterpret_cast<const char *>(desc->pInputModule),
                                                           static_cast<uint32_t>(desc->inputSize),
                                                           buildOptions.c_str(),
                                                           internalBuildOptions.c_str(),
                                                           desc->pConstants);
        } else {
            return ZE_RESULT_ERROR_INVALID_ENUMERATION;
        }
    }

    this->updateBuildLog(neoDevice);
    verifyDebugCapabilities();

    auto &rootDeviceEnvironment = neoDevice->getRootDeviceEnvironment();
    auto containsStatefulAccess = NEO::AddressingModeHelper::containsStatefulAccess(translationUnit->programInfo.kernelInfos, false);
    auto isUserKernel = (type == ModuleType::User);

    auto failBuildProgram = containsStatefulAccess &&
                            isUserKernel &&
                            NEO::AddressingModeHelper::failBuildProgramWithStatefulAccess(rootDeviceEnvironment);

    if (failBuildProgram) {
        result = ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    }

    if (result != ZE_RESULT_SUCCESS) {
        return result;
    }

    kernelImmDatas.reserve(this->translationUnit->programInfo.kernelInfos.size());
    for (auto &ki : this->translationUnit->programInfo.kernelInfos) {
        std::unique_ptr<KernelImmutableData> kernelImmData{new KernelImmutableData(this->device)};
        result = kernelImmData->initialize(ki, device, device->getNEODevice()->getDeviceInfo().computeUnitsUsedForScratch,
                                           this->translationUnit->globalConstBuffer, this->translationUnit->globalVarBuffer,
                                           this->type == ModuleType::Builtin);
        if (result != ZE_RESULT_SUCCESS) {
            return result;
        }
        kernelImmDatas.push_back(std::move(kernelImmData));
    }

    auto refBin = ArrayRef<const uint8_t>::fromAny(translationUnit->unpackedDeviceBinary.get(), translationUnit->unpackedDeviceBinarySize);
    if (NEO::isDeviceBinaryFormat<NEO::DeviceBinaryFormat::Zebin>(refBin)) {
        isZebinBinary = true;
    }

    StackVec<NEO::GraphicsAllocation *, 32> moduleAllocs = getModuleAllocations();
    if (!moduleAllocs.empty()) {
        auto minGpuAddressAlloc = std::min_element(moduleAllocs.begin(), moduleAllocs.end(), [](const auto &alloc1, const auto &alloc2) { return alloc1->getGpuAddress() < alloc2->getGpuAddress(); });
        moduleLoadAddress = (*minGpuAddressAlloc)->getGpuAddress();
    }

    registerElfInDebuggerL0();

    checkIfPrivateMemoryPerDispatchIsNeeded();

    linkageSuccessful = this->linkBinary();

    linkageSuccessful &= populateHostGlobalSymbolsMap(this->translationUnit->programInfo.globalsDeviceToHostNameMap);
    this->updateBuildLog(neoDevice);

    if (debugEnabled) {
        passDebugData();
    }

    const auto &productHelper = neoDevice->getProductHelper();

    if (this->isFullyLinked && this->type == ModuleType::User) {
        for (auto &ki : kernelImmDatas) {

            if (!ki->isIsaCopiedToAllocation()) {

                NEO::MemoryTransferHelper::transferMemoryToAllocation(productHelper.isBlitCopyRequiredForLocalMemory(rootDeviceEnvironment, *ki->getIsaGraphicsAllocation()),
                                                                      *neoDevice, ki->getIsaGraphicsAllocation(), 0, ki->getKernelInfo()->heapInfo.pKernelHeap,
                                                                      static_cast<size_t>(ki->getKernelInfo()->heapInfo.kernelHeapSize));

                ki->setIsaCopiedToAllocation();
            }
        }

        if (device->getL0Debugger()) {
            auto allocs = getModuleAllocations();
            device->getL0Debugger()->notifyModuleLoadAllocations(device->getNEODevice(), allocs);
            notifyModuleCreate();
        }
    }
    if (linkageSuccessful == false) {
        result = ZE_RESULT_ERROR_MODULE_LINK_FAILURE;
    }
    return result;
}

void ModuleImp::createDebugZebin() {
    auto refBin = ArrayRef<const uint8_t>::fromAny(translationUnit->unpackedDeviceBinary.get(), translationUnit->unpackedDeviceBinarySize);
    auto segments = getZebinSegments();
    auto debugZebin = NEO::Zebin::Debug::createDebugZebin(refBin, segments);

    translationUnit->debugDataSize = debugZebin.size();
    translationUnit->debugData.reset(new char[translationUnit->debugDataSize]);
    memcpy_s(translationUnit->debugData.get(), translationUnit->debugDataSize,
             debugZebin.data(), debugZebin.size());
}

void ModuleImp::passDebugData() {
    if (isZebinBinary) {
        createDebugZebin();
        if (device->getSourceLevelDebugger()) {
            NEO::DebugData debugData; // pass debug zebin in vIsa field
            debugData.vIsa = reinterpret_cast<const char *>(translationUnit->debugData.get());
            debugData.vIsaSize = static_cast<uint32_t>(translationUnit->debugDataSize);
            device->getSourceLevelDebugger()->notifyKernelDebugData(&debugData, "debug_zebin", nullptr, 0);
        }
    } else {
        if (device->getSourceLevelDebugger()) {
            for (auto kernelInfo : this->translationUnit->programInfo.kernelInfos) {
                NEO::DebugData *notifyDebugData = kernelInfo->kernelDescriptor.external.debugData.get();
                NEO::DebugData relocatedDebugData;

                if (kernelInfo->kernelDescriptor.external.relocatedDebugData.get()) {
                    relocatedDebugData.genIsa = kernelInfo->kernelDescriptor.external.debugData->genIsa;
                    relocatedDebugData.genIsaSize = kernelInfo->kernelDescriptor.external.debugData->genIsaSize;
                    relocatedDebugData.vIsa = reinterpret_cast<char *>(kernelInfo->kernelDescriptor.external.relocatedDebugData.get());
                    relocatedDebugData.vIsaSize = kernelInfo->kernelDescriptor.external.debugData->vIsaSize;
                    notifyDebugData = &relocatedDebugData;
                }

                device->getSourceLevelDebugger()->notifyKernelDebugData(notifyDebugData,
                                                                        kernelInfo->kernelDescriptor.kernelMetadata.kernelName,
                                                                        kernelInfo->heapInfo.pKernelHeap,
                                                                        kernelInfo->heapInfo.kernelHeapSize);
            }
        }
    }
}

const KernelImmutableData *ModuleImp::getKernelImmutableData(const char *kernelName) const {
    for (auto &kernelImmData : kernelImmDatas) {
        if (kernelImmData->getDescriptor().kernelMetadata.kernelName.compare(kernelName) == 0) {
            return kernelImmData.get();
        }
    }
    return nullptr;
}

uint32_t ModuleImp::getMaxGroupSize(const NEO::KernelDescriptor &kernelDescriptor) const {
    return this->device->getGfxCoreHelper().calculateMaxWorkGroupSize(kernelDescriptor, static_cast<uint32_t>(this->device->getDeviceInfo().maxWorkGroupSize));
}

void ModuleImp::createBuildOptions(const char *pBuildFlags, std::string &apiOptions, std::string &internalBuildOptions) {
    if (pBuildFlags != nullptr) {
        std::string buildFlags(pBuildFlags);

        apiOptions = pBuildFlags;
        NEO::CompilerOptions::applyAdditionalApiOptions(apiOptions);

        moveBuildOption(apiOptions, apiOptions, NEO::CompilerOptions::optDisable, BuildOptions::optDisable);
        moveBuildOption(internalBuildOptions, apiOptions, NEO::CompilerOptions::greaterThan4gbBuffersRequired, BuildOptions::greaterThan4GbRequired);
        moveBuildOption(internalBuildOptions, apiOptions, NEO::CompilerOptions::allowZebin, NEO::CompilerOptions::enableZebin);
        moveBuildOption(internalBuildOptions, apiOptions, NEO::CompilerOptions::largeGrf, BuildOptions::optLargeRegisterFile);
        moveBuildOption(internalBuildOptions, apiOptions, NEO::CompilerOptions::autoGrf, BuildOptions::optAutoGrf);

        NEO::CompilerOptions::applyAdditionalInternalOptions(internalBuildOptions);

        moveOptLevelOption(apiOptions, apiOptions);
        moveProfileFlagsOption(apiOptions, apiOptions);
        this->isFunctionSymbolExportEnabled = moveBuildOption(apiOptions, apiOptions, BuildOptions::enableLibraryCompile, BuildOptions::enableLibraryCompile);
        this->isGlobalSymbolExportEnabled = moveBuildOption(apiOptions, apiOptions, BuildOptions::enableGlobalVariableSymbols, BuildOptions::enableGlobalVariableSymbols);

        createBuildExtraOptions(apiOptions, internalBuildOptions);
    }
    if (NEO::ApiSpecificConfig::getBindlessConfiguration()) {
        NEO::CompilerOptions::concatenateAppend(internalBuildOptions, NEO::CompilerOptions::bindlessMode.str());
    }
}

bool ModuleImp::moveOptLevelOption(std::string &dstOptionsSet, std::string &srcOptionSet) {
    const char optDelim = ' ';
    const char valDelim = '=';

    auto optInSrcPos = srcOptionSet.find(BuildOptions::optLevel.begin());
    if (std::string::npos == optInSrcPos) {
        return false;
    }

    std::string dstOptionStr(NEO::CompilerOptions::optLevel);
    auto valInSrcPos = srcOptionSet.find(valDelim, optInSrcPos);
    auto optInSrcEndPos = srcOptionSet.find(optDelim, optInSrcPos);
    if (std::string::npos == valInSrcPos) {
        return false;
    }
    dstOptionStr += srcOptionSet.substr(valInSrcPos + 1, (optInSrcEndPos - (valInSrcPos + 1)));
    srcOptionSet.erase(optInSrcPos, (optInSrcEndPos + 1 - optInSrcPos));
    NEO::CompilerOptions::concatenateAppend(dstOptionsSet, dstOptionStr);
    return true;
}

bool ModuleImp::moveProfileFlagsOption(std::string &dstOptionsSet, std::string &srcOptionSet) {
    const char optDelim = ' ';

    auto optInSrcPos = srcOptionSet.find(BuildOptions::profileFlags.begin());
    if (std::string::npos == optInSrcPos) {
        return false;
    }

    std::string dstOptionStr(BuildOptions::profileFlags);
    auto valInSrcPos = srcOptionSet.find(optDelim, optInSrcPos);
    auto optInSrcEndPos = srcOptionSet.find(optDelim, valInSrcPos + 1);
    if (std::string::npos == valInSrcPos) {
        return false;
    }
    std::string valStr = srcOptionSet.substr(valInSrcPos, (optInSrcEndPos - valInSrcPos));
    profileFlags = static_cast<uint32_t>(strtoul(valStr.c_str(), nullptr, 16));
    dstOptionStr += valStr;

    srcOptionSet.erase(optInSrcPos, (optInSrcEndPos + 1 - optInSrcPos));
    NEO::CompilerOptions::concatenateAppend(dstOptionsSet, dstOptionStr);
    return true;
}

void ModuleImp::updateBuildLog(NEO::Device *neoDevice) {
    if (this->moduleBuildLog) {
        moduleBuildLog->appendString(this->translationUnit->buildLog.c_str(), this->translationUnit->buildLog.size());
    }
}

ze_result_t ModuleImp::createKernel(const ze_kernel_desc_t *desc,
                                    ze_kernel_handle_t *kernelHandle) {
    ze_result_t res;
    if (!isFullyLinked) {
        return ZE_RESULT_ERROR_INVALID_MODULE_UNLINKED;
    }
    auto kernel = Kernel::create(productFamily, this, desc, &res);

    if (res == ZE_RESULT_SUCCESS) {
        *kernelHandle = kernel->toHandle();
    }

    auto localMemSize = static_cast<uint32_t>(this->getDevice()->getNEODevice()->getDeviceInfo().localMemSize);

    for (const auto &kernelImmutableData : this->getKernelImmutableDataVector()) {
        auto slmInlineSize = kernelImmutableData->getDescriptor().kernelAttributes.slmInlineSize;
        if (slmInlineSize > 0 && localMemSize < slmInlineSize) {
            PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Size of SLM (%u) larger than available (%u)\n", slmInlineSize, localMemSize);
            res = ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
            break;
        }
    }

    return res;
}

ze_result_t ModuleImp::getNativeBinary(size_t *pSize, uint8_t *pModuleNativeBinary) {
    auto genBinary = this->translationUnit->packedDeviceBinary.get();

    *pSize = this->translationUnit->packedDeviceBinarySize;
    if (pModuleNativeBinary != nullptr) {
        memcpy_s(pModuleNativeBinary, this->translationUnit->packedDeviceBinarySize, genBinary, this->translationUnit->packedDeviceBinarySize);
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t ModuleImp::getDebugInfo(size_t *pDebugDataSize, uint8_t *pDebugData) {
    if (translationUnit == nullptr) {
        return ZE_RESULT_ERROR_UNINITIALIZED;
    }

    if (nullptr == translationUnit->debugData.get() && isZebinBinary) {
        createDebugZebin();
    }
    if (pDebugData != nullptr) {
        if (*pDebugDataSize < translationUnit->debugDataSize) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
        memcpy_s(pDebugData, *pDebugDataSize, translationUnit->debugData.get(), translationUnit->debugDataSize);
    }
    *pDebugDataSize = translationUnit->debugDataSize;
    return ZE_RESULT_SUCCESS;
}

void ModuleImp::copyPatchedSegments(const NEO::Linker::PatchableSegments &isaSegmentsForPatching) {
    if (this->translationUnit->programInfo.linkerInput && this->translationUnit->programInfo.linkerInput->getTraits().requiresPatchingOfInstructionSegments) {
        auto &rootDeviceEnvironment = device->getNEODevice()->getRootDeviceEnvironment();
        const auto &productHelper = this->device->getProductHelper();

        for (auto &kernelImmData : this->kernelImmDatas) {
            if (nullptr == kernelImmData->getIsaGraphicsAllocation()) {
                continue;
            }

            UNRECOVERABLE_IF(kernelImmData->isIsaCopiedToAllocation());

            kernelImmData->getIsaGraphicsAllocation()->setTbxWritable(true, std::numeric_limits<uint32_t>::max());
            kernelImmData->getIsaGraphicsAllocation()->setAubWritable(true, std::numeric_limits<uint32_t>::max());
            auto segmentId = &kernelImmData - &this->kernelImmDatas[0];

            NEO::MemoryTransferHelper::transferMemoryToAllocation(productHelper.isBlitCopyRequiredForLocalMemory(rootDeviceEnvironment, *kernelImmData->getIsaGraphicsAllocation()),
                                                                  *device->getNEODevice(), kernelImmData->getIsaGraphicsAllocation(), 0, isaSegmentsForPatching[segmentId].hostPointer,
                                                                  isaSegmentsForPatching[segmentId].segmentSize);

            kernelImmData->setIsaCopiedToAllocation();

            if (device->getL0Debugger()) {
                NEO::MemoryOperationsHandler *memoryOperationsIface = rootDeviceEnvironment.memoryOperationsInterface.get();
                auto allocation = kernelImmData->getIsaGraphicsAllocation();
                if (memoryOperationsIface) {
                    memoryOperationsIface->makeResident(device->getNEODevice(), ArrayRef<NEO::GraphicsAllocation *>(&allocation, 1));
                }
            }
        }
    }
}

bool ModuleImp::linkBinary() {
    using namespace NEO;
    auto linkerInput = this->translationUnit->programInfo.linkerInput.get();
    if (linkerInput == nullptr) {
        isFullyLinked = true;
        return true;
    }
    Linker linker(*linkerInput);
    Linker::SegmentInfo globals;
    Linker::SegmentInfo constants;
    Linker::SegmentInfo exportedFunctions;
    Linker::SegmentInfo strings;
    GraphicsAllocation *globalsForPatching = translationUnit->globalVarBuffer;
    GraphicsAllocation *constantsForPatching = translationUnit->globalConstBuffer;
    if (globalsForPatching != nullptr) {
        globals.gpuAddress = static_cast<uintptr_t>(globalsForPatching->getGpuAddress());
        globals.segmentSize = globalsForPatching->getUnderlyingBufferSize();
    }
    if (constantsForPatching != nullptr) {
        constants.gpuAddress = static_cast<uintptr_t>(constantsForPatching->getGpuAddress());
        constants.segmentSize = constantsForPatching->getUnderlyingBufferSize();
    }
    if (translationUnit->programInfo.globalStrings.initData != nullptr) {
        strings.gpuAddress = reinterpret_cast<uintptr_t>(translationUnit->programInfo.globalStrings.initData);
        strings.segmentSize = translationUnit->programInfo.globalStrings.size;
    }
    if (linkerInput->getExportedFunctionsSegmentId() >= 0) {
        auto exportedFunctionHeapId = linkerInput->getExportedFunctionsSegmentId();
        this->exportedFunctionsSurface = this->kernelImmDatas[exportedFunctionHeapId]->getIsaGraphicsAllocation();
        exportedFunctions.gpuAddress = static_cast<uintptr_t>(exportedFunctionsSurface->getGpuAddressToPatch());
        exportedFunctions.segmentSize = exportedFunctionsSurface->getUnderlyingBufferSize();
    }

    Linker::KernelDescriptorsT kernelDescriptors;
    if (linkerInput->getTraits().requiresPatchingOfInstructionSegments) {
        patchedIsaTempStorage.reserve(this->kernelImmDatas.size());
        kernelDescriptors.reserve(this->kernelImmDatas.size());
        for (size_t i = 0; i < kernelImmDatas.size(); i++) {
            auto kernelInfo = this->translationUnit->programInfo.kernelInfos.at(i);
            auto &kernHeapInfo = kernelInfo->heapInfo;
            const char *originalIsa = reinterpret_cast<const char *>(kernHeapInfo.pKernelHeap);
            patchedIsaTempStorage.push_back(std::vector<char>(originalIsa, originalIsa + kernHeapInfo.kernelHeapSize));
            isaSegmentsForPatching.push_back(Linker::PatchableSegment{patchedIsaTempStorage.rbegin()->data(), static_cast<uintptr_t>(kernelImmDatas.at(i)->getIsaGraphicsAllocation()->getGpuAddressToPatch()), kernHeapInfo.kernelHeapSize});
            kernelDescriptors.push_back(&kernelInfo->kernelDescriptor);
        }
    }

    auto linkStatus = linker.link(globals, constants, exportedFunctions, strings,
                                  globalsForPatching, constantsForPatching,
                                  isaSegmentsForPatching, unresolvedExternalsInfo, this->device->getNEODevice(),
                                  translationUnit->programInfo.globalConstants.initData,
                                  translationUnit->programInfo.globalConstants.size,
                                  translationUnit->programInfo.globalVariables.initData,
                                  translationUnit->programInfo.globalVariables.size,
                                  kernelDescriptors, translationUnit->programInfo.externalFunctions);
    this->symbols = linker.extractRelocatedSymbols();
    if (LinkingStatus::LinkedFully != linkStatus) {
        if (moduleBuildLog) {
            std::vector<std::string> kernelNames;
            for (const auto &kernelInfo : this->translationUnit->programInfo.kernelInfos) {
                kernelNames.push_back("kernel : " + kernelInfo->kernelDescriptor.kernelMetadata.kernelName);
            }
            auto error = constructLinkerErrorMessage(unresolvedExternalsInfo, kernelNames);
            moduleBuildLog->appendString(error.c_str(), error.size());
        }
        isFullyLinked = false;
        return LinkingStatus::LinkedPartially == linkStatus;
    } else if (type != ModuleType::Builtin) {
        copyPatchedSegments(isaSegmentsForPatching);
    } else {
        for (auto &kernelDescriptor : kernelDescriptors) {
            kernelDescriptor->kernelAttributes.flags.requiresImplicitArgs = false;
        }
    }

    DBG_LOG(PrintRelocations, NEO::constructRelocationsDebugMessage(this->symbols));
    isFullyLinked = true;
    for (auto kernelId = 0u; kernelId < kernelImmDatas.size(); kernelId++) {
        auto &kernImmData = kernelImmDatas[kernelId];

        kernImmData->getResidencyContainer().reserve(kernImmData->getResidencyContainer().size() +
                                                     ((this->exportedFunctionsSurface != nullptr) ? 1 : 0) + this->importedSymbolAllocations.size());

        if (nullptr != this->exportedFunctionsSurface) {
            kernImmData->getResidencyContainer().push_back(this->exportedFunctionsSurface);
        }
        kernImmData->getResidencyContainer().insert(kernImmData->getResidencyContainer().end(), this->importedSymbolAllocations.begin(),
                                                    this->importedSymbolAllocations.end());
    }
    return true;
}

ze_result_t ModuleImp::getFunctionPointer(const char *pFunctionName, void **pfnFunction) {
    // Check if the function is in the exported symbol table
    auto symbolIt = symbols.find(pFunctionName);
    if ((symbolIt != symbols.end()) && (symbolIt->second.symbol.segment == NEO::SegmentType::Instructions)) {
        *pfnFunction = reinterpret_cast<void *>(symbolIt->second.gpuAddress);
    }
    // If the Function Pointer is not in the exported symbol table, then this function might be a kernel.
    // Check if the function name matches a kernel and return the gpu address to that function
    if (*pfnFunction == nullptr) {
        auto kernelImmData = this->getKernelImmutableData(pFunctionName);
        if (kernelImmData != nullptr) {
            auto isaAllocation = kernelImmData->getIsaGraphicsAllocation();
            *pfnFunction = reinterpret_cast<void *>(isaAllocation->getGpuAddress());
            // Ensure that any kernel in this module which uses this kernel module function pointer has access to the memory.
            for (auto &data : this->getKernelImmutableDataVector()) {
                if (data.get() != kernelImmData) {
                    data.get()->getResidencyContainer().insert(data.get()->getResidencyContainer().end(), isaAllocation);
                }
            }
        }
    }

    if (*pfnFunction == nullptr) {
        if (!this->isFunctionSymbolExportEnabled) {
            PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Function Pointers Not Supported Without Compiler flag %s\n", BuildOptions::enableLibraryCompile.str().c_str());
            return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
        }
        return ZE_RESULT_ERROR_INVALID_FUNCTION_NAME;
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t ModuleImp::getGlobalPointer(const char *pGlobalName, size_t *pSize, void **pPtr) {
    uint64_t address;
    size_t size;

    auto hostSymbolIt = hostGlobalSymbolsMap.find(pGlobalName);
    if (hostSymbolIt != hostGlobalSymbolsMap.end()) {
        address = hostSymbolIt->second.address;
        size = hostSymbolIt->second.size;
    } else {
        auto deviceSymbolIt = symbols.find(pGlobalName);
        if (deviceSymbolIt != symbols.end()) {
            if (deviceSymbolIt->second.symbol.segment == NEO::SegmentType::Instructions) {
                return ZE_RESULT_ERROR_INVALID_GLOBAL_NAME;
            }
        } else {
            if (!this->isGlobalSymbolExportEnabled) {
                PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Global Pointers Not Supported Without Compiler flag %s\n", BuildOptions::enableGlobalVariableSymbols.str().c_str());
                return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
            }
            return ZE_RESULT_ERROR_INVALID_GLOBAL_NAME;
        }
        address = deviceSymbolIt->second.gpuAddress;
        size = deviceSymbolIt->second.symbol.size;
    }
    if (pPtr) {
        *pPtr = reinterpret_cast<void *>(address);
    }
    if (pSize) {
        *pSize = size;
    }
    return ZE_RESULT_SUCCESS;
}

Module *Module::create(Device *device, const ze_module_desc_t *desc,
                       ModuleBuildLog *moduleBuildLog, ModuleType type, ze_result_t *result) {
    auto module = new ModuleImp(device, moduleBuildLog, type);

    *result = module->initialize(desc, device->getNEODevice());
    if (*result != ZE_RESULT_SUCCESS) {
        module->destroy();
        return nullptr;
    }

    return module;
}

ze_result_t ModuleImp::getKernelNames(uint32_t *pCount, const char **pNames) {
    auto &kernelImmDatas = this->getKernelImmutableDataVector();
    if (*pCount == 0) {
        *pCount = static_cast<uint32_t>(kernelImmDatas.size());
        return ZE_RESULT_SUCCESS;
    }

    if (*pCount > static_cast<uint32_t>(kernelImmDatas.size())) {
        *pCount = static_cast<uint32_t>(kernelImmDatas.size());
    }

    uint32_t outCount = 0;
    for (auto &kernelImmData : kernelImmDatas) {
        *(pNames + outCount) = kernelImmData->getDescriptor().kernelMetadata.kernelName.c_str();
        outCount++;
        if (outCount == *pCount) {
            break;
        }
    }

    return ZE_RESULT_SUCCESS;
}

bool ModuleImp::isDebugEnabled() const {
    return debugEnabled;
}

void ModuleImp::verifyDebugCapabilities() {
    bool debugCapabilities = device->getNEODevice()->getDebugger() != nullptr;

    if (debugCapabilities) {
        // verify all kernels are debuggable
        for (auto kernelInfo : this->translationUnit->programInfo.kernelInfos) {
            bool systemThreadSurfaceAvailable = NEO::isValidOffset(kernelInfo->kernelDescriptor.payloadMappings.implicitArgs.systemThreadSurfaceAddress.bindful) ||
                                                NEO::isValidOffset(kernelInfo->kernelDescriptor.payloadMappings.implicitArgs.systemThreadSurfaceAddress.bindless);

            debugCapabilities &= systemThreadSurfaceAvailable;
        }
    }
    debugEnabled = debugCapabilities;
}

void ModuleImp::checkIfPrivateMemoryPerDispatchIsNeeded() {
    size_t modulePrivateMemorySize = 0;
    for (auto &kernelImmData : this->kernelImmDatas) {
        if (0 == kernelImmData->getDescriptor().kernelAttributes.perHwThreadPrivateMemorySize) {
            continue;
        }
        auto kernelPrivateMemorySize = NEO::KernelHelper::getPrivateSurfaceSize(kernelImmData->getDescriptor().kernelAttributes.perHwThreadPrivateMemorySize,
                                                                                this->device->getNEODevice()->getDeviceInfo().computeUnitsUsedForScratch);
        modulePrivateMemorySize += kernelPrivateMemorySize;
    }

    this->allocatePrivateMemoryPerDispatch = false;
    if (modulePrivateMemorySize > 0U) {
        auto globalMemorySize = device->getNEODevice()->getRootDevice()->getGlobalMemorySize(static_cast<uint32_t>(device->getNEODevice()->getDeviceBitfield().to_ulong()));
        this->allocatePrivateMemoryPerDispatch = modulePrivateMemorySize > globalMemorySize;
    }
}

ze_result_t ModuleImp::getProperties(ze_module_properties_t *pModuleProperties) {
    pModuleProperties->flags = 0;

    if (!unresolvedExternalsInfo.empty()) {
        pModuleProperties->flags |= ZE_MODULE_PROPERTY_FLAG_IMPORTS;
    }

    return ZE_RESULT_SUCCESS;
}

ze_result_t ModuleImp::performDynamicLink(uint32_t numModules,
                                          ze_module_handle_t *phModules,
                                          ze_module_build_log_handle_t *phLinkLog) {
    std::map<void *, std::map<void *, void *>> dependencies;
    ModuleBuildLog *moduleLinkLog = nullptr;
    if (phLinkLog) {
        moduleLinkLog = ModuleBuildLog::create();
        *phLinkLog = moduleLinkLog->toHandle();
    }
    for (auto i = 0u; i < numModules; i++) {
        auto moduleId = static_cast<ModuleImp *>(Module::fromHandle(phModules[i]));
        // Add all provided Module's Exported Functions Surface to each Module to allow for all symbols
        // to be accessed from any module either directly thru Unresolved symbol resolution below or indirectly
        // thru function pointers or callbacks between the Modules.
        uint32_t functionSymbolExportEnabledCounter = 0;
        for (auto i = 0u; i < numModules; i++) {
            auto moduleHandle = static_cast<ModuleImp *>(Module::fromHandle(phModules[i]));
            functionSymbolExportEnabledCounter += static_cast<uint32_t>(moduleHandle->isFunctionSymbolExportEnabled);
            if (nullptr != moduleHandle->exportedFunctionsSurface) {
                moduleId->importedSymbolAllocations.insert(moduleHandle->exportedFunctionsSurface);
            }
        }
        for (auto &kernImmData : moduleId->kernelImmDatas) {
            kernImmData->getResidencyContainer().insert(kernImmData->getResidencyContainer().end(), moduleId->importedSymbolAllocations.begin(),
                                                        moduleId->importedSymbolAllocations.end());
        }

        // If the Module is fully linked, this means no Unresolved Symbols Exist that require patching.
        if (moduleId->isFullyLinked) {
            continue;
        }

        // Resolve Unresolved Symbols in the Relocation Table between the Modules if Required.
        auto &isaSegmentsForPatching = moduleId->isaSegmentsForPatching;
        auto &patchedIsaTempStorage = moduleId->patchedIsaTempStorage;
        uint32_t numPatchedSymbols = 0u;
        std::vector<std::string> unresolvedSymbolLogMessages;
        if (moduleId->translationUnit->programInfo.linkerInput && moduleId->translationUnit->programInfo.linkerInput->getTraits().requiresPatchingOfInstructionSegments) {
            if (patchedIsaTempStorage.empty()) {
                patchedIsaTempStorage.reserve(moduleId->kernelImmDatas.size());
                for (size_t i = 0; i < kernelImmDatas.size(); i++) {
                    const auto kernelInfo = this->translationUnit->programInfo.kernelInfos.at(i);
                    auto &kernHeapInfo = kernelInfo->heapInfo;
                    const char *originalIsa = reinterpret_cast<const char *>(kernHeapInfo.pKernelHeap);
                    patchedIsaTempStorage.push_back(std::vector<char>(originalIsa, originalIsa + kernHeapInfo.kernelHeapSize));
                    isaSegmentsForPatching.push_back(NEO::Linker::PatchableSegment{patchedIsaTempStorage.rbegin()->data(), static_cast<uintptr_t>(kernelImmDatas.at(i)->getIsaGraphicsAllocation()->getGpuAddressToPatch()), kernHeapInfo.kernelHeapSize});
                }
            }
            for (const auto &unresolvedExternal : moduleId->unresolvedExternalsInfo) {
                if (moduleLinkLog) {
                    std::stringstream logMessage;
                    logMessage << "Module <" << moduleId << ">: "
                               << " Unresolved Symbol <" << unresolvedExternal.unresolvedRelocation.symbolName << ">";
                    unresolvedSymbolLogMessages.push_back(logMessage.str());
                }
                for (auto i = 0u; i < numModules; i++) {
                    auto moduleHandle = static_cast<ModuleImp *>(Module::fromHandle(phModules[i]));
                    auto symbolIt = moduleHandle->symbols.find(unresolvedExternal.unresolvedRelocation.symbolName);
                    if (symbolIt != moduleHandle->symbols.end()) {
                        auto relocAddress = ptrOffset(isaSegmentsForPatching[unresolvedExternal.instructionsSegmentId].hostPointer,
                                                      static_cast<uintptr_t>(unresolvedExternal.unresolvedRelocation.offset));

                        NEO::Linker::patchAddress(relocAddress, symbolIt->second.gpuAddress, unresolvedExternal.unresolvedRelocation);
                        numPatchedSymbols++;

                        if (moduleLinkLog) {
                            std::stringstream logMessage;
                            logMessage << " Successfully Resolved Thru Dynamic Link to Module <" << moduleHandle << ">";
                            unresolvedSymbolLogMessages.back().append(logMessage.str());
                        }

                        break;
                    }
                }
            }
        }
        if (moduleLinkLog) {
            for (int i = 0; i < (int)unresolvedSymbolLogMessages.size(); i++) {
                moduleLinkLog->appendString(unresolvedSymbolLogMessages[i].c_str(), unresolvedSymbolLogMessages[i].size());
            }
        }
        if (numPatchedSymbols != moduleId->unresolvedExternalsInfo.size()) {
            if (functionSymbolExportEnabledCounter == 0) {
                PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Dynamic Link Not Supported Without Compiler flag %s\n", BuildOptions::enableLibraryCompile.str().c_str());
                return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
            }
            return ZE_RESULT_ERROR_MODULE_LINK_FAILURE;
        }
        moduleId->copyPatchedSegments(isaSegmentsForPatching);
        moduleId->isFullyLinked = true;
    }

    {
        NEO::ExternalFunctionInfosT externalFunctionInfos;
        NEO::FunctionDependenciesT extFuncDependencies;
        NEO::KernelDependenciesT kernelDependencies;
        NEO::KernelDescriptorMapT nameToKernelDescriptor;
        for (auto i = 0u; i < numModules; i++) {
            auto moduleId = static_cast<ModuleImp *>(Module::fromHandle(phModules[i]));
            auto &programInfo = moduleId->translationUnit->programInfo;

            auto toPtrVec = [](auto &inVec, auto &outPtrVec) {
                auto pos = outPtrVec.size();
                outPtrVec.resize(pos + inVec.size());
                for (size_t i = 0; i < inVec.size(); i++) {
                    outPtrVec[pos + i] = &inVec[i];
                }
            };
            toPtrVec(programInfo.externalFunctions, externalFunctionInfos);
            if (programInfo.linkerInput) {
                toPtrVec(programInfo.linkerInput->getFunctionDependencies(), extFuncDependencies);
                toPtrVec(programInfo.linkerInput->getKernelDependencies(), kernelDependencies);
            }

            for (auto &kernelInfo : programInfo.kernelInfos) {
                auto &kd = kernelInfo->kernelDescriptor;
                nameToKernelDescriptor[kd.kernelMetadata.kernelName] = &kd;
            }
        }
        auto error = NEO::resolveExternalDependencies(externalFunctionInfos, kernelDependencies, extFuncDependencies, nameToKernelDescriptor);
        if (error != NEO::RESOLVE_SUCCESS) {
            return ZE_RESULT_ERROR_MODULE_LINK_FAILURE;
        }
    }
    return ZE_RESULT_SUCCESS;
}

bool ModuleImp::populateHostGlobalSymbolsMap(std::unordered_map<std::string, std::string> &devToHostNameMapping) {
    bool retVal = true;
    hostGlobalSymbolsMap.reserve(devToHostNameMapping.size());
    for (auto &[devName, hostName] : devToHostNameMapping) {
        auto findSymbolRes = symbols.find(devName);
        if (findSymbolRes != symbols.end()) {
            auto symbol = findSymbolRes->second;
            if (isDataSegment(symbol.symbol.segment)) {
                HostGlobalSymbol hostGlobalSymbol;
                hostGlobalSymbol.address = symbol.gpuAddress;
                hostGlobalSymbol.size = symbol.symbol.size;
                hostGlobalSymbolsMap[hostName] = hostGlobalSymbol;
            } else {
                translationUnit->buildLog.append("Error: Symbol with given device name: " + devName + " is not in .data segment.\n");
                retVal = false;
            }
        } else {
            translationUnit->buildLog.append("Error: No symbol found with given device name: " + devName + ".\n");
            retVal = false;
        }
    }
    return retVal;
}

ze_result_t ModuleImp::destroy() {
    notifyModuleDestroy();

    auto tempHandle = debugModuleHandle;
    auto tempDevice = device;
    delete this;

    if (tempDevice->getL0Debugger() && tempHandle != 0) {
        tempDevice->getL0Debugger()->removeZebinModule(tempHandle);
    }

    return ZE_RESULT_SUCCESS;
}

void ModuleImp::registerElfInDebuggerL0() {
    auto debuggerL0 = device->getL0Debugger();

    if (this->type != ModuleType::User || !debuggerL0) {
        return;
    }

    if (isZebinBinary) {
        size_t debugDataSize = 0;
        getDebugInfo(&debugDataSize, nullptr);

        NEO::DebugData debugData; // pass debug zebin in vIsa field
        debugData.vIsa = reinterpret_cast<const char *>(translationUnit->debugData.get());
        debugData.vIsaSize = static_cast<uint32_t>(translationUnit->debugDataSize);
        this->debugElfHandle = debuggerL0->registerElf(&debugData);

        if (NEO::DebugManager.flags.DebuggerLogBitmask.get() & NEO::DebugVariables::DEBUGGER_LOG_BITMASK::DUMP_ELF) {
            dumpFileIncrement(debugData.vIsa, debugData.vIsaSize, "dumped_debug_module", ".elf");
        }

        StackVec<NEO::GraphicsAllocation *, 32> segmentAllocs;
        for (auto &kernImmData : kernelImmDatas) {
            segmentAllocs.push_back(kernImmData->getIsaGraphicsAllocation());
        }

        if (translationUnit->globalVarBuffer) {
            segmentAllocs.push_back(translationUnit->globalVarBuffer);
        }
        if (translationUnit->globalConstBuffer) {
            segmentAllocs.push_back(translationUnit->globalConstBuffer);
        }

        debuggerL0->attachZebinModuleToSegmentAllocations(segmentAllocs, this->debugModuleHandle, this->debugElfHandle);
    } else {
        for (auto &kernImmData : kernelImmDatas) {
            if (kernImmData->getKernelInfo()->kernelDescriptor.external.debugData.get()) {
                NEO::DebugData *notifyDebugData = kernImmData->getKernelInfo()->kernelDescriptor.external.debugData.get();
                NEO::DebugData relocatedDebugData;

                if (kernImmData->getKernelInfo()->kernelDescriptor.external.relocatedDebugData.get()) {
                    relocatedDebugData.genIsa = kernImmData->getKernelInfo()->kernelDescriptor.external.debugData->genIsa;
                    relocatedDebugData.genIsaSize = kernImmData->getKernelInfo()->kernelDescriptor.external.debugData->genIsaSize;
                    relocatedDebugData.vIsa = reinterpret_cast<char *>(kernImmData->getKernelInfo()->kernelDescriptor.external.relocatedDebugData.get());
                    relocatedDebugData.vIsaSize = kernImmData->getKernelInfo()->kernelDescriptor.external.debugData->vIsaSize;
                    notifyDebugData = &relocatedDebugData;
                }

                debuggerL0->registerElfAndLinkWithAllocation(notifyDebugData, kernImmData->getIsaGraphicsAllocation());
            }
        }
    }
}

void ModuleImp::notifyModuleCreate() {
    auto debuggerL0 = device->getL0Debugger();

    if (!debuggerL0) {
        return;
    }

    if (isZebinBinary) {
        size_t debugDataSize = 0;
        getDebugInfo(&debugDataSize, nullptr);
        UNRECOVERABLE_IF(!translationUnit->debugData);
        debuggerL0->notifyModuleCreate(translationUnit->debugData.get(), static_cast<uint32_t>(debugDataSize), moduleLoadAddress);
    } else {
        for (auto &kernImmData : kernelImmDatas) {
            auto debugData = kernImmData->getKernelInfo()->kernelDescriptor.external.debugData.get();
            auto relocatedDebugData = kernImmData->getKernelInfo()->kernelDescriptor.external.relocatedDebugData.get();

            if (debugData) {
                debuggerL0->notifyModuleCreate(relocatedDebugData ? reinterpret_cast<char *>(relocatedDebugData) : const_cast<char *>(debugData->vIsa), debugData->vIsaSize, kernImmData->getIsaGraphicsAllocation()->getGpuAddress());
            } else {
                debuggerL0->notifyModuleCreate(nullptr, 0, kernImmData->getIsaGraphicsAllocation()->getGpuAddress());
            }
        }
    }
}

void ModuleImp::notifyModuleDestroy() {
    auto debuggerL0 = device->getL0Debugger();

    if (!debuggerL0) {
        return;
    }

    if (isZebinBinary) {
        debuggerL0->notifyModuleDestroy(moduleLoadAddress);
    } else {
        for (auto &kernImmData : kernelImmDatas) {
            debuggerL0->notifyModuleDestroy(kernImmData->getIsaGraphicsAllocation()->getGpuAddress());
        }
    }
}

StackVec<NEO::GraphicsAllocation *, 32> ModuleImp::getModuleAllocations() {
    StackVec<NEO::GraphicsAllocation *, 32> allocs;
    for (auto &kernImmData : kernelImmDatas) {
        allocs.push_back(kernImmData->getIsaGraphicsAllocation());
    }

    if (translationUnit) {
        if (translationUnit->globalVarBuffer) {
            allocs.push_back(translationUnit->globalVarBuffer);
        }
        if (translationUnit->globalConstBuffer) {
            allocs.push_back(translationUnit->globalConstBuffer);
        }
    }
    return allocs;
}

bool moveBuildOption(std::string &dstOptionsSet, std::string &srcOptionSet, NEO::ConstStringRef dstOptionName, NEO::ConstStringRef srcOptionName) {
    auto optInSrcPos = srcOptionSet.find(srcOptionName.begin());
    if (std::string::npos == optInSrcPos) {
        return false;
    }

    srcOptionSet.erase(optInSrcPos, srcOptionName.length());
    NEO::CompilerOptions::concatenateAppend(dstOptionsSet, dstOptionName);
    return true;
}

} // namespace L0
