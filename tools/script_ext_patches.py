"""Build-time relocation patcher for extending the game's ScriptCommands table."""

_EX_BASE = b'((V)host_script_cmd_base)'
_EX_MAX = b'((V)host_script_cmd_max_opcode)'
_EX_CNT = b'((V)host_script_cmd_count)'


def _off(delta: int) -> bytes:
    return f'((V)(host_script_cmd_base+0x{delta:x}u))'.encode()


# Keyed by lowercase executable SHA-256 -> {label: (old_literal, replacement, expected_count)}.
SCRIPT_EXT_PROFILES = {
    # Fallout: New Vegas Steam (unpacked, 19406942e487...)
    '19406942e48724d797300c4ea6be9ac69a32f670b8c35db09279a2258422739f': {
        'L_593909:': (b'((V)0x1280ULL)', _EX_MAX, 3),
        'L_5b115b:': (b'((V)0x1280ULL)', _EX_MAX, 3),
        'L_5b1170:': (b'((V)0x1190910ULL)', _EX_BASE, 3),
        'L_5b19a0:': (b'((V)0x280ULL)', _EX_CNT, 3),
        'L_5b19af:': (b'((V)0x1190910ULL)', _EX_BASE, 1),
        'L_5b19cc:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5b19f6:': (b'((V)0x1190918ULL)', _off(0x08), 1),
        'L_5bcbdd:': (b'((V)0x280ULL)', _EX_CNT, 3),
        'L_5bcc08:': (b'((V)0x119091cULL)', _off(0x0c), 1),
        'L_5bcc2b:': (b'((V)0x1190910ULL)', _EX_BASE, 1),
        'L_5bcc4e:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5bcc6e:': (b'((V)0x119091cULL)', _off(0x0c), 1),
        'L_5bcc84:': (b'((V)0x119091cULL)', _off(0x0c), 1),
        'L_5bcca4:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5bccb6:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5bccd2:': (b'((V)0x119091cULL)', _off(0x0c), 1),
        'L_5bcce2:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5bccf2:': (b'((V)0x1190910ULL)', _EX_BASE, 1),
        'L_5bcd11:': (b'((V)0x119091cULL)', _off(0x0c), 1),
        'L_5bcd21:': (b'((V)0x1190910ULL)', _EX_BASE, 1),
        'L_5bcd40:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5bcd52:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5bcd6e:': (b'((V)0x1190914ULL)', _off(0x04), 1),
        'L_5bcd7e:': (b'((V)0x1190910ULL)', _EX_BASE, 1),
        'L_5bcd9d:': (b'((V)0x1190910ULL)', _EX_BASE, 1),
        'L_681238:': (b'((V)0x1280ULL)', _EX_MAX, 3),
        'L_681255:': (b'((V)0x280ULL)', _EX_CNT, 3),
        'L_681274:': (b'((V)0x280ULL)', _EX_CNT, 3),
        'L_681709:': (b'((V)0x1190930ULL)', _off(0x20), 1),
        'L_68171f:': (b'((V)0x1190920ULL)', _off(0x10), 1),
        'L_681750:': (b'((V)0x1190930ULL)', _off(0x20), 1),
        'L_681ae9:': (b'((V)0x1190930ULL)', _off(0x20), 1),
        'L_681cdc:': (b'((V)0x1190910ULL)', _EX_BASE, 1),
        'L_6820fd:': (b'((V)0x1190924ULL)', _off(0x14), 1),
        'L_68228a:': (b'((V)0x1190922ULL)', _off(0x12), 1),
        'L_6822fd:': (b'((V)0x1190924ULL)', _off(0x14), 1),
        'L_682350:': (b'((V)0x1190924ULL)', _off(0x14), 1),
        'L_6823b0:': (b'((V)0x1190924ULL)', _off(0x14), 1),
        'L_87a906:': (b'((V)0x1190922ULL)', _off(0x12), 1),
        'L_87a946:': (b'((V)0x1190924ULL)', _off(0x14), 1),
    },
}

_EXTERN_DECL = (
    b'#include "rt.h"\n'
    b'extern uint32_t host_script_cmd_base, host_script_cmd_max_opcode, host_script_cmd_count;\n'
)


def is_supported(exe_sha256: str) -> bool:
    return bool(exe_sha256 and exe_sha256.lower() in SCRIPT_EXT_PROFILES)


def expected_labels(exe_sha256: str) -> set[str]:
    if not exe_sha256:
        return set()
    return set(SCRIPT_EXT_PROFILES.get(exe_sha256.lower(), ()))


def patch_tu(content: bytes, exe_sha256: str) -> tuple[bytes, set[str]]:
    """Redirect ScriptCommands table references in matching instruction blocks."""
    profile = SCRIPT_EXT_PROFILES.get((exe_sha256 or '').lower())
    if not profile:
        return content, set()

    # Quick pre-filter so untouched TUs avoid line splitting.
    if not any(old in content for old, _, _ in profile.values()):
        return content, set()

    lines = content.splitlines(keepends=True)
    matched: set[str] = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith(b'L_'):
            label = line.split(b':', 1)[0].decode('ascii', 'ignore') + ':'
            spec = profile.get(label)
            if spec is not None:
                old, new, expected_count = spec
                j = i + 1
                while j < len(lines) and not lines[j].startswith(b'L_') and not lines[j].startswith(b'}'):
                    j += 1
                block = b''.join(lines[i + 1:j])
                actual_count = block.count(old)
                if actual_count != expected_count:
                    raise ValueError(
                        f'ScriptCommands patch mismatch at {label}: expected {expected_count} of {old!r}, got {actual_count}'
                    )
                if label in matched:
                    raise ValueError(f'Duplicate ScriptCommands patch label {label}')
                patched_block = block.replace(old, new)
                lines[i + 1:j] = patched_block.splitlines(keepends=True)
                matched.add(label)
                i += 1 + len(patched_block.splitlines())
                continue
        i += 1

    if not matched:
        return content, matched

    out = b''.join(lines)
    if b'#include "rt.h"\n' not in out:
        raise ValueError('Missing #include "rt.h" in patched translation unit')
    out = out.replace(b'#include "rt.h"\n', _EXTERN_DECL, 1)
    return out, matched
