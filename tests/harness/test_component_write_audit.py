"""Fail-closed contracts for ECS component writes.

The live Tier 2 suite exercises these through ComponentProxy assignment, but
the only size-zero layouts currently registered by the game are fieldless tag
components.  Pin the exact internal guards offline as well so a source change
cannot make the live refusal test pass for the wrong reason.
"""

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
COMPONENT_PROPERTY_C = REPO_ROOT / "src/entity/component_property.c"


def _function_body(source: str, start: str, end: str) -> str:
    match = re.search(
        rf"{re.escape(start)}(.*?){re.escape(end)}",
        source,
        re.DOTALL,
    )
    assert match, f"could not isolate {start}"
    return match.group(1)


def test_unknown_component_size_is_an_unconditional_write_refusal():
    source = COMPONENT_PROPERTY_C.read_text()
    body = _function_body(
        source,
        "static bool component_property_bounds_valid(",
        "\nbool component_property_write(",
    )
    guard = re.search(
        r"if\s*\(\s*layout->componentSize\s*==\s*0\s*\)"
        r"\s*\{.*?return\s+false\s*;",
        body,
        re.DOTALL,
    )
    assert guard, (
        "componentSize==0 must refuse writes unconditionally; without a known "
        "boundary, any field write can corrupt the adjacent ECS component"
    )


def test_dynamic_array_fields_are_classified_as_pointer_typed():
    source = COMPONENT_PROPERTY_C.read_text()
    body = _function_body(
        source,
        "static bool component_property_is_pointer_typed(",
        "\nstatic bool component_property_bounds_valid(",
    )
    assert re.search(
        r"prop->type\s*==\s*FIELD_TYPE_DYNAMIC_ARRAY", body
    ), (
        "DYNAMIC_ARRAY owns a game buffer pointer and must stay on the "
        "pointer-write refusal path"
    )
