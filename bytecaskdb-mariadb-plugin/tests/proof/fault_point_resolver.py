# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# fault_point_resolver.py — Maps failure classes to ScopedFaultInjector config.

from dataclasses import dataclass
from typing import Optional

from .scenario_matrix import PluginFailureClass


@dataclass
class FaultConfig:
    injector_name: Optional[str] = None
    needs_concurrent_writer: bool = False


def resolve_fault(failure: PluginFailureClass) -> FaultConfig:
    if failure == PluginFailureClass.SUCCESS:
        return FaultConfig()

    if failure == PluginFailureClass.OCC_CONFLICT:
        return FaultConfig(needs_concurrent_writer=True)

    if failure == PluginFailureClass.ENGINE_DEGRADED:
        return FaultConfig(injector_name="io_data_file_append")

    return FaultConfig()
