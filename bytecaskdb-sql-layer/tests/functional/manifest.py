# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""features.yaml: every feature a case may name (*The feature manifest*)."""

from __future__ import annotations

import dataclasses
import pathlib

import yaml

MANIFEST = pathlib.Path(__file__).with_name("features.yaml")


@dataclasses.dataclass(frozen=True)
class Feature:
    id: str
    name: str
    mandatory: bool
    pg: bool | None
    out_of_scope: str | None
    ours: bool


def load_manifest(path: pathlib.Path = MANIFEST) -> dict[str, Feature]:
    raw = yaml.safe_load(path.read_text()) or {}
    features = {}
    for fid, entry in raw.items():
        unknown = set(entry) - {"name", "mandatory", "pg", "out_of_scope", "description"}
        if unknown:
            raise ValueError(f"{path.name}: {fid}: unknown keys {sorted(unknown)}")
        ours = ":" in fid
        if ours and "description" not in entry:
            raise ValueError(f"{path.name}: {fid}: our own feature needs a description")
        features[fid] = Feature(
            id=fid,
            name=entry.get("name") or entry.get("description", ""),
            mandatory=bool(entry.get("mandatory", False)),
            pg=entry.get("pg"),
            out_of_scope=entry.get("out_of_scope"),
            ours=ours,
        )
    return features
