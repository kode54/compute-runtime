/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/assert_handler/assert_handler.h"
#include "shared/source/memory_manager/graphics_allocation.h"
#include "shared/test/common/mocks/mock_assert_handler.h"
#include "shared/test/common/mocks/mock_device.h"

#include "gtest/gtest.h"

using namespace NEO;

TEST(AssertHandlerTests, WhenAssertHandlerIsCreatedThenAssertBufferIsAllocated) {
    auto device = std::unique_ptr<MockDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    MockAssertHandler assertHandler(device.get());

    ASSERT_NE(nullptr, assertHandler.getAssertBuffer());
    EXPECT_EQ(assertHandler.assertBufferSize, assertHandler.getAssertBuffer()->getUnderlyingBufferSize());

    EXPECT_EQ(0u, reinterpret_cast<AssertBufferHeader *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer())->flags);
    EXPECT_EQ(assertHandler.assertBufferSize, reinterpret_cast<AssertBufferHeader *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer())->size);
    EXPECT_EQ(sizeof(AssertBufferHeader), reinterpret_cast<AssertBufferHeader *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer())->begin);
}

TEST(AssertHandlerTests, GivenAssertHandlerWhenCheckingAssertThenReturnValuIsBasedOnFlags) {
    auto device = std::unique_ptr<MockDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    MockAssertHandler assertHandler(device.get());

    ASSERT_NE(nullptr, assertHandler.getAssertBuffer());

    EXPECT_FALSE(assertHandler.checkAssert());

    reinterpret_cast<AssertBufferHeader *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer())->flags = 1;
    EXPECT_TRUE(assertHandler.checkAssert());
}

TEST(AssertHandlerTests, GivenNoFlagSetWhenPrintAssertAndAbortCalledThenAbortIsNotCalled) {
    if (sizeof(void *) == 4) {
        GTEST_SKIP();
    }
    auto device = std::unique_ptr<MockDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    MockAssertHandler assertHandler(device.get());
    ASSERT_NE(nullptr, assertHandler.getAssertBuffer());

    reinterpret_cast<AssertBufferHeader *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer())->flags = 0;

    ::testing::internal::CaptureStderr();
    assertHandler.printAssertAndAbort();

    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_STREQ("", output.c_str());
}

TEST(AssertHandlerTests, GivenFlagSetWhenPrintAssertAndAbortCalledThenMessageIsPrintedAndAbortCalled) {
    if (sizeof(void *) == 4) {
        GTEST_SKIP();
    }
    auto device = std::unique_ptr<MockDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    MockAssertHandler assertHandler(device.get());
    ASSERT_NE(nullptr, assertHandler.getAssertBuffer());

    reinterpret_cast<AssertBufferHeader *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer())->flags = 1;

    char const *message = "assert!";
    auto stringAddress = ptrOffset(reinterpret_cast<uint64_t *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer()), offsetof(AssertBufferHeader, begin) + sizeof(AssertBufferHeader::begin));
    auto formatStringAddress = reinterpret_cast<uint64_t>(message);
    memcpy(stringAddress, &formatStringAddress, 8);
    reinterpret_cast<AssertBufferHeader *>(assertHandler.getAssertBuffer()->getUnderlyingBuffer())->size = 2 * sizeof(uint64_t);

    ::testing::internal::CaptureStderr();
    EXPECT_THROW(assertHandler.printAssertAndAbort(), std::exception);

    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_STREQ("assert!", output.c_str());
}