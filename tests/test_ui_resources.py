"""Check source-authored Localized literals against every shipped catalog."""
import json
import pathlib
import re
import sys


def msg_key(value):
    digest = 14695981039346656037
    for byte in value.encode("utf-8"):
        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"msg_{digest:016x}"


def check(root):
    literals = {}
    for source in (root / "src").rglob("*"):
        if source.suffix not in {".cpp", ".cppm", ".inc", ".h"}:
            continue
        for match in re.finditer(r'Localized(?:Format|Resource)?\("((?:[^"\\]|\\.)*)"',
                                 source.read_text(encoding="utf-8")):
            value = json.loads('"' + match[1] + '"')
            literals[msg_key(value)] = (source.relative_to(root), value)
    # Card-back menus resolve titles dynamically through Localized(spec.title).
    grid = root / "src/ui/home_grid.h"
    for match in re.finditer(r'\{HomeCardKind::\w+,\s*"[^"]*",\s*"((?:[^"\\]|\\.)*)"',
                             grid.read_text(encoding="utf-8")):
        value = json.loads('"' + match[1] + '"')
        literals[msg_key(value)] = (grid.relative_to(root), value)
    assert literals, "No localized source literals found"
    errors = []
    catalogs = sorted((root / "resources/strings").glob("*.properties"))
    assert {"default", "en", "zh"} <= {catalog.stem for catalog in catalogs}, "Missing shipped catalogs"
    for catalog in catalogs:
        keys = set(re.findall(r"^(msg_[0-9a-f]{16})\s*=", catalog.read_text(encoding="utf-8"), re.M))
        for key in literals.keys() - keys:
            source, value = literals[key]
            errors.append(f"{catalog.name}: missing {key} ({source}: {value})")
    assert not errors, "\n".join(errors)
    print(f"UI resource catalogs cover {len(literals)} source literals")


if __name__ == "__main__":
    check(pathlib.Path(sys.argv[1]).resolve())
