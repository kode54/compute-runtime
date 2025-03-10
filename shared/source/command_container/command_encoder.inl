/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/command_container/command_encoder.h"
#include "shared/source/command_stream/linear_stream.h"
#include "shared/source/debugger/debugger_l0.h"
#include "shared/source/device/device.h"
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/helpers/bindless_heaps_helper.h"
#include "shared/source/helpers/blit_commands_helper.h"
#include "shared/source/helpers/definitions/command_encoder_args.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/local_id_gen.h"
#include "shared/source/helpers/preamble.h"
#include "shared/source/helpers/register_offsets.h"
#include "shared/source/helpers/simd_helper.h"
#include "shared/source/helpers/string.h"
#include "shared/source/image/image_surface_state.h"
#include "shared/source/indirect_heap/indirect_heap.h"
#include "shared/source/kernel/dispatch_kernel_encoder_interface.h"
#include "shared/source/kernel/implicit_args.h"
#include "shared/source/kernel/kernel_descriptor.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/program/kernel_info.h"

#include "encode_surface_state.inl"
#include "encode_surface_state_args.h"

#include <algorithm>

namespace NEO {

template <typename Family>
uint32_t EncodeStates<Family>::copySamplerState(IndirectHeap *dsh,
                                                uint32_t samplerStateOffset,
                                                uint32_t samplerCount,
                                                uint32_t borderColorOffset,
                                                const void *fnDynamicStateHeap,
                                                BindlessHeapsHelper *bindlessHeapHelper,
                                                const RootDeviceEnvironment &rootDeviceEnvironment) {
    auto sizeSamplerState = sizeof(SAMPLER_STATE) * samplerCount;
    auto borderColorSize = samplerStateOffset - borderColorOffset;

    SAMPLER_STATE *dstSamplerState = nullptr;
    uint32_t samplerStateOffsetInDsh = 0;

    dsh->align(EncodeStates<Family>::alignIndirectStatePointer);
    uint32_t borderColorOffsetInDsh = 0;
    if (!ApiSpecificConfig::getBindlessConfiguration()) {
        borderColorOffsetInDsh = static_cast<uint32_t>(dsh->getUsed());
        auto borderColor = dsh->getSpace(borderColorSize);

        memcpy_s(borderColor, borderColorSize, ptrOffset(fnDynamicStateHeap, borderColorOffset),
                 borderColorSize);

        dsh->align(INTERFACE_DESCRIPTOR_DATA::SAMPLERSTATEPOINTER_ALIGN_SIZE);
        samplerStateOffsetInDsh = static_cast<uint32_t>(dsh->getUsed());

        dstSamplerState = reinterpret_cast<SAMPLER_STATE *>(dsh->getSpace(sizeSamplerState));
    } else {
        auto borderColor = reinterpret_cast<const SAMPLER_BORDER_COLOR_STATE *>(ptrOffset(fnDynamicStateHeap, borderColorOffset));
        if (borderColor->getBorderColorRed() != 0.0f ||
            borderColor->getBorderColorGreen() != 0.0f ||
            borderColor->getBorderColorBlue() != 0.0f ||
            (borderColor->getBorderColorAlpha() != 0.0f && borderColor->getBorderColorAlpha() != 1.0f)) {
            UNRECOVERABLE_IF(true);
        } else if (borderColor->getBorderColorAlpha() == 0.0f) {
            borderColorOffsetInDsh = bindlessHeapHelper->getDefaultBorderColorOffset();
        } else {
            borderColorOffsetInDsh = bindlessHeapHelper->getAlphaBorderColorOffset();
        }
        dsh->align(INTERFACE_DESCRIPTOR_DATA::SAMPLERSTATEPOINTER_ALIGN_SIZE);
        auto samplerStateInDsh = bindlessHeapHelper->allocateSSInHeap(sizeSamplerState, nullptr, BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
        dstSamplerState = reinterpret_cast<SAMPLER_STATE *>(samplerStateInDsh.ssPtr);
        samplerStateOffsetInDsh = static_cast<uint32_t>(samplerStateInDsh.surfaceStateOffset);
    }

    auto &helper = rootDeviceEnvironment.getHelper<ProductHelper>();
    auto &hwInfo = *rootDeviceEnvironment.getHardwareInfo();
    auto srcSamplerState = reinterpret_cast<const SAMPLER_STATE *>(ptrOffset(fnDynamicStateHeap, samplerStateOffset));
    SAMPLER_STATE state = {};
    for (uint32_t i = 0; i < samplerCount; i++) {
        state = srcSamplerState[i];
        state.setIndirectStatePointer(static_cast<uint32_t>(borderColorOffsetInDsh));
        helper.adjustSamplerState(&state, hwInfo);
        dstSamplerState[i] = state;
    }

    return samplerStateOffsetInDsh;
} // namespace NEO

template <typename Family>
void EncodeMathMMIO<Family>::encodeMulRegVal(CommandContainer &container, uint32_t offset, uint32_t val, uint64_t dstAddress) {
    int logLws = 0;
    int i = val;
    while (val >> logLws) {
        logLws++;
    }

    EncodeSetMMIO<Family>::encodeREG(container, CS_GPR_R0, offset);
    EncodeSetMMIO<Family>::encodeIMM(container, CS_GPR_R1, 0, true);

    i = 0;
    while (i < logLws) {
        if (val & (1 << i)) {
            EncodeMath<Family>::addition(container, AluRegisters::R_1,
                                         AluRegisters::R_0, AluRegisters::R_2);
            EncodeSetMMIO<Family>::encodeREG(container, CS_GPR_R1, CS_GPR_R2);
        }
        EncodeMath<Family>::addition(container, AluRegisters::R_0,
                                     AluRegisters::R_0, AluRegisters::R_2);
        EncodeSetMMIO<Family>::encodeREG(container, CS_GPR_R0, CS_GPR_R2);
        i++;
    }
    EncodeStoreMMIO<Family>::encode(*container.getCommandStream(), CS_GPR_R1, dstAddress, false);
}

/*
 * Compute *firstOperand > secondOperand and store the result in
 * MI_PREDICATE_RESULT where  firstOperand is an device memory address.
 *
 * To calculate the "greater than" operation in the device,
 * (secondOperand - *firstOperand) is used, and if the carry flag register is
 * set, then (*firstOperand) is greater than secondOperand.
 */
template <typename Family>
void EncodeMathMMIO<Family>::encodeGreaterThanPredicate(CommandContainer &container, uint64_t firstOperand, uint32_t secondOperand) {
    EncodeSetMMIO<Family>::encodeMEM(container, CS_GPR_R0, firstOperand);
    EncodeSetMMIO<Family>::encodeIMM(container, CS_GPR_R1, secondOperand, true);

    /* CS_GPR_R* registers map to AluRegisters::R_* registers */
    EncodeMath<Family>::greaterThan(container, AluRegisters::R_0,
                                    AluRegisters::R_1, AluRegisters::R_2);

    EncodeSetMMIO<Family>::encodeREG(container, CS_PREDICATE_RESULT, CS_GPR_R2);
}

/*
 * Compute bitwise AND between a register value from regOffset and immVal
 * and store it into dstAddress.
 */
template <typename Family>
void EncodeMathMMIO<Family>::encodeBitwiseAndVal(CommandContainer &container, uint32_t regOffset, uint32_t immVal, uint64_t dstAddress,
                                                 bool workloadPartition) {
    EncodeSetMMIO<Family>::encodeREG(container, CS_GPR_R13, regOffset);
    EncodeSetMMIO<Family>::encodeIMM(container, CS_GPR_R14, immVal, true);
    EncodeMath<Family>::bitwiseAnd(container, AluRegisters::R_13,
                                   AluRegisters::R_14,
                                   AluRegisters::R_15);
    EncodeStoreMMIO<Family>::encode(*container.getCommandStream(),
                                    CS_GPR_R15, dstAddress, workloadPartition);
}

/*
 * encodeAlu() performs operations that leave a state including the result of
 * an operation such as the carry flag, and the accu flag with subtraction and
 * addition result.
 *
 * Parameter "postOperationStateRegister" is the ALU register with the result
 * from the operation that the function caller is interested in obtaining.
 *
 * Parameter "finalResultRegister" is the final destination register where
 * data from "postOperationStateRegister" will be copied.
 */
template <typename Family>
void EncodeMathMMIO<Family>::encodeAlu(MI_MATH_ALU_INST_INLINE *pAluParam, AluRegisters srcA, AluRegisters srcB, AluRegisters op, AluRegisters finalResultRegister, AluRegisters postOperationStateRegister) {
    MI_MATH_ALU_INST_INLINE aluParam;

    aluParam.DW0.Value = 0x0;
    aluParam.DW0.BitField.ALUOpcode = static_cast<uint32_t>(AluRegisters::OPCODE_LOAD);
    aluParam.DW0.BitField.Operand1 = static_cast<uint32_t>(AluRegisters::R_SRCA);
    aluParam.DW0.BitField.Operand2 = static_cast<uint32_t>(srcA);
    *pAluParam = aluParam;
    pAluParam++;

    aluParam.DW0.Value = 0x0;
    aluParam.DW0.BitField.ALUOpcode = static_cast<uint32_t>(AluRegisters::OPCODE_LOAD);
    aluParam.DW0.BitField.Operand1 = static_cast<uint32_t>(AluRegisters::R_SRCB);
    aluParam.DW0.BitField.Operand2 = static_cast<uint32_t>(srcB);
    *pAluParam = aluParam;
    pAluParam++;

    /* Order of operation: Operand1 <ALUOpcode> Operand2 */
    aluParam.DW0.Value = 0x0;
    aluParam.DW0.BitField.ALUOpcode = static_cast<uint32_t>(op);
    aluParam.DW0.BitField.Operand1 = 0;
    aluParam.DW0.BitField.Operand2 = 0;
    *pAluParam = aluParam;
    pAluParam++;

    aluParam.DW0.Value = 0x0;
    aluParam.DW0.BitField.ALUOpcode = static_cast<uint32_t>(AluRegisters::OPCODE_STORE);
    aluParam.DW0.BitField.Operand1 = static_cast<uint32_t>(finalResultRegister);
    aluParam.DW0.BitField.Operand2 = static_cast<uint32_t>(postOperationStateRegister);
    *pAluParam = aluParam;
    pAluParam++;
}

template <typename Family>
uint32_t *EncodeMath<Family>::commandReserve(CommandContainer &container) {
    return commandReserve(*container.getCommandStream());
}

template <typename Family>
uint32_t *EncodeMath<Family>::commandReserve(LinearStream &cmdStream) {
    size_t size = sizeof(MI_MATH) + sizeof(MI_MATH_ALU_INST_INLINE) * NUM_ALU_INST_FOR_READ_MODIFY_WRITE;

    auto cmd = reinterpret_cast<uint32_t *>(cmdStream.getSpace(size));
    MI_MATH mathBuffer;
    mathBuffer.DW0.Value = 0x0;
    mathBuffer.DW0.BitField.InstructionType = MI_MATH::COMMAND_TYPE_MI_COMMAND;
    mathBuffer.DW0.BitField.InstructionOpcode = MI_MATH::MI_COMMAND_OPCODE_MI_MATH;
    mathBuffer.DW0.BitField.DwordLength = NUM_ALU_INST_FOR_READ_MODIFY_WRITE - 1;
    *reinterpret_cast<MI_MATH *>(cmd) = mathBuffer;
    cmd++;

    return cmd;
}

template <typename Family>
void EncodeMathMMIO<Family>::encodeAluAdd(MI_MATH_ALU_INST_INLINE *pAluParam,
                                          AluRegisters firstOperandRegister,
                                          AluRegisters secondOperandRegister,
                                          AluRegisters finalResultRegister) {
    encodeAlu(pAluParam, firstOperandRegister, secondOperandRegister, AluRegisters::OPCODE_ADD, finalResultRegister, AluRegisters::R_ACCU);
}

template <typename Family>
void EncodeMathMMIO<Family>::encodeAluSubStoreCarry(MI_MATH_ALU_INST_INLINE *pAluParam, AluRegisters regA, AluRegisters regB, AluRegisters finalResultRegister) {
    /* regB is subtracted from regA */
    encodeAlu(pAluParam, regA, regB, AluRegisters::OPCODE_SUB, finalResultRegister, AluRegisters::R_CF);
}

template <typename Family>
void EncodeMathMMIO<Family>::encodeAluAnd(MI_MATH_ALU_INST_INLINE *pAluParam,
                                          AluRegisters firstOperandRegister,
                                          AluRegisters secondOperandRegister,
                                          AluRegisters finalResultRegister) {
    encodeAlu(pAluParam, firstOperandRegister, secondOperandRegister, AluRegisters::OPCODE_AND, finalResultRegister, AluRegisters::R_ACCU);
}

template <typename Family>
void EncodeMathMMIO<Family>::encodeIncrementOrDecrement(LinearStream &cmdStream, AluRegisters operandRegister, IncrementOrDecrementOperation operationType) {
    LriHelper<Family>::program(&cmdStream, CS_GPR_R7, 1, true);
    LriHelper<Family>::program(&cmdStream, CS_GPR_R7 + 4, 0, true);

    EncodeAluHelper<Family, 4> aluHelper;
    aluHelper.setNextAlu(AluRegisters::OPCODE_LOAD, AluRegisters::R_SRCA, operandRegister);
    aluHelper.setNextAlu(AluRegisters::OPCODE_LOAD, AluRegisters::R_SRCB, AluRegisters::R_7);
    aluHelper.setNextAlu((operationType == IncrementOrDecrementOperation::Increment) ? AluRegisters::OPCODE_ADD
                                                                                     : AluRegisters::OPCODE_SUB);
    aluHelper.setNextAlu(AluRegisters::OPCODE_STORE, operandRegister, AluRegisters::R_ACCU);

    aluHelper.copyToCmdStream(cmdStream);
}

template <typename Family>
void EncodeMathMMIO<Family>::encodeIncrement(LinearStream &cmdStream, AluRegisters operandRegister) {
    encodeIncrementOrDecrement(cmdStream, operandRegister, IncrementOrDecrementOperation::Increment);
}

template <typename Family>
void EncodeMathMMIO<Family>::encodeDecrement(LinearStream &cmdStream, AluRegisters operandRegister) {
    encodeIncrementOrDecrement(cmdStream, operandRegister, IncrementOrDecrementOperation::Decrement);
}

/*
 * greaterThan() tests if firstOperandRegister is greater than
 * secondOperandRegister.
 */
template <typename Family>
void EncodeMath<Family>::greaterThan(CommandContainer &container,
                                     AluRegisters firstOperandRegister,
                                     AluRegisters secondOperandRegister,
                                     AluRegisters finalResultRegister) {
    uint32_t *cmd = EncodeMath<Family>::commandReserve(container);

    /* firstOperandRegister will be subtracted from secondOperandRegister */
    EncodeMathMMIO<Family>::encodeAluSubStoreCarry(reinterpret_cast<MI_MATH_ALU_INST_INLINE *>(cmd),
                                                   secondOperandRegister,
                                                   firstOperandRegister,
                                                   finalResultRegister);
}

template <typename Family>
void EncodeMath<Family>::addition(CommandContainer &container,
                                  AluRegisters firstOperandRegister,
                                  AluRegisters secondOperandRegister,
                                  AluRegisters finalResultRegister) {
    uint32_t *cmd = EncodeMath<Family>::commandReserve(container);

    EncodeMathMMIO<Family>::encodeAluAdd(reinterpret_cast<MI_MATH_ALU_INST_INLINE *>(cmd),
                                         firstOperandRegister,
                                         secondOperandRegister,
                                         finalResultRegister);
}

template <typename Family>
void EncodeMath<Family>::addition(LinearStream &cmdStream,
                                  AluRegisters firstOperandRegister,
                                  AluRegisters secondOperandRegister,
                                  AluRegisters finalResultRegister) {
    uint32_t *cmd = EncodeMath<Family>::commandReserve(cmdStream);

    EncodeMathMMIO<Family>::encodeAluAdd(reinterpret_cast<MI_MATH_ALU_INST_INLINE *>(cmd),
                                         firstOperandRegister,
                                         secondOperandRegister,
                                         finalResultRegister);
}

template <typename Family>
void EncodeMath<Family>::bitwiseAnd(CommandContainer &container,
                                    AluRegisters firstOperandRegister,
                                    AluRegisters secondOperandRegister,
                                    AluRegisters finalResultRegister) {
    uint32_t *cmd = EncodeMath<Family>::commandReserve(container);

    EncodeMathMMIO<Family>::encodeAluAnd(reinterpret_cast<MI_MATH_ALU_INST_INLINE *>(cmd),
                                         firstOperandRegister,
                                         secondOperandRegister,
                                         finalResultRegister);
}

template <typename Family>
inline void EncodeSetMMIO<Family>::encodeIMM(CommandContainer &container, uint32_t offset, uint32_t data, bool remap) {
    EncodeSetMMIO<Family>::encodeIMM(*container.getCommandStream(), offset, data, remap);
}

template <typename Family>
inline void EncodeSetMMIO<Family>::encodeMEM(CommandContainer &container, uint32_t offset, uint64_t address) {
    EncodeSetMMIO<Family>::encodeMEM(*container.getCommandStream(), offset, address);
}

template <typename Family>
inline void EncodeSetMMIO<Family>::encodeREG(CommandContainer &container, uint32_t dstOffset, uint32_t srcOffset) {
    EncodeSetMMIO<Family>::encodeREG(*container.getCommandStream(), dstOffset, srcOffset);
}

template <typename Family>
inline void EncodeSetMMIO<Family>::encodeIMM(LinearStream &cmdStream, uint32_t offset, uint32_t data, bool remap) {
    LriHelper<Family>::program(&cmdStream,
                               offset,
                               data,
                               remap);
}

template <typename Family>
inline void EncodeStateBaseAddress<Family>::setSbaTrackingForL0DebuggerIfEnabled(bool trackingEnabled,
                                                                                 Device &device,
                                                                                 LinearStream &commandStream,
                                                                                 STATE_BASE_ADDRESS &sbaCmd, bool useFirstLevelBB) {
    if (!trackingEnabled) {
        return;
    }
    NEO::Debugger::SbaAddresses sbaAddresses = {};
    NEO::EncodeStateBaseAddress<Family>::setSbaAddressesForDebugger(sbaAddresses, sbaCmd);
    device.getL0Debugger()->captureStateBaseAddress(commandStream, sbaAddresses, useFirstLevelBB);
}

template <typename Family>
void EncodeSetMMIO<Family>::encodeMEM(LinearStream &cmdStream, uint32_t offset, uint64_t address) {
    MI_LOAD_REGISTER_MEM cmd = Family::cmdInitLoadRegisterMem;
    cmd.setRegisterAddress(offset);
    cmd.setMemoryAddress(address);
    remapOffset(&cmd);

    auto buffer = cmdStream.getSpaceForCmd<MI_LOAD_REGISTER_MEM>();
    *buffer = cmd;
}

template <typename Family>
void EncodeSetMMIO<Family>::encodeREG(LinearStream &cmdStream, uint32_t dstOffset, uint32_t srcOffset) {
    MI_LOAD_REGISTER_REG cmd = Family::cmdInitLoadRegisterReg;
    cmd.setSourceRegisterAddress(srcOffset);
    cmd.setDestinationRegisterAddress(dstOffset);
    remapOffset(&cmd);
    auto buffer = cmdStream.getSpaceForCmd<MI_LOAD_REGISTER_REG>();
    *buffer = cmd;
}

template <typename Family>
void EncodeStoreMMIO<Family>::encode(LinearStream &csr, uint32_t offset, uint64_t address, bool workloadPartition) {
    auto buffer = csr.getSpaceForCmd<MI_STORE_REGISTER_MEM>();
    EncodeStoreMMIO<Family>::encode(buffer, offset, address, workloadPartition);
}

template <typename Family>
inline void EncodeStoreMMIO<Family>::encode(MI_STORE_REGISTER_MEM *cmdBuffer, uint32_t offset, uint64_t address, bool workloadPartition) {
    MI_STORE_REGISTER_MEM cmd = Family::cmdInitStoreRegisterMem;
    cmd.setRegisterAddress(offset);
    cmd.setMemoryAddress(address);
    appendFlags(&cmd, workloadPartition);
    *cmdBuffer = cmd;
}

template <typename Family>
void EncodeSurfaceState<Family>::encodeBuffer(EncodeSurfaceStateArgs &args) {
    auto surfaceState = reinterpret_cast<R_SURFACE_STATE *>(args.outMemory);
    auto bufferSize = alignUp(args.size, getSurfaceBaseAddressAlignment());

    SURFACE_STATE_BUFFER_LENGTH length = {0};
    length.length = static_cast<uint32_t>(bufferSize - 1);

    surfaceState->setWidth(length.surfaceState.width + 1);
    surfaceState->setHeight(length.surfaceState.height + 1);
    surfaceState->setDepth(length.surfaceState.depth + 1);

    surfaceState->setSurfaceType((args.graphicsAddress != 0) ? R_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_BUFFER
                                                             : R_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_NULL);
    surfaceState->setSurfaceFormat(SURFACE_FORMAT::SURFACE_FORMAT_RAW);
    surfaceState->setSurfaceVerticalAlignment(R_SURFACE_STATE::SURFACE_VERTICAL_ALIGNMENT_VALIGN_4);
    surfaceState->setSurfaceHorizontalAlignment(R_SURFACE_STATE::SURFACE_HORIZONTAL_ALIGNMENT_HALIGN_DEFAULT);

    surfaceState->setTileMode(R_SURFACE_STATE::TILE_MODE_LINEAR);
    surfaceState->setVerticalLineStride(0);
    surfaceState->setVerticalLineStrideOffset(0);
    surfaceState->setMemoryObjectControlState(args.mocs);
    surfaceState->setSurfaceBaseAddress(args.graphicsAddress);

    surfaceState->setAuxiliarySurfaceMode(AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_NONE);

    setCoherencyType(surfaceState, args.cpuCoherent ? R_SURFACE_STATE::COHERENCY_TYPE_IA_COHERENT : R_SURFACE_STATE::COHERENCY_TYPE_GPU_COHERENT);

    auto compressionEnabled = args.allocation ? args.allocation->isCompressionEnabled() : false;
    if (compressionEnabled && !args.forceNonAuxMode) {
        // Its expected to not program pitch/qpitch/baseAddress for Aux surface in CCS scenarios
        setCoherencyType(surfaceState, R_SURFACE_STATE::COHERENCY_TYPE_GPU_COHERENT);
        setBufferAuxParamsForCCS(surfaceState);
    }

    if (DebugManager.flags.DisableCachingForStatefulBufferAccess.get()) {
        surfaceState->setMemoryObjectControlState(args.gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER_CACHELINE_MISALIGNED));
    }

    EncodeSurfaceState<Family>::encodeExtraBufferParams(args);

    EncodeSurfaceState<Family>::appendBufferSurfaceState(args);
}

template <typename Family>
void EncodeSurfaceState<Family>::getSshAlignedPointer(uintptr_t &ptr, size_t &offset) {
    auto sshAlignmentMask =
        getSurfaceBaseAddressAlignmentMask();
    uintptr_t alignedPtr = ptr & sshAlignmentMask;

    offset = 0;
    if (ptr != alignedPtr) {
        offset = ptrDiff(ptr, alignedPtr);
        ptr = alignedPtr;
    }
}

// Returned binding table pointer is relative to given heap (which is assumed to be the Surface state base addess)
// as required by the INTERFACE_DESCRIPTOR_DATA.
template <typename Family>
size_t EncodeSurfaceState<Family>::pushBindingTableAndSurfaceStates(IndirectHeap &dstHeap,
                                                                    const void *srcKernelSsh, size_t srcKernelSshSize,
                                                                    size_t numberOfBindingTableStates, size_t offsetOfBindingTable) {
    using BINDING_TABLE_STATE = typename Family::BINDING_TABLE_STATE;
    using INTERFACE_DESCRIPTOR_DATA = typename Family::INTERFACE_DESCRIPTOR_DATA;
    using RENDER_SURFACE_STATE = typename Family::RENDER_SURFACE_STATE;

    size_t sshSize = srcKernelSshSize;
    DEBUG_BREAK_IF(srcKernelSsh == nullptr);

    auto srcSurfaceState = srcKernelSsh;
    // Allocate space for new ssh data
    auto dstSurfaceState = dstHeap.getSpace(sshSize);

    // Compiler sends BTI table that is already populated with surface state pointers relative to local SSH.
    // We may need to patch these pointers so that they are relative to surface state base address
    if (dstSurfaceState == dstHeap.getCpuBase()) {
        // nothing to patch, we're at the start of heap (which is assumed to be the surface state base address)
        // we need to simply copy the ssh (including BTIs from compiler)
        memcpy_s(dstSurfaceState, sshSize, srcSurfaceState, sshSize);
        return offsetOfBindingTable;
    }

    // We can copy-over the surface states, but BTIs will need to be patched
    memcpy_s(dstSurfaceState, sshSize, srcSurfaceState, offsetOfBindingTable);

    uint32_t surfaceStatesOffset = static_cast<uint32_t>(ptrDiff(dstSurfaceState, dstHeap.getCpuBase()));

    // march over BTIs and offset the pointers based on surface state base address
    auto *dstBtiTableBase = reinterpret_cast<BINDING_TABLE_STATE *>(ptrOffset(dstSurfaceState, offsetOfBindingTable));
    DEBUG_BREAK_IF(reinterpret_cast<uintptr_t>(dstBtiTableBase) % INTERFACE_DESCRIPTOR_DATA::BINDINGTABLEPOINTER_ALIGN_SIZE != 0);
    auto *srcBtiTableBase = reinterpret_cast<const BINDING_TABLE_STATE *>(ptrOffset(srcSurfaceState, offsetOfBindingTable));
    BINDING_TABLE_STATE bti = Family::cmdInitBindingTableState;
    for (uint32_t i = 0, e = static_cast<uint32_t>(numberOfBindingTableStates); i != e; ++i) {
        uint32_t localSurfaceStateOffset = srcBtiTableBase[i].getSurfaceStatePointer();
        uint32_t offsetedSurfaceStateOffset = localSurfaceStateOffset + surfaceStatesOffset;
        bti.setSurfaceStatePointer(offsetedSurfaceStateOffset); // patch just the SurfaceStatePointer bits
        dstBtiTableBase[i] = bti;
        DEBUG_BREAK_IF(bti.getRawData(0) % sizeof(BINDING_TABLE_STATE::SURFACESTATEPOINTER_ALIGN_SIZE) != 0);
    }

    return ptrDiff(dstBtiTableBase, dstHeap.getCpuBase());
}

template <typename Family>
inline void EncodeSurfaceState<Family>::encodeExtraCacheSettings(R_SURFACE_STATE *surfaceState, const EncodeSurfaceStateArgs &args) {}

template <typename Family>
void EncodeSurfaceState<Family>::setImageAuxParamsForCCS(R_SURFACE_STATE *surfaceState, Gmm *gmm) {
    using AUXILIARY_SURFACE_MODE = typename Family::RENDER_SURFACE_STATE::AUXILIARY_SURFACE_MODE;
    // Its expected to not program pitch/qpitch/baseAddress for Aux surface in CCS scenarios
    surfaceState->setAuxiliarySurfaceMode(AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_CCS_E);
    setFlagsForMediaCompression(surfaceState, gmm);

    setClearColorParams(surfaceState, gmm);
    setUnifiedAuxBaseAddress<Family>(surfaceState, gmm);
}

template <typename Family>
void EncodeSurfaceState<Family>::setBufferAuxParamsForCCS(R_SURFACE_STATE *surfaceState) {
    using AUXILIARY_SURFACE_MODE = typename R_SURFACE_STATE::AUXILIARY_SURFACE_MODE;

    surfaceState->setAuxiliarySurfaceMode(AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_CCS_E);
}

template <typename Family>
bool EncodeSurfaceState<Family>::isAuxModeEnabled(R_SURFACE_STATE *surfaceState, Gmm *gmm) {
    using AUXILIARY_SURFACE_MODE = typename R_SURFACE_STATE::AUXILIARY_SURFACE_MODE;

    return (surfaceState->getAuxiliarySurfaceMode() == AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_CCS_E);
}

template <typename Family>
void EncodeSurfaceState<Family>::appendParamsForImageFromBuffer(R_SURFACE_STATE *surfaceState) {
}

template <typename Family>
void EncodeSurfaceState<Family>::encodeImplicitScalingParams(const EncodeSurfaceStateArgs &args) {}

template <typename Family>
void *EncodeDispatchKernel<Family>::getInterfaceDescriptor(CommandContainer &container, IndirectHeap *childDsh, uint32_t &iddOffset) {

    if (container.nextIddInBlockRef() == container.getNumIddPerBlock()) {
        if (ApiSpecificConfig::getBindlessConfiguration()) {
            container.getDevice()->getBindlessHeapsHelper()->getHeap(BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH)->align(EncodeStates<Family>::alignInterfaceDescriptorData);
            container.setIddBlock(container.getDevice()->getBindlessHeapsHelper()->getSpaceInHeap(sizeof(INTERFACE_DESCRIPTOR_DATA) * container.getNumIddPerBlock(), BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH));
        } else {
            void *heapPointer = nullptr;
            size_t heapSize = sizeof(INTERFACE_DESCRIPTOR_DATA) * container.getNumIddPerBlock();
            if (childDsh != nullptr) {
                childDsh->align(EncodeStates<Family>::alignInterfaceDescriptorData);
                heapPointer = childDsh->getSpace(heapSize);
            } else {
                container.getIndirectHeap(HeapType::DYNAMIC_STATE)->align(EncodeStates<Family>::alignInterfaceDescriptorData);
                heapPointer = container.getHeapSpaceAllowGrow(HeapType::DYNAMIC_STATE, heapSize);
            }
            container.setIddBlock(heapPointer);
        }
        container.nextIddInBlockRef() = 0;
    }

    iddOffset = container.nextIddInBlockRef();
    auto interfaceDescriptorData = static_cast<INTERFACE_DESCRIPTOR_DATA *>(container.getIddBlock());
    container.nextIddInBlockRef()++;
    return &interfaceDescriptorData[iddOffset];
}

template <typename Family>
bool EncodeDispatchKernel<Family>::inlineDataProgrammingRequired(const KernelDescriptor &kernelDesc) {
    auto checkKernelForInlineData = true;
    if (DebugManager.flags.EnablePassInlineData.get() != -1) {
        checkKernelForInlineData = !!DebugManager.flags.EnablePassInlineData.get();
    }
    if (checkKernelForInlineData) {
        return kernelDesc.kernelAttributes.flags.passInlineData;
    }
    return false;
}

template <typename Family>
void EncodeDispatchKernel<Family>::adjustTimestampPacket(WALKER_TYPE &walkerCmd, const HardwareInfo &hwInfo) {}

template <typename Family>
void EncodeIndirectParams<Family>::encode(CommandContainer &container, uint64_t crossThreadDataGpuVa, DispatchKernelEncoderI *dispatchInterface, uint64_t implicitArgsGpuPtr) {
    const auto &kernelDescriptor = dispatchInterface->getKernelDescriptor();
    setGroupCountIndirect(container, kernelDescriptor.payloadMappings.dispatchTraits.numWorkGroups, crossThreadDataGpuVa);
    setGlobalWorkSizeIndirect(container, kernelDescriptor.payloadMappings.dispatchTraits.globalWorkSize, crossThreadDataGpuVa, dispatchInterface->getGroupSize());
    UNRECOVERABLE_IF(NEO::isValidOffset(kernelDescriptor.payloadMappings.dispatchTraits.workDim) && (kernelDescriptor.payloadMappings.dispatchTraits.workDim & 0b11) != 0u);
    setWorkDimIndirect(container, kernelDescriptor.payloadMappings.dispatchTraits.workDim, crossThreadDataGpuVa, dispatchInterface->getGroupSize());
    if (implicitArgsGpuPtr) {
        CrossThreadDataOffset groupCountOffset[] = {offsetof(ImplicitArgs, groupCountX), offsetof(ImplicitArgs, groupCountY), offsetof(ImplicitArgs, groupCountZ)};
        CrossThreadDataOffset globalSizeOffset[] = {offsetof(ImplicitArgs, globalSizeX), offsetof(ImplicitArgs, globalSizeY), offsetof(ImplicitArgs, globalSizeZ)};
        setGroupCountIndirect(container, groupCountOffset, implicitArgsGpuPtr);
        setGlobalWorkSizeIndirect(container, globalSizeOffset, implicitArgsGpuPtr, dispatchInterface->getGroupSize());
        setWorkDimIndirect(container, offsetof(ImplicitArgs, numWorkDim), implicitArgsGpuPtr, dispatchInterface->getGroupSize());
    }
}

template <typename Family>
void EncodeIndirectParams<Family>::setGroupCountIndirect(CommandContainer &container, const NEO::CrossThreadDataOffset offsets[3], uint64_t crossThreadAddress) {
    for (int i = 0; i < 3; ++i) {
        if (NEO::isUndefinedOffset(offsets[i])) {
            continue;
        }
        EncodeStoreMMIO<Family>::encode(*container.getCommandStream(), GPUGPU_DISPATCHDIM[i], ptrOffset(crossThreadAddress, offsets[i]), false);
    }
}

template <typename Family>
void EncodeIndirectParams<Family>::setWorkDimIndirect(CommandContainer &container, const NEO::CrossThreadDataOffset workDimOffset, uint64_t crossThreadAddress, const uint32_t *groupSize) {
    if (NEO::isValidOffset(workDimOffset)) {
        auto dstPtr = ptrOffset(crossThreadAddress, workDimOffset);
        constexpr uint32_t resultRegister = CS_GPR_R0;
        constexpr AluRegisters resultAluRegister = AluRegisters::R_0;
        const uint32_t offset = static_cast<uint32_t>((1ull << 8 * (dstPtr & 0b11)) - 1);
        const uint32_t memoryMask = std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>((1ull << 8 * ((dstPtr & 0b11) + 1)) - 1) + offset;

        /*
         * if ( groupSize[2] > 1 || groupCount[2] > 1 ) { workdim = 3 }
         * else if ( groupSize[1] + groupCount[1] > 2 ) { workdim = 2 }
         * else { workdim = 1 }
         */

        if (groupSize[2] > 1) {
            EncodeSetMMIO<Family>::encodeIMM(container, resultRegister, 3 << (8 * (dstPtr & 0b11)), true);
        } else {

            constexpr uint32_t groupCount2Register = CS_GPR_R1;
            constexpr AluRegisters groupCount2AluRegister = AluRegisters::R_1;

            constexpr uint32_t groupSize1Register = CS_GPR_R0;
            constexpr AluRegisters groupSize1AluRegister = AluRegisters::R_0;

            constexpr uint32_t groupCount1Register = CS_GPR_R1;
            constexpr AluRegisters groupCount1AluRegister = AluRegisters::R_1;

            constexpr AluRegisters sumAluRegister = AluRegisters::R_0;

            constexpr AluRegisters workDimEq3AluRegister = AluRegisters::R_3;

            constexpr AluRegisters workDimGe2AluRegister = AluRegisters::R_4;

            constexpr uint32_t constantOneRegister = CS_GPR_R5;
            constexpr AluRegisters constantOneAluRegister = AluRegisters::R_5;
            constexpr uint32_t constantTwoRegister = CS_GPR_R6;
            constexpr AluRegisters constantTwoAluRegister = AluRegisters::R_6;

            constexpr uint32_t backupRegister = CS_GPR_R7;
            constexpr AluRegisters backupAluRegister = AluRegisters::R_7;

            constexpr uint32_t memoryMaskRegister = CS_GPR_R8;
            constexpr AluRegisters memoryMaskAluRegister = AluRegisters::R_8;

            constexpr uint32_t offsetRegister = CS_GPR_R8;
            constexpr AluRegisters offsetAluRegister = AluRegisters::R_8;

            if (offset) {
                EncodeSetMMIO<Family>::encodeMEM(container, backupRegister, dstPtr);
                EncodeSetMMIO<Family>::encodeIMM(container, memoryMaskRegister, memoryMask, true);
                EncodeMath<Family>::bitwiseAnd(container, memoryMaskAluRegister, backupAluRegister, backupAluRegister);
                EncodeSetMMIO<Family>::encodeIMM(container, offsetRegister, offset, true);
            }

            EncodeSetMMIO<Family>::encodeIMM(container, constantOneRegister, 1, true);
            EncodeSetMMIO<Family>::encodeIMM(container, constantTwoRegister, 2, true);

            EncodeSetMMIO<Family>::encodeREG(container, groupCount2Register, GPUGPU_DISPATCHDIM[2]);

            EncodeMath<Family>::greaterThan(container, groupCount2AluRegister, constantOneAluRegister, workDimEq3AluRegister);
            EncodeMath<Family>::bitwiseAnd(container, workDimEq3AluRegister, constantOneAluRegister, workDimEq3AluRegister);

            EncodeSetMMIO<Family>::encodeIMM(container, groupSize1Register, groupSize[1], true);
            EncodeSetMMIO<Family>::encodeREG(container, groupCount1Register, GPUGPU_DISPATCHDIM[1]);

            EncodeMath<Family>::addition(container, groupSize1AluRegister, groupCount1AluRegister, sumAluRegister);
            EncodeMath<Family>::addition(container, sumAluRegister, workDimEq3AluRegister, sumAluRegister);
            EncodeMath<Family>::greaterThan(container, sumAluRegister, constantTwoAluRegister, workDimGe2AluRegister);
            EncodeMath<Family>::bitwiseAnd(container, workDimGe2AluRegister, constantOneAluRegister, workDimGe2AluRegister);

            if (offset) {
                EncodeMath<Family>::addition(container, constantOneAluRegister, offsetAluRegister, constantOneAluRegister);
                EncodeMath<Family>::addition(container, workDimEq3AluRegister, offsetAluRegister, workDimEq3AluRegister);
                EncodeMath<Family>::bitwiseAnd(container, workDimEq3AluRegister, constantOneAluRegister, workDimEq3AluRegister);
                EncodeMath<Family>::addition(container, workDimGe2AluRegister, offsetAluRegister, workDimGe2AluRegister);
                EncodeMath<Family>::bitwiseAnd(container, workDimGe2AluRegister, constantOneAluRegister, workDimGe2AluRegister);
            }

            EncodeSetMMIO<Family>::encodeREG(container, resultRegister, constantOneRegister);
            EncodeMath<Family>::addition(container, resultAluRegister, workDimGe2AluRegister, resultAluRegister);
            EncodeMath<Family>::addition(container, resultAluRegister, workDimEq3AluRegister, resultAluRegister);

            if (offset) {
                EncodeMath<Family>::addition(container, resultAluRegister, backupAluRegister, resultAluRegister);
            }
        }
        EncodeStoreMMIO<Family>::encode(*container.getCommandStream(), resultRegister, dstPtr, false);
    }
}

template <typename Family>
bool EncodeSurfaceState<Family>::doBindingTablePrefetch() {
    auto enableBindingTablePrefetech = isBindingTablePrefetchPreferred();
    if (DebugManager.flags.ForceBtpPrefetchMode.get() != -1) {
        enableBindingTablePrefetech = static_cast<bool>(DebugManager.flags.ForceBtpPrefetchMode.get());
    }
    return enableBindingTablePrefetech;
}

template <typename Family>
void EncodeDispatchKernel<Family>::adjustBindingTablePrefetch(INTERFACE_DESCRIPTOR_DATA &interfaceDescriptor, uint32_t samplerCount, uint32_t bindingTableEntryCount) {
    auto enablePrefetch = EncodeSurfaceState<Family>::doBindingTablePrefetch();

    if (enablePrefetch) {
        interfaceDescriptor.setSamplerCount(static_cast<typename INTERFACE_DESCRIPTOR_DATA::SAMPLER_COUNT>((samplerCount + 3) / 4));
        interfaceDescriptor.setBindingTableEntryCount(std::min(bindingTableEntryCount, 31u));
    } else {
        interfaceDescriptor.setSamplerCount(INTERFACE_DESCRIPTOR_DATA::SAMPLER_COUNT::SAMPLER_COUNT_NO_SAMPLERS_USED);
        interfaceDescriptor.setBindingTableEntryCount(0u);
    }
}

template <typename Family>
void EncodeDispatchKernel<Family>::adjustInterfaceDescriptorData(INTERFACE_DESCRIPTOR_DATA &interfaceDescriptor, const Device &device, const HardwareInfo &hwInfo, const uint32_t threadGroupCount, const uint32_t numGrf, WALKER_TYPE &walkerCmd) {}

template <typename Family>
size_t EncodeDispatchKernel<Family>::getSizeRequiredDsh(const KernelDescriptor &kernelDescriptor, uint32_t iddCount) {
    using INTERFACE_DESCRIPTOR_DATA = typename Family::INTERFACE_DESCRIPTOR_DATA;
    constexpr auto samplerStateSize = sizeof(typename Family::SAMPLER_STATE);
    const auto numSamplers = kernelDescriptor.payloadMappings.samplerTable.numSamplers;
    const auto additionalDshSize = additionalSizeRequiredDsh(iddCount);
    if (numSamplers == 0U) {
        return alignUp(additionalDshSize, EncodeDispatchKernel<Family>::getDefaultDshAlignment());
    }

    size_t size = kernelDescriptor.payloadMappings.samplerTable.tableOffset -
                  kernelDescriptor.payloadMappings.samplerTable.borderColor;
    size = alignUp(size, EncodeDispatchKernel<Family>::getDefaultDshAlignment());

    size += numSamplers * samplerStateSize;
    size = alignUp(size, INTERFACE_DESCRIPTOR_DATA::SAMPLERSTATEPOINTER_ALIGN_SIZE);

    if (additionalDshSize > 0) {
        size += additionalDshSize;
        size = alignUp(size, EncodeDispatchKernel<Family>::getDefaultDshAlignment());
    }

    return size;
}

template <typename Family>
size_t EncodeDispatchKernel<Family>::getSizeRequiredSsh(const KernelInfo &kernelInfo) {
    size_t requiredSshSize = kernelInfo.heapInfo.surfaceStateHeapSize;
    requiredSshSize = alignUp(requiredSshSize, EncodeDispatchKernel<Family>::getDefaultSshAlignment());
    return requiredSshSize;
}

template <typename Family>
size_t EncodeDispatchKernel<Family>::getDefaultDshAlignment() {
    return EncodeStates<Family>::alignIndirectStatePointer;
}

template <typename Family>
void EncodeIndirectParams<Family>::setGlobalWorkSizeIndirect(CommandContainer &container, const NEO::CrossThreadDataOffset offsets[3], uint64_t crossThreadAddress, const uint32_t *lws) {
    for (int i = 0; i < 3; ++i) {
        if (NEO::isUndefinedOffset(offsets[i])) {
            continue;
        }
        EncodeMathMMIO<Family>::encodeMulRegVal(container, GPUGPU_DISPATCHDIM[i], lws[i], ptrOffset(crossThreadAddress, offsets[i]));
    }
}

template <typename Family>
inline size_t EncodeIndirectParams<Family>::getCmdsSizeForSetWorkDimIndirect(const uint32_t *groupSize, bool misaligedPtr) {
    constexpr uint32_t aluCmdSize = sizeof(MI_MATH) + sizeof(MI_MATH_ALU_INST_INLINE) * NUM_ALU_INST_FOR_READ_MODIFY_WRITE;
    auto requiredSize = sizeof(MI_STORE_REGISTER_MEM) + sizeof(MI_LOAD_REGISTER_IMM);
    UNRECOVERABLE_IF(!groupSize);
    if (groupSize[2] < 2) {
        requiredSize += 2 * sizeof(MI_LOAD_REGISTER_IMM) + 3 * sizeof(MI_LOAD_REGISTER_REG) + 8 * aluCmdSize;
        if (misaligedPtr) {
            requiredSize += 2 * sizeof(MI_LOAD_REGISTER_IMM) + sizeof(MI_LOAD_REGISTER_MEM) + 7 * aluCmdSize;
        }
    }
    return requiredSize;
}
template <typename Family>
void EncodeSemaphore<Family>::addMiSemaphoreWaitCommand(LinearStream &commandStream,
                                                        uint64_t compareAddress,
                                                        uint32_t compareData,
                                                        COMPARE_OPERATION compareMode) {
    addMiSemaphoreWaitCommand(commandStream, compareAddress, compareData, compareMode, false);
}

template <typename Family>
void EncodeSemaphore<Family>::addMiSemaphoreWaitCommand(LinearStream &commandStream,
                                                        uint64_t compareAddress,
                                                        uint32_t compareData,
                                                        COMPARE_OPERATION compareMode,
                                                        bool registerPollMode) {
    auto semaphoreCommand = commandStream.getSpaceForCmd<MI_SEMAPHORE_WAIT>();
    programMiSemaphoreWait(semaphoreCommand,
                           compareAddress,
                           compareData,
                           compareMode,
                           registerPollMode,
                           true);
}
template <typename Family>
void EncodeSemaphore<Family>::applyMiSemaphoreWaitCommand(LinearStream &commandStream, std::list<void *> &commandsList) {
    MI_SEMAPHORE_WAIT *semaphoreCommand = commandStream.getSpaceForCmd<MI_SEMAPHORE_WAIT>();
    commandsList.push_back(semaphoreCommand);
}

template <typename Family>
inline void EncodeAtomic<Family>::setMiAtomicAddress(MI_ATOMIC &atomic, uint64_t writeAddress) {
    atomic.setMemoryAddress(static_cast<uint32_t>(writeAddress & 0x0000FFFFFFFFULL));
    atomic.setMemoryAddressHigh(static_cast<uint32_t>(writeAddress >> 32));
}

template <typename Family>
void EncodeAtomic<Family>::programMiAtomic(MI_ATOMIC *atomic,
                                           uint64_t writeAddress,
                                           ATOMIC_OPCODES opcode,
                                           DATA_SIZE dataSize,
                                           uint32_t returnDataControl,
                                           uint32_t csStall,
                                           uint32_t operand1dword0,
                                           uint32_t operand1dword1) {
    MI_ATOMIC cmd = Family::cmdInitAtomic;
    cmd.setAtomicOpcode(opcode);
    cmd.setDataSize(dataSize);
    EncodeAtomic<Family>::setMiAtomicAddress(cmd, writeAddress);
    cmd.setReturnDataControl(returnDataControl);
    cmd.setCsStall(csStall);
    if (opcode == ATOMIC_OPCODES::ATOMIC_4B_MOVE ||
        opcode == ATOMIC_OPCODES::ATOMIC_8B_MOVE) {
        cmd.setDwordLength(MI_ATOMIC::DWORD_LENGTH::DWORD_LENGTH_INLINE_DATA_1);
        cmd.setInlineData(0x1);
        cmd.setOperand1DataDword0(operand1dword0);
        cmd.setOperand1DataDword1(operand1dword1);
    }

    *atomic = cmd;
}

template <typename Family>
void EncodeAtomic<Family>::programMiAtomic(LinearStream &commandStream,
                                           uint64_t writeAddress,
                                           ATOMIC_OPCODES opcode,
                                           DATA_SIZE dataSize,
                                           uint32_t returnDataControl,
                                           uint32_t csStall,
                                           uint32_t operand1dword0,
                                           uint32_t operand1dword1) {
    auto miAtomic = commandStream.getSpaceForCmd<MI_ATOMIC>();
    EncodeAtomic<Family>::programMiAtomic(miAtomic, writeAddress, opcode, dataSize, returnDataControl, csStall, operand1dword0, operand1dword1);
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programConditionalDataMemBatchBufferStart(LinearStream &commandStream, uint64_t startAddress, uint64_t compareAddress,
                                                                                    uint32_t compareData, CompareOperation compareOperation, bool indirect) {
    EncodeSetMMIO<Family>::encodeMEM(commandStream, CS_GPR_R7, compareAddress);
    LriHelper<Family>::program(&commandStream, CS_GPR_R7 + 4, 0, true);

    LriHelper<Family>::program(&commandStream, CS_GPR_R8, compareData, true);
    LriHelper<Family>::program(&commandStream, CS_GPR_R8 + 4, 0, true);

    programConditionalBatchBufferStartBase(commandStream, startAddress, AluRegisters::R_7, AluRegisters::R_8, compareOperation, indirect);
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programConditionalDataRegBatchBufferStart(LinearStream &commandStream, uint64_t startAddress, uint32_t compareReg,
                                                                                    uint32_t compareData, CompareOperation compareOperation, bool indirect) {
    EncodeSetMMIO<Family>::encodeREG(commandStream, CS_GPR_R7, compareReg);
    LriHelper<Family>::program(&commandStream, CS_GPR_R7 + 4, 0, true);

    LriHelper<Family>::program(&commandStream, CS_GPR_R8, compareData, true);
    LriHelper<Family>::program(&commandStream, CS_GPR_R8 + 4, 0, true);

    programConditionalBatchBufferStartBase(commandStream, startAddress, AluRegisters::R_7, AluRegisters::R_8, compareOperation, indirect);
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programConditionalRegRegBatchBufferStart(LinearStream &commandStream, uint64_t startAddress, AluRegisters compareReg0,
                                                                                   AluRegisters compareReg1, CompareOperation compareOperation, bool indirect) {

    programConditionalBatchBufferStartBase(commandStream, startAddress, compareReg0, compareReg1, compareOperation, indirect);
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programConditionalRegMemBatchBufferStart(LinearStream &commandStream, uint64_t startAddress, uint64_t compareAddress, uint32_t compareReg,
                                                                                   CompareOperation compareOperation, bool indirect) {
    EncodeSetMMIO<Family>::encodeMEM(commandStream, CS_GPR_R7, compareAddress);
    LriHelper<Family>::program(&commandStream, CS_GPR_R7 + 4, 0, true);

    EncodeSetMMIO<Family>::encodeREG(commandStream, CS_GPR_R8, compareReg);
    LriHelper<Family>::program(&commandStream, CS_GPR_R8 + 4, 0, true);

    programConditionalBatchBufferStartBase(commandStream, startAddress, AluRegisters::R_7, AluRegisters::R_8, compareOperation, indirect);
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programConditionalBatchBufferStartBase(LinearStream &commandStream, uint64_t startAddress, AluRegisters regA, AluRegisters regB,
                                                                                 CompareOperation compareOperation, bool indirect) {
    EncodeAluHelper<Family, 4> aluHelper;
    aluHelper.setNextAlu(AluRegisters::OPCODE_LOAD, AluRegisters::R_SRCA, regA);
    aluHelper.setNextAlu(AluRegisters::OPCODE_LOAD, AluRegisters::R_SRCB, regB);
    aluHelper.setNextAlu(AluRegisters::OPCODE_SUB);

    if ((compareOperation == CompareOperation::Equal) || (compareOperation == CompareOperation::NotEqual)) {
        aluHelper.setNextAlu(AluRegisters::OPCODE_STORE, AluRegisters::R_7, AluRegisters::R_ZF);
    } else if ((compareOperation == CompareOperation::GreaterOrEqual) || (compareOperation == CompareOperation::Less)) {
        aluHelper.setNextAlu(AluRegisters::OPCODE_STORE, AluRegisters::R_7, AluRegisters::R_CF);
    } else {
        UNRECOVERABLE_IF(true);
    }

    aluHelper.copyToCmdStream(commandStream);

    EncodeSetMMIO<Family>::encodeREG(commandStream, CS_PREDICATE_RESULT_2, CS_GPR_R7);

    MiPredicateType predicateType = MiPredicateType::NoopOnResult2Clear; // Equal or Less
    if ((compareOperation == CompareOperation::NotEqual) || (compareOperation == CompareOperation::GreaterOrEqual)) {
        predicateType = MiPredicateType::NoopOnResult2Set;
    }

    EncodeMiPredicate<Family>::encode(commandStream, predicateType);

    programBatchBufferStart(&commandStream, startAddress, false, indirect, true);

    EncodeMiPredicate<Family>::encode(commandStream, MiPredicateType::Disable);
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programBatchBufferStart(MI_BATCH_BUFFER_START *cmdBuffer, uint64_t address, bool secondLevel, bool indirect, bool predicate) {
    MI_BATCH_BUFFER_START cmd = Family::cmdInitBatchBufferStart;
    if (secondLevel) {
        cmd.setSecondLevelBatchBuffer(MI_BATCH_BUFFER_START::SECOND_LEVEL_BATCH_BUFFER_SECOND_LEVEL_BATCH);
    }
    cmd.setAddressSpaceIndicator(MI_BATCH_BUFFER_START::ADDRESS_SPACE_INDICATOR_PPGTT);
    cmd.setBatchBufferStartAddress(address);

    appendBatchBufferStart(cmd, indirect, predicate);

    *cmdBuffer = cmd;
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programBatchBufferStart(LinearStream *commandStream, uint64_t address, bool secondLevel, bool indirect, bool predicate) {
    programBatchBufferStart(commandStream->getSpaceForCmd<MI_BATCH_BUFFER_START>(), address, secondLevel, indirect, predicate);
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::appendBatchBufferStart(MI_BATCH_BUFFER_START &cmd, bool indirect, bool predicate) {
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programBatchBufferEnd(LinearStream &commandStream) {
    MI_BATCH_BUFFER_END cmd = Family::cmdInitBatchBufferEnd;
    auto buffer = commandStream.getSpaceForCmd<MI_BATCH_BUFFER_END>();
    *buffer = cmd;
}

template <typename Family>
void EncodeBatchBufferStartOrEnd<Family>::programBatchBufferEnd(CommandContainer &container) {
    programBatchBufferEnd(*container.getCommandStream());
}

template <typename GfxFamily>
void EncodeMiFlushDW<GfxFamily>::appendWa(LinearStream &commandStream, MiFlushArgs &args) {
    BlitCommandsHelper<GfxFamily>::dispatchDummyBlit(commandStream, args.waArgs);
}

template <typename Family>
void EncodeMiFlushDW<Family>::programWithWa(LinearStream &commandStream, uint64_t immediateDataGpuAddress, uint64_t immediateData,
                                            MiFlushArgs &args) {
    appendWa(commandStream, args);

    auto miFlushDwCmd = commandStream.getSpaceForCmd<MI_FLUSH_DW>();
    MI_FLUSH_DW miFlush = Family::cmdInitMiFlushDw;
    if (args.commandWithPostSync) {
        auto postSyncType = args.timeStampOperation ? MI_FLUSH_DW::POST_SYNC_OPERATION_WRITE_TIMESTAMP_REGISTER : MI_FLUSH_DW::POST_SYNC_OPERATION_WRITE_IMMEDIATE_DATA_QWORD;
        miFlush.setPostSyncOperation(postSyncType);
        miFlush.setDestinationAddress(immediateDataGpuAddress);
        miFlush.setImmediateData(immediateData);
    }
    miFlush.setNotifyEnable(args.notifyEnable);
    miFlush.setTlbInvalidate(args.tlbFlush);
    adjust(&miFlush, args.waArgs.rootDeviceEnvironment->getProductHelper());
    *miFlushDwCmd = miFlush;
}

template <typename Family>
size_t EncodeMiFlushDW<Family>::getWaSize(const EncodeDummyBlitWaArgs &waArgs) {
    return BlitCommandsHelper<Family>::getDummyBlitSize(waArgs);
}

template <typename Family>
size_t EncodeMiFlushDW<Family>::getCommandSizeWithWa(const EncodeDummyBlitWaArgs &waArgs) {
    return sizeof(typename Family::MI_FLUSH_DW) + EncodeMiFlushDW<Family>::getWaSize(waArgs);
}

template <typename Family>
inline void EncodeMemoryPrefetch<Family>::programMemoryPrefetch(LinearStream &commandStream, const GraphicsAllocation &graphicsAllocation, uint32_t size, size_t offset, const RootDeviceEnvironment &rootDeviceEnvironment) {}

template <typename Family>
inline size_t EncodeMemoryPrefetch<Family>::getSizeForMemoryPrefetch(size_t size, const RootDeviceEnvironment &rootDeviceEnvironment) { return 0u; }

template <typename Family>
void EncodeMiArbCheck<Family>::program(LinearStream &commandStream, std::optional<bool> preParserDisable) {
    MI_ARB_CHECK cmd = Family::cmdInitArbCheck;

    EncodeMiArbCheck<Family>::adjust(cmd, preParserDisable);
    auto miArbCheckStream = commandStream.getSpaceForCmd<MI_ARB_CHECK>();
    *miArbCheckStream = cmd;
}

template <typename Family>
size_t EncodeMiArbCheck<Family>::getCommandSize() { return sizeof(MI_ARB_CHECK); }

template <typename Family>
void EncodeMiArbCheck<Family>::programWithWa(LinearStream &commandStream, std::optional<bool> preParserDisable, EncodeDummyBlitWaArgs &waArgs) {
    BlitCommandsHelper<Family>::dispatchDummyBlit(commandStream, waArgs);
    EncodeMiArbCheck<Family>::program(commandStream, preParserDisable);
}

template <typename Family>
size_t EncodeMiArbCheck<Family>::getCommandSizeWithWa(const EncodeDummyBlitWaArgs &waArgs) {
    return EncodeMiArbCheck<Family>::getCommandSize() + BlitCommandsHelper<Family>::getDummyBlitSize(waArgs);
}

template <typename Family>
inline void EncodeNoop<Family>::alignToCacheLine(LinearStream &commandStream) {
    auto used = commandStream.getUsed();
    auto alignment = MemoryConstants::cacheLineSize;
    auto partialCacheline = used & (alignment - 1);
    if (partialCacheline) {
        auto amountToPad = alignment - partialCacheline;
        auto pCmd = commandStream.getSpace(amountToPad);
        memset(pCmd, 0, amountToPad);
    }
}

template <typename Family>
inline void EncodeNoop<Family>::emitNoop(LinearStream &commandStream, size_t bytesToUpdate) {
    if (bytesToUpdate) {
        auto ptr = commandStream.getSpace(bytesToUpdate);
        memset(ptr, 0, bytesToUpdate);
    }
}

template <typename Family>
inline void EncodeStoreMemory<Family>::programStoreDataImm(LinearStream &commandStream,
                                                           uint64_t gpuAddress,
                                                           uint32_t dataDword0,
                                                           uint32_t dataDword1,
                                                           bool storeQword,
                                                           bool workloadPartitionOffset) {
    auto miStoreDataImmBuffer = commandStream.getSpaceForCmd<MI_STORE_DATA_IMM>();
    EncodeStoreMemory<Family>::programStoreDataImm(miStoreDataImmBuffer,
                                                   gpuAddress,
                                                   dataDword0,
                                                   dataDword1,
                                                   storeQword,
                                                   workloadPartitionOffset);
}

template <typename GfxFamily>
void EncodeEnableRayTracing<GfxFamily>::append3dStateBtd(void *ptr3dStateBtd) {}

template <typename GfxFamily>
inline void EncodeWA<GfxFamily>::setAdditionalPipeControlFlagsForNonPipelineStateCommand(PipeControlArgs &args) {}

template <typename Family>
size_t EncodeMemoryFence<Family>::getSystemMemoryFenceSize() {
    return 0;
}

template <typename Family>
void EncodeMemoryFence<Family>::encodeSystemMemoryFence(LinearStream &commandStream, const GraphicsAllocation *globalFenceAllocation, LogicalStateHelper *logicalStateHelper) {
}

template <typename Family>
size_t EncodeKernelArgsBuffer<Family>::getKernelArgsBufferCmdsSize(const GraphicsAllocation *kernelArgsBufferAllocation, LogicalStateHelper *logicalStateHelper) {
    return 0;
}

template <typename Family>
void EncodeKernelArgsBuffer<Family>::encodeKernelArgsBufferCmds(const GraphicsAllocation *kernelArgsBufferAllocation, LogicalStateHelper *logicalStateHelper) {}

template <typename Family>
void EncodeMiPredicate<Family>::encode(LinearStream &cmdStream, [[maybe_unused]] MiPredicateType predicateType) {
    if constexpr (Family::isUsingMiSetPredicate) {
        using MI_SET_PREDICATE = typename Family::MI_SET_PREDICATE;
        using PREDICATE_ENABLE = typename MI_SET_PREDICATE::PREDICATE_ENABLE;

        auto miSetPredicate = Family::cmdInitSetPredicate;
        miSetPredicate.setPredicateEnable(static_cast<PREDICATE_ENABLE>(predicateType));

        *cmdStream.getSpaceForCmd<MI_SET_PREDICATE>() = miSetPredicate;
    }
}
} // namespace NEO
