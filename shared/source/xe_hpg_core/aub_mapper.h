/*
 * Copyright (C) 2021-2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/aub/aub_mapper_base.h"
#include "shared/source/helpers/constants.h"
#include "shared/source/xe_hpg_core/hw_cmds.h"

#include "aubstream/engine_node.h"

namespace NEO {
struct XeHpgCoreFamily;

template <>
struct AUBFamilyMapper<XeHpgCoreFamily> {
    enum { device = AubMemDump::DeviceValues::Dg2 };

    using AubTraits = AubMemDump::Traits<device, MemoryConstants::GfxAddressBits>;

    static const AubMemDump::LrcaHelper *const csTraits[aub_stream::NUM_ENGINES];

    static const MMIOList globalMMIO;
    static const MMIOList *perEngineMMIO[aub_stream::NUM_ENGINES];

    typedef AubMemDump::AubDump<AubTraits> AUB;
};
} // namespace NEO
