"""Check version and branding resources in the linked Windows executable."""
from datetime import datetime, date
from pathlib import Path
import struct
import sys

executable, version, mode = sys.argv[1:]
data = Path(executable).read_bytes()


def unpack(fmt, offset):
    return struct.unpack_from("<" + fmt, data, offset)


pe = unpack("I", 0x3C)[0]
assert data[pe:pe + 4] == b"PE\0\0", "Invalid PE image"
section_count = unpack("H", pe + 6)[0]
optional_size = unpack("H", pe + 20)[0]
optional = pe + 24
assert unpack("H", optional)[0] == 0x20B, "Expected Windows x64 image"
resource_rva = unpack("I", optional + 112 + 2 * 8)[0]
sections = []
for index in range(section_count):
    entry = optional + optional_size + index * 40
    virtual_size, rva, raw_size, raw_offset = unpack("IIII", entry + 8)
    sections.append((rva, max(virtual_size, raw_size), raw_offset))


def file_offset(rva):
    for start, size, raw in sections:
        if start <= rva < start + size:
            return raw + rva - start
    raise AssertionError(f"Unmapped resource RVA: {rva}")


resource_root = file_offset(resource_rva)


def entries(relative):
    directory = resource_root + relative
    named, numbered = unpack("HH", directory + 12)
    return [unpack("II", directory + 16 + index * 8) for index in range(named + numbered)]


def resource(kind, identifier):
    relative = 0
    for wanted in (kind, identifier):
        matches = [child for key, child in entries(relative) if key == wanted]
        assert len(matches) == 1 and matches[0] & 0x80000000, "Missing resource directory"
        relative = matches[0] & 0x7FFFFFFF
    language, child = entries(relative)[0]
    assert not child & 0x80000000, "Invalid resource data entry"
    rva, size = unpack("II", resource_root + child)
    offset = file_offset(rva)
    return data[offset:offset + size]


version_info = resource(16, 1)  # RT_VERSION
wide_version = version.encode("utf-16le") + b"\0\0"
assert wide_version in version_info, "Windows display version is stale"
assert ("SIMP\nSimple Image Manipulation Program\nVersion " + version).encode("utf-16le") in data, "About version is stale"
assert ("v. " + version).encode("utf-16le") + b"\0\0" in data, "Splash version is missing or stale"
assert ("SIMP " + version).encode() + b"\0" in data, "Vulkan application version is stale"

fixed = version_info.index(struct.pack("<I", 0xFEEF04BD))
words = struct.unpack_from("<13I", version_info, fixed)
major, minor, stamp, build = version.split(".")
epoch_day = (datetime.strptime(stamp, "%Y%m%d").date() - date(2020, 1, 1)).days
expected = (int(major) << 16 | int(minor), epoch_day << 16 | int(build))
assert words[2:4] == expected and words[4:6] == expected, "Windows fixed version is stale"
assert words[7] == (1 if mode == "debug" else 0), "Windows debug flag does not match the build"

branding = Path(__file__).resolve().parent.parent / "graphics/branding"
for identifier, name in ((120, "vulkan.png"), (121, "lakeshore.png")):
    assert resource(10, identifier) == (branding / name).read_bytes(), f"Embedded {name} differs from the supplied PNG"
root = branding.parent.parent
assert resource(10, 1000) == (root / "graphics/splash_screen/frame_0001.png").read_bytes(), "Splash poster was modified"
movie = resource(10, 122)
assert movie == (root / "build/generated/splash.mp4").read_bytes() and movie[4:8] == b"ftyp", "Missing embedded MP4"
raw = [child for key, child in entries(0) if key == 10][0] & 0x7FFFFFFF
assert not any(1001 <= key < 2000 for key, child in entries(raw)), "Old PNG sequence remains embedded"
# Optional video components must not become loader dependencies: Windows N
# without media components must still be able to launch and display the poster.
import_rva = unpack("I", optional + 112 + 8)[0]
imports = []
if import_rva:
    cursor = file_offset(import_rva)
    while any(unpack("IIIII", cursor)):
        name = file_offset(unpack("I", cursor + 12)[0])
        imports.append(data[name:data.index(b"\0", name)].decode().lower())
        cursor += 20
assert not {"mfplat.dll", "mfreadwrite.dll", "mf.dll"} & set(imports), "Media DLLs must remain optional"
for url in ("https://www.vulkan.org/", "https://www.lakeshoretechnical.com/"):
    assert url.encode("utf-16le") + b"\0\0" in data, f"Splash hyperlink is missing: {url}"
print(f"Verified {Path(executable).name}: {version}, About/splash/Vulkan strings, Windows metadata, both branding logos and links, MP4 and original poster (no PNG sequence)")
