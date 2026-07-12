#!/usr/bin/env python3
"""Generates the CQL column-value wire codec from cql/engine/codegen/wire_types.json.

Single source of truth for "which bytes, in what order, for which type::Basic" — every
consumer (io.codec's coroutine read/skip/write-default, io.codec's ColumnValue writer, and
io.codec's synchronous BTree-comparator decoder) is rendered from the same field list per
type, so they cannot silently drift apart.

This script only parses wire_types.json into dataclasses and computes small derived facts
(e.g. which fields must be retained during skip, which type is the "native" width for a
literal-widen family). All C++ syntax is built by the Jinja templates in templates/.

Usage: python3 generate.py   (run from anywhere; paths are relative to this file)
Requires: jinja2 (pip install jinja2). Not a build dependency — output is checked in under
cql/engine/generated/; re-run this script by hand after editing wire_types.json.
"""
import json
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from jinja2 import Environment, FileSystemLoader

ROOT = Path(__file__).resolve().parent
SPEC_PATH = ROOT / "wire_types.json"
TEMPLATES_DIR = ROOT / "templates"
OUT_DIR = ROOT.parent / "generated"

GENERATED_HEADER = """\
// GENERATED FILE — do not edit by hand.
// Produced by cql/engine/codegen/generate.py from cql/engine/codegen/wire_types.json.
// Re-run that script after editing the spec; do not hand-patch this file.
"""


@dataclass
class Field:
    kind: str
    member: str | None = None
    ctype: str | None = None
    width: int | None = None
    condition_member: str | None = None
    true_member: str | None = None
    true_width: int | None = None
    false_member: str | None = None
    false_width: int | None = None
    # Derived: this field is a flag/raw_scalar referenced as a later conditional_bytes
    # field's condition_member, so skip_column_value must read it into a plain local
    # instead of discarding its bytes.
    retained_for_skip: bool = False

    @property
    def local_name(self) -> str:
        return (self.member or "").replace(".", "_")


@dataclass
class WireType:
    basic: list[str]
    shape: str
    ctype: str | None = None
    fields: list[Field] = field(default_factory=list)
    literal_widen: str | None = None
    token_eligible: bool = False

    @property
    def column_value_ctype(self) -> str:
        if self.shape == "text":
            return "AutoString8"
        if self.shape == "blob":
            return "Blob"
        return self.ctype


@dataclass
class CtypeGroup:
    ctype: str
    basics: list[str]


@dataclass
class NarrowArm:
    ctype: str
    basics: list[str]
    is_native: bool


def parse_field(raw: dict) -> Field:
    return Field(
        kind=raw["kind"],
        member=raw.get("member"),
        ctype=raw.get("ctype"),
        width=raw.get("width"),
        condition_member=raw.get("condition_member"),
        true_member=raw.get("true_member"),
        true_width=raw.get("true_width"),
        false_member=raw.get("false_member"),
        false_width=raw.get("false_width"),
    )


def parse_wire_type(raw: dict) -> WireType:
    fields = [parse_field(f) for f in raw.get("fields", [])]
    retained = {f.condition_member for f in fields if f.kind == "conditional_bytes"}
    for f in fields:
        if f.member in retained:
            f.retained_for_skip = True
    return WireType(
        basic=raw["basic"],
        shape=raw["shape"],
        ctype=raw.get("ctype"),
        fields=fields,
        literal_widen=raw.get("literal_widen"),
        token_eligible=raw.get("token_eligible", False),
    )


def load_types() -> list[WireType]:
    spec = json.loads(SPEC_PATH.read_text())
    types = [parse_wire_type(t) for t in spec["types"]]
    for t in types:
        assert len(t.basic) >= 1
    return types


def dedup_by_ctype(types: list[WireType]) -> list[WireType]:
    """One representative WireType per distinct ColumnValue C++ type (multiple JSON
    entries can share a ctype, e.g. boolean and tinyint are both U8)."""
    seen: dict[str, WireType] = {}
    for t in types:
        seen.setdefault(t.column_value_ctype, t)
    return list(seen.values())


def token_wire_types(types: list[WireType]) -> list[WireType]:
    """Token-eligible types, deduped by ColumnValue ctype — the token wire body only
    depends on ctype/shape, so e.g. tinyint and boolean (both U8) need one arm, not two."""
    return dedup_by_ctype([t for t in types if t.token_eligible])


def exact_ctype_groups(types: list[WireType]) -> list[CtypeGroup]:
    """Every ColumnValue C++ type's exact compatibility set, except S64/F64 which use the
    wider literal-widen family instead (a Literal S64/F64 can narrow into several distinct
    ColumnValue ctypes)."""
    groups: dict[str, list[str]] = {}
    for t in types:
        groups.setdefault(t.column_value_ctype, []).extend(t.basic)
    return [CtypeGroup(ctype, basics) for ctype, basics in groups.items() if ctype not in ("S64", "F64")]


def basics_with_widen(types: list[WireType], widen: str) -> list[str]:
    out: list[str] = []
    for t in types:
        if t.literal_widen == widen:
            out.extend(t.basic)
    return out


def narrow_arms(types: list[WireType], widen: str, native_ctype: str) -> list[NarrowArm]:
    arms = [NarrowArm(t.ctype, t.basic, t.ctype == native_ctype) for t in types if t.literal_widen == widen]
    assert any(a.is_native for a in arms), f"no native-width ({native_ctype}) entry for literal_widen={widen!r}"
    return arms


def main() -> None:
    types = load_types()

    env = Environment(
        loader=FileSystemLoader(str(TEMPLATES_DIR)),
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    io_codec_wire = env.get_template("io_codec_wire.cpp.jinja").render(
        header=GENERATED_HEADER,
        types=types,
    )
    write_generated(OUT_DIR / "io_codec_wire.cpp", io_codec_wire)

    io_codec_wire_module = env.get_template("io_codec_wire.cppm.jinja").render(
        header=GENERATED_HEADER,
        write_arm_types=dedup_by_ctype(types),
        compat_exact_groups=exact_ctype_groups(types),
        integer_family_basics=basics_with_widen(types, "integer"),
        float_family_basics=basics_with_widen(types, "float"),
        narrow_s64_arms=narrow_arms(types, "integer", "S64"),
        narrow_f64_arms=narrow_arms(types, "float", "F64"),
        narrow_s64_native_ctype="S64",
        narrow_f64_native_ctype="F64",
    )
    write_generated(OUT_DIR / "io_codec_wire.cppm", io_codec_wire_module)

    io_codec_sync = env.get_template("io_codec_sync.cpp.jinja").render(
        header=GENERATED_HEADER,
        types=types,
    )
    write_generated(OUT_DIR / "io_codec_sync.cpp", io_codec_sync)

    token_wire_module = env.get_template("token_wire.cppm.jinja").render(
        header=GENERATED_HEADER,
    )
    write_generated(OUT_DIR / "token_wire.cppm", token_wire_module)

    token_wire = env.get_template("token_wire.cpp.jinja").render(
        header=GENERATED_HEADER,
        token_types=token_wire_types(types),
    )
    write_generated(OUT_DIR / "token_wire.cpp", token_wire)


def write_generated(path: Path, content: str) -> None:
    path.write_text(content)
    clang_format = shutil.which("clang-format")
    if clang_format:
        subprocess.run([clang_format, "-i", str(path)], check=True)
    print(f"Wrote {path}{'' if clang_format else ' (clang-format not found, left unformatted)'}")


if __name__ == "__main__":
    main()
