/*
 * Copyright (C) 2017-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "opencl/test/unit_test/fixtures/device_fixture.h"
#include "opencl/test/unit_test/fixtures/device_host_queue_fixture.h"
#include "opencl/test/unit_test/mocks/mock_buffer.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_program.h"

using namespace NEO;
using namespace DeviceHostQueue;

struct KernelArgDevQueueTest : public DeviceFixture,
                               public DeviceHostQueueFixture<DeviceQueue> {
  protected:
    void SetUp() override {
        DeviceFixture::SetUp();
        DeviceHostQueueFixture<DeviceQueue>::SetUp();
        if (!this->pDevice->getHardwareInfo().capabilityTable.supportsDeviceEnqueue) {
            GTEST_SKIP();
        }

        pDeviceQueue = createQueueObject();

        pKernelInfo = std::make_unique<KernelInfo>();
        pKernelInfo->kernelArgInfo.resize(1);
        pKernelInfo->kernelArgInfo[0].isDeviceQueue = true;

        kernelArgPatchInfo.crossthreadOffset = 0x4;
        kernelArgPatchInfo.size = 0x4;
        kernelArgPatchInfo.sourceOffset = 0;

        pKernelInfo->kernelArgInfo[0].kernelArgPatchInfoVector.push_back(kernelArgPatchInfo);

        program = std::make_unique<MockProgram>(*pDevice->getExecutionEnvironment());
        pKernel = new MockKernel(program.get(), *pKernelInfo, *pClDevice);
        ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

        uint8_t pCrossThreadData[crossThreadDataSize];
        memset(pCrossThreadData, crossThreadDataInit, sizeof(pCrossThreadData));
        pKernel->setCrossThreadData(pCrossThreadData, sizeof(pCrossThreadData));
    }

    void TearDown() override {
        delete pKernel;

        delete pDeviceQueue;

        DeviceHostQueueFixture<DeviceQueue>::TearDown();
        DeviceFixture::TearDown();
    }

    bool crossThreadDataUnchanged() {
        for (uint32_t i = 0; i < crossThreadDataSize; i++) {
            if (pKernel->mockCrossThreadData[i] != crossThreadDataInit) {
                return false;
            }
        }

        return true;
    }

    static const uint32_t crossThreadDataSize = 0x10;
    static const char crossThreadDataInit = 0x7e;

    std::unique_ptr<MockProgram> program;
    DeviceQueue *pDeviceQueue = nullptr;
    MockKernel *pKernel = nullptr;
    std::unique_ptr<KernelInfo> pKernelInfo;
    KernelArgPatchInfo kernelArgPatchInfo;
};

HWCMDTEST_F(IGFX_GEN8_CORE, KernelArgDevQueueTest, GIVENkernelWithDevQueueArgWHENsetArgHandleTHENsetsProperHandle) {
    EXPECT_EQ(pKernel->kernelArgHandlers[0], &Kernel::setArgDevQueue);
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelArgDevQueueTest, GIVENdevQueueArgHandlerWHENpassDevQueueTHENacceptObjAndPatch) {
    auto clDeviceQueue = static_cast<cl_command_queue>(pDeviceQueue);

    auto ret = pKernel->setArgDevQueue(0, sizeof(cl_command_queue), &clDeviceQueue);
    EXPECT_EQ(ret, CL_SUCCESS);

    auto gpuAddress = static_cast<uint32_t>(pDeviceQueue->getQueueBuffer()->getGpuAddressToPatch());
    auto patchLocation = ptrOffset(pKernel->mockCrossThreadData.data(), kernelArgPatchInfo.crossthreadOffset);
    EXPECT_EQ(*(reinterpret_cast<uint32_t *>(patchLocation)), gpuAddress);
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelArgDevQueueTest, GIVENdevQueueArgHandlerWHENpassNormalQueueTHENrejectObjAndReturnError) {
    auto clCmdQueue = static_cast<cl_command_queue>(pCommandQueue);

    auto ret = pKernel->setArgDevQueue(0, sizeof(cl_command_queue), &clCmdQueue);
    EXPECT_EQ(ret, CL_INVALID_DEVICE_QUEUE);
    EXPECT_EQ(crossThreadDataUnchanged(), true);
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelArgDevQueueTest, GIVENdevQueueArgHandlerWHENpassNonQueueObjTHENrejectObjAndReturnError) {
    Buffer *buffer = new MockBuffer();
    auto clBuffer = static_cast<cl_mem>(buffer);

    auto ret = pKernel->setArgDevQueue(0, sizeof(cl_command_queue), &clBuffer);
    EXPECT_EQ(ret, CL_INVALID_DEVICE_QUEUE);
    EXPECT_EQ(crossThreadDataUnchanged(), true);

    delete buffer;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelArgDevQueueTest, GIVENdevQueueArgHandlerWHENpassFakeQueueTHENrejectObjAndReturnError) {
    char *pFakeDeviceQueue = new char[sizeof(DeviceQueue)];
    auto clFakeDeviceQueue = reinterpret_cast<cl_command_queue *>(pFakeDeviceQueue);

    auto ret = pKernel->setArgDevQueue(0, sizeof(cl_command_queue), &clFakeDeviceQueue);
    EXPECT_EQ(ret, CL_INVALID_DEVICE_QUEUE);
    EXPECT_EQ(crossThreadDataUnchanged(), true);

    delete[] pFakeDeviceQueue;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelArgDevQueueTest, GIVENdevQueueArgHandlerWHENpassNullptrTHENrejectObjAndReturnError) {
    auto ret = pKernel->setArgDevQueue(0, sizeof(cl_command_queue), nullptr);
    EXPECT_EQ(ret, CL_INVALID_ARG_VALUE);
    EXPECT_EQ(crossThreadDataUnchanged(), true);
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelArgDevQueueTest, GIVENdevQueueArgHandlerWHENpassWrongSizeTHENrejectObjAndReturnError) {
    auto clDeviceQueue = static_cast<cl_command_queue>(pDeviceQueue);

    auto ret = pKernel->setArgDevQueue(0, sizeof(cl_command_queue) - 1, &clDeviceQueue);
    EXPECT_EQ(ret, CL_INVALID_ARG_SIZE);
    EXPECT_EQ(crossThreadDataUnchanged(), true);
}
