"""Embed build-time SPIR-V; the executable needs no shader compiler or sidecars."""
from pathlib import Path
import struct
import sys

directory = Path(sys.argv[1])
lines = ["#pragma once", "#include <cstdint>"]
for name, stage, symbol in (("ui", "vert", "kUiVert"), ("ui", "frag", "kUiFrag"), ("paint", "comp", "kPaintComp"), ("canvas", "vert", "kCanvasVert"), ("canvas", "frag", "kCanvasFrag")):
    data = (directory / f"{name}.{stage}.spv").read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines.append(f"inline constexpr std::uint32_t {symbol}[] = {{")
    for offset in range(0, len(words), 8):
        lines.append(",".join(f"0x{word:08x}" for word in words[offset:offset + 8]) + ",")
    lines.append("};")
(directory / "ui_shaders.generated.h").write_text("\n".join(lines) + "\n")
