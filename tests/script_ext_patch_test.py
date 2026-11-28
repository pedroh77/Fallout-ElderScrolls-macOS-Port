"""Unit tests for tools/script_ext_patches.py."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import script_ext_patches  # noqa: E402

STEAM_SHA = '19406942e48724d797300c4ea6be9ac69a32f670b8c35db09279a2258422739f'

assert script_ext_patches.is_supported(STEAM_SHA)
assert script_ext_patches.is_supported(STEAM_SHA.upper())
assert not script_ext_patches.is_supported('0' * 64)

expected = script_ext_patches.expected_labels(STEAM_SHA)
assert len(expected) == 40

# Build a synthetic TU containing all 40 expected labels with their exact occurrence counts,
# plus an untouched bystander label containing 0x1280ULL to prove patching is strictly scoped.
profile = script_ext_patches.SCRIPT_EXT_PROFILES[STEAM_SHA]
chunks = [
    '#include "rt.h"\n',
    'void g_dummy(CPU *c) {\n',
    'L_999999: /* CMP EAX,0x1280 */\n',
    '  t0=((V)0x1280ULL);\n',
]
for label, (old, _, count) in sorted(profile.items()):
    chunks.append(f'{label} /* test */\n')
    for idx in range(count):
        chunks.append(f'  t{idx}={old.decode()};\n')
chunks.append('}\n')
synthetic = ''.join(chunks).encode()

patched, matched = script_ext_patches.patch_tu(synthetic, STEAM_SHA)
assert matched == expected, f'Mismatch: {expected ^ matched}'
assert b'extern uint32_t host_script_cmd_base, host_script_cmd_max_opcode, host_script_cmd_count;' in patched
assert b'L_999999: /* test */\n' not in patched
# Bystander label must keep its original 0x1280 literal.
bystander_block = patched.split(b'L_999999:', 1)[1].split(b'\nL_', 1)[0]
assert b'((V)0x1280ULL)' in bystander_block
assert b'host_script_cmd_max_opcode' not in bystander_block

# Unknown SHA leaves input untouched.
untouched, none_matched = script_ext_patches.patch_tu(synthetic, 'f' * 64)
assert untouched == synthetic and none_matched == set()

# Fail-closed check: wrong occurrence count inside a matched label raises ValueError.
bad = b'#include "rt.h"\nvoid g_bad(CPU *c){\nL_593909:\n  t0=((V)0x1280ULL);\n}\n'
try:
    script_ext_patches.patch_tu(bad, STEAM_SHA)
except ValueError:
    pass
else:
    raise AssertionError('Expected ValueError for mismatched occurrence count')

print('PASS: all 40 ScriptCommands relocation specs verified with strict scoping and fail-closed checks')
