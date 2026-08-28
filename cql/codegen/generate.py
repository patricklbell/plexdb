#!/usr/bin/env python3
"""Generic codegen driver: renders every cql/*/templates/*.jinja against cql/codegen/types.json,
writing each to the sibling cql/*/generated/ directory.

This script has no knowledge of which packages exist under cql/ (no "engine"/"native" special
casing) — it discovers templates by walking for `templates/` directories and mirrors each one
to a `generated/` directory next to it. Adding a new consumer means dropping a templates/
folder next to its code, not editing this file. What IS coupled here is the data source
(types.json — "which bytes, in what order, for which type::Basic") and the small pure
functions that derive per-template-shape data from it (dedup_by_ctype, token_wire_types, …);
which of those derived views a given template needs is looked up by the template's own
filename in EXTRA_CONTEXT below — a generic string key, not a reference to any consumer.

Usage: python3 generate.py   (run from anywhere; paths are relative to this file)
Requires: jinja2 (pip install jinja2). Not a build dependency — output is checked in; re-run
this script by hand after editing types.json or any *.jinja template.
"""
import json
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from jinja2 import Environment, FileSystemLoader

ROOT = Path(__file__).resolve().parent
CQL_ROOT = ROOT.parent
SPEC_PATH = ROOT / "types.json"

GENERATED_HEADER = """\
// GENERATED FILE — do not edit by hand.
// Produced by cql/codegen/generate.py from cql/codegen/types.json.
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


def io_codec_wire_cppm_context(types: list[WireType]) -> dict:
    return dict(
        write_arm_types=dedup_by_ctype(types),
        compat_exact_groups=exact_ctype_groups(types),
        integer_family_basics=basics_with_widen(types, "integer"),
        float_family_basics=basics_with_widen(types, "float"),
        narrow_s64_arms=narrow_arms(types, "integer", "S64"),
        narrow_f64_arms=narrow_arms(types, "float", "F64"),
        narrow_s64_native_ctype="S64",
        narrow_f64_native_ctype="F64",
    )


# Per-template extra render context, keyed by the template's own filename (a generic string,
# not a reference to whichever package owns it). Templates not listed here just get the base
# context (types, header).
EXTRA_CONTEXT = {
    "io_codec_wire.cppm.jinja": io_codec_wire_cppm_context,
    "token_wire.cpp.jinja": lambda types: dict(token_types=token_wire_types(types)),
    "native_wire.cppm.jinja": lambda types: dict(write_arm_types=dedup_by_ctype(types)),
}


def discover_templates() -> list[Path]:
    """Every *.jinja file under a cql/*/templates/ directory, sorted for reproducible output."""
    return sorted(CQL_ROOT.glob("*/templates/*.jinja"))


def output_path_for(template_path: Path) -> Path:
    """cql/<pkg>/templates/foo.ext.jinja -> cql/<pkg>/generated/foo.ext"""
    templates_dir = template_path.parent
    pkg_dir = templates_dir.parent
    return pkg_dir / "generated" / template_path.stem


def main() -> None:
    types = load_types()
    templates = discover_templates()
    template_dirs = sorted({str(t.parent) for t in templates})

    env = Environment(
        loader=FileSystemLoader([str(ROOT)] + template_dirs),
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )

    for template_path in templates:
        context = dict(header=GENERATED_HEADER, types=types)
        extra = EXTRA_CONTEXT.get(template_path.name)
        if extra is not None:
            context.update(extra(types))

        rendered = env.get_template(template_path.name).render(**context)
        out_path = output_path_for(template_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        write_generated(out_path, rendered)


def write_generated(path: Path, content: str) -> None:
    path.write_text(content)
    clang_format = shutil.which("clang-format")
    if clang_format:
        subprocess.run([clang_format, "-i", str(path)], check=True)
    print(f"Wrote {path}{'' if clang_format else ' (clang-format not found, left unformatted)'}")


if __name__ == "__main__":
    main()
