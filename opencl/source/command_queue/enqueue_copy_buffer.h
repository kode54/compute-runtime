/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "opencl/source/command_queue/command_queue_hw.h"
#include "opencl/source/command_queue/enqueue_common.h"
#include "opencl/source/mem_obj/buffer.h"
#include "opencl/source/memory_manager/mem_obj_surface.h"

namespace NEO {

template <typename GfxFamily>
cl_int CommandQueueHw<GfxFamily>::enqueueCopyBuffer(
    Buffer *srcBuffer,
    Buffer *dstBuffer,
    size_t srcOffset,
    size_t dstOffset,
    size_t size,
    cl_uint numEventsInWaitList,
    const cl_event *eventWaitList,
    cl_event *event) {
    auto eBuiltInOpsType = EBuiltInOps::CopyBufferToBuffer;
    constexpr cl_command_type cmdType = CL_COMMAND_COPY_BUFFER;

    CsrSelectionArgs csrSelectionArgs{cmdType, srcBuffer, dstBuffer, device->getRootDeviceIndex(), &size};
    CommandStreamReceiver &csr = selectCsrForBuiltinOperation(csrSelectionArgs);

    if (forceStateless(std::max(srcBuffer->getSize(), dstBuffer->getSize()))) {
        eBuiltInOpsType = EBuiltInOps::CopyBufferToBufferStateless;
    }

    BuiltinOpParams dc;
    dc.srcMemObj = srcBuffer;
    dc.dstMemObj = dstBuffer;
    dc.srcOffset = {srcOffset, 0, 0};
    dc.dstOffset = {dstOffset, 0, 0};
    dc.size = {size, 0, 0};
    dc.bcsSplit = this->isSplitEnqueueBlitNeeded(csrSelectionArgs.direction, size, csr);
    dc.direction = csrSelectionArgs.direction;

    MultiDispatchInfo dispatchInfo(dc);

    MemObjSurface s1(srcBuffer);
    MemObjSurface s2(dstBuffer);
    Surface *surfaces[] = {&s1, &s2};

    return dispatchBcsOrGpgpuEnqueue<CL_COMMAND_COPY_BUFFER>(dispatchInfo, surfaces, eBuiltInOpsType, numEventsInWaitList, eventWaitList, event, false, csr);
}

} // namespace NEO
