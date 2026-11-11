"""Check wire feature allocations against the protocol registry."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
NAME = r"(?:LI|ML)_FF_[A-Z0-9_]+"


def header_flags(text):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    flags = []
    for name, value in re.findall(
        rf"^\s*#\s*define\s+({NAME})\s+([^\n]+)", text, re.M
    ):
        value = value.strip().strip("()").strip()
        value = re.sub(r"[uUlL]+$", "", value)
        if not re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9]+", value):
            raise ValueError(f"{name}: unsupported flag expression {value!r}")
        flags.append((name, int(value, 16 if value.lower().startswith("0x") else 10)))
    return flags


def registry_flags(text):
    return [
        (name, int(value, 16))
        for name, value in re.findall(
            rf"^\|\s*`({NAME})`\s*\|\s*`(0[xX][0-9a-fA-F]+)`\s*\|", text, re.M
        )
    ]


def allocation_errors(definitions, registry):
    errors = []
    for label, entries in (("header", definitions), ("registry", registry)):
        names = set()
        values = {}
        for name, value in entries:
            if name in names:
                errors.append(f"{label}: duplicate identifier {name}")
            names.add(name)
            namespace = name[:5]
            key = namespace, value
            if key in values and values[key] != name:
                errors.append(
                    f"{label}: {name} and {values[key]} collide at {value:#x}"
                )
            values[key] = name
            if value == 0 or value & (value - 1):
                errors.append(f"{label}: {name} is not a single bit: {value:#x}")
    registered = dict(registry)
    for name, value in definitions:
        if name not in registered:
            errors.append(f"{name}={value:#x} missing from registry")
        elif registered[name] != value:
            errors.append(
                f"{name}: header {value:#x} != registry {registered[name]:#x}"
            )
    return errors


class ProtocolRegistryTests(unittest.TestCase):
    def test_feature_allocations(self):
        definitions = []
        for path in sorted((ROOT / "src").glob("*.h")):
            definitions.extend(header_flags(path.read_text(encoding="utf-8")))
        # Also catch flags accidentally introduced in implementation files.
        for path in sorted((ROOT / "src").glob("*.c")):
            definitions.extend(header_flags(path.read_text(encoding="utf-8")))
        registry = registry_flags(
            (ROOT / "docs" / "protocol-extension-registry.md").read_text(
                encoding="utf-8"
            )
        )
        self.assertTrue(definitions, "No feature definitions found")
        self.assertTrue(registry, "No registry rows found")
        self.assertEqual([], allocation_errors(definitions, registry))

    def test_collisions_are_checked_within_each_namespace(self):
        entries = [("LI_FF_ONE", 0x8000), ("ML_FF_ONE", 0x8000)]
        self.assertEqual([], allocation_errors(entries, entries))
        entries.append(("LI_FF_TWO", 0x8000))
        self.assertIn("collide at 0x8000", "\n".join(allocation_errors(entries, entries)))

    def test_missing_and_mismatched_registry_values(self):
        errors = allocation_errors(
            [("LI_FF_ONE", 0x4000), ("ML_FF_ONE", 0x8000)],
            [("LI_FF_ONE", 0x8000)],
        )
        self.assertIn("LI_FF_ONE: header 0x4000 != registry 0x8000", errors)
        self.assertIn("ML_FF_ONE=0x8000 missing from registry", errors)

    def test_duplicate_registry_rows_and_non_bits(self):
        errors = allocation_errors([], [("LI_FF_ONE", 3), ("LI_FF_ONE", 3)])
        self.assertIn("registry: duplicate identifier LI_FF_ONE", errors)
        self.assertIn("registry: LI_FF_ONE is not a single bit: 0x3", errors)

    def test_parser_handles_comments_suffixes_and_table_only_rows(self):
        self.assertEqual(
            [("LI_FF_ONE", 0x4000), ("ML_FF_ONE", 32768)],
            header_flags(
                "// #define LI_FF_UNUSED 0x4000\n"
                "/* #define ML_FF_UNUSED 0x8000 */\n"
                "#define LI_FF_ONE (0x4000U) // reserved\n"
                "#define ML_FF_ONE 32768UL\n"
            ),
        )
        self.assertEqual(
            [("LI_FF_ONE", 0x4000)],
            registry_flags(
                "Reserve `LI_FF_UNUSED` at `0x4000`.\n"
                "| `LI_FF_ONE` | `0X4000` | Restart. |\n"
            ),
        )

    def test_unknown_expression_fails_instead_of_skipping_a_flag(self):
        with self.assertRaisesRegex(ValueError, "unsupported flag expression"):
            header_flags("#define LI_FF_ONE (1 << 14)\n")


if __name__ == "__main__":
    unittest.main()
