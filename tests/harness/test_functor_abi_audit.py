"""Offline structural guards for the ExecuteStatsFunctors wrapper ABI."""

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
FUNCTOR_HOOKS_C = REPO_ROOT / "src/stats/functor_hooks.c"

EXECUTE_WRAPPERS = {
    "hook_ExecuteFunctors_AttackTarget": "g_OrigAttackTarget",
    "hook_ExecuteFunctors_AttackPosition": "g_OrigAttackPosition",
    "hook_ExecuteFunctors_Move": "g_OrigMove",
    "hook_ExecuteFunctors_Target": "g_OrigTarget",
    "hook_ExecuteFunctors_NearbyAttacked": "g_OrigNearbyAttacked",
    "hook_ExecuteFunctors_NearbyAttacking": "g_OrigNearbyAttacking",
    "hook_ExecuteFunctors_Equip": "g_OrigEquip",
    "hook_ExecuteFunctors_Source": "g_OrigSource",
    "hook_ExecuteFunctors_Interrupt": "g_OrigInterrupt",
}


def _matching_delimiter(text: str, start: int, opening: str, closing: str) -> int:
    """Return the matching delimiter index for a simple C source region."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opening:
            depth += 1
        elif text[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError(f"unmatched {opening!r} at offset {start}")


def _function_signature_and_body(source: str, name: str) -> tuple[str, str]:
    definition = re.search(rf"\bstatic\s+void\s+{re.escape(name)}\s*\(", source)
    assert definition, f"{name} definition missing"

    parameters_start = source.find("(", definition.start())
    parameters_end = _matching_delimiter(source, parameters_start, "(", ")")
    body_start = source.find("{", parameters_end)
    assert body_start != -1, f"{name} body missing"
    body_end = _matching_delimiter(source, body_start, "{", "}")
    return (
        source[parameters_start + 1:parameters_end],
        source[body_start + 1:body_end],
    )


def test_execute_stats_functor_wrappers_forward_hidden_result_out():
    """All nine wrappers must receive and forward the hidden result storage.

    ``result_out`` is absent from the demangled C++ names but occupies the
    first machine-ABI argument. Omitting it from a wrapper or any call to the
    original function shifts every following argument and can crash the game.
    """
    source = FUNCTOR_HOOKS_C.read_text()
    discovered = set(
        re.findall(r"\bstatic\s+void\s+(hook_ExecuteFunctors_\w+)\s*\(", source)
    )
    assert discovered == set(EXECUTE_WRAPPERS), (
        "ExecuteStatsFunctors wrapper set changed; update the ABI forwarding "
        f"guard. Expected {sorted(EXECUTE_WRAPPERS)}, found {sorted(discovered)}"
    )
    assert len(EXECUTE_WRAPPERS) == 9

    for wrapper, original in EXECUTE_WRAPPERS.items():
        signature, body = _function_signature_and_body(source, wrapper)
        assert re.match(r"\s*void\s*\*\s*result_out\s*,", signature), (
            f"{wrapper} must declare hidden result_out as its first parameter"
        )

        original_calls = re.findall(
            rf"\b{re.escape(original)}\s*\((.*?)\)\s*;", body, re.DOTALL
        )
        assert original_calls, f"{wrapper} does not call {original}"
        for arguments in original_calls:
            assert re.match(r"\s*result_out\s*,", arguments), (
                f"{wrapper} call to {original} must forward result_out first: "
                f"{arguments.strip()}"
            )
