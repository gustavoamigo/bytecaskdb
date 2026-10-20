// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_view.h — the SQL layer's byte-view adapters
// (bytecaskdb-sql-layer/common/bytecask_view.h), under the plugin's
// namespace, and the size limits the plugin opens the DB with.

#pragma once

#include "bytecask.hpp"
#include "bytecaskdb-sql-layer/common/bytecask_view.h"

namespace bytecaskdb {

using bytecask::sql::as_view;
using bytecask::sql::assign_bytes;
using bytecask::sql::to_uint8;
using bytecask::sql::u8_data;

// The key and value limits the plugin opens the DB with. A WritePlan built
// without a snapshot checks the limits it is given, the engine's defaults
// (4 KiB keys, 4 MiB values) if none, so every such plan takes these.
inline constexpr bytecask::SizeLimits kPluginSizeLimits{
    65535,              // the engine's hard ceiling: collation weights need it
    16 * 1024 * 1024};  // MEDIUMBLOB (16 MiB)

}  // namespace bytecaskdb
