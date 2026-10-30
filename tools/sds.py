#!/usr/bin/env python3
"""Rozbalovač SDS archivů Mafie II (verze 19, PC) pro Linux – bez Windows a .NET.

  python3 tools/sds.py list    <archiv.sds>
  python3 tools/sds.py extract <archiv.sds> <výstupní složka>
  python3 tools/sds.py scripts <složka se .sds> <výstupní složka>   # všechny Lua skripty ze všech archivů
  python3 tools/sds.py texts   <složka se .sds> <výstupní složka>   # textové tabulky: GUI, cíle, titulky dialogů

Upravená verze (port čtecí části do Pythonu: výpis, rozbalení resources a Lua skriptů, odšifrování
archivů zabalených přes „tables/fsfh.bin“; jen čtení, bez Oodle) kódu Gibbed.Illusion –
https://github.com/gibbed/Gibbed.Illusion

Copyright (c) 2017 Rick (rick 'at' gibbed 'dot' us)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would
   be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not
   be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.
"""

from __future__ import annotations

import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib
from dataclasses import dataclass
from pathlib import Path

SDS_MAGIC = b"SDS\0"

# Šifrované archivy (ArchiveEncryption z Gibbed.Illusion): vnější SDS obsahuje jen návnadu
# „tables/fsfh.bin“, skutečný archiv leží od 0x10000 zašifrovaný TEA (dvě sady parametrů).
FSFH_MARKER_OFFSET = 0x90
FSFH_MARKER = b"tables/fsfh.bin"
FSFH_DATA_OFFSET = 0x10000
TEA_KEYS = (0x73766E46, 0x6D454D5A, 0x336A6D68, 0x38425072)
TEA_SETTINGS = ((0x79FB0B01, 0x4B989BCD, 5), (0xA62336C0, 0x9D3119B6, 32))


def fnv32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value = ((value * 0x01000193) & 0xFFFFFFFF) ^ byte
    return value


def tea_decrypt(data: bytes, sum_start: int, delta: int, rounds: int) -> bytes:
    """TEA dešifrování všech celých 8B bloků (vektorově přes numpy), zbytek beze změny."""
    import numpy as np

    full = len(data) // 8 * 8
    words = np.frombuffer(data[:full], dtype="<u4").reshape(-1, 2).astype(np.uint32)
    v0, v1 = words[:, 0].copy(), words[:, 1].copy()
    k0, k1, k2, k3 = (np.uint32(k) for k in TEA_KEYS)
    total = sum_start
    with np.errstate(over="ignore"):
        for _ in range(rounds):
            s = np.uint32(total)
            v1 -= ((v0 << np.uint32(4)) + k2) ^ (v0 + s) ^ ((v0 >> np.uint32(5)) + k3)
            v0 -= ((v1 << np.uint32(4)) + k0) ^ (v1 + s) ^ ((v1 >> np.uint32(5)) + k1)
            total = (total - delta) & 0xFFFFFFFF
    return np.stack([v0, v1], axis=1).astype("<u4").tobytes() + data[full:]


def unwrap_encrypted(data: bytes) -> bytes:
    """Vrátí odšifrovaný vnitřní archiv, nebo původní data, pokud archiv šifrovaný není."""
    if data[FSFH_MARKER_OFFSET:FSFH_MARKER_OFFSET + len(FSFH_MARKER)] != FSFH_MARKER:
        return data
    encrypted = data[FSFH_DATA_OFFSET:]
    for settings in TEA_SETTINGS:
        header = tea_decrypt(encrypted[:16], *settings)
        if header[:4] == SDS_MAGIC and struct.unpack_from("<I", header, 12)[0] == fnv32(header[:12]):
            return tea_decrypt(encrypted, *settings)
    raise ValueError("šifrovaný archiv: žádná známá sada TEA parametrů nesedí")


BLOCK_MAGIC = 0x6C7A4555  # 'UEzl'
LUA_MAGIC = b"\x1bLua"


@dataclass
class Resource:
    type_name: str
    version: int
    data: bytes
    name: str


class Reader:
    def __init__(self, data: bytes, pos: int = 0):
        self.data = data
        self.pos = pos

    def take(self, size: int) -> bytes:
        chunk = self.data[self.pos:self.pos + size]
        if len(chunk) != size:
            raise ValueError("neočekávaný konec dat")
        self.pos += size
        return chunk

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def str_u16(self) -> str:
        return self.take(self.u16()).decode("latin-1")

    def str_u32(self) -> str:
        return self.take(self.u32()).decode("latin-1")


def read_blocks(data: bytes, offset: int) -> bytes:
    """Datová část archivu: posloupnost bloků (zlib nebo nekomprimovaných), každý na `alignment` bajtů."""
    r = Reader(data, offset)
    magic, alignment, flags = r.u32(), r.u32(), r.u8()
    if magic != BLOCK_MAGIC or flags != 4:
        raise ValueError("neplatná tabulka bloků")
    out = bytearray()
    while True:
        size = r.u32()
        compressed = r.u8() != 0
        if size == 0:
            break
        if compressed:
            # Hlavička 32 B: rozbalená velikost, velikost hlavičky (32 = zlib), velikost a počet chunků,
            # neznámé u32 a velikosti až 8 samostatně komprimovaných chunků (u16). Jeden chunk (běžný
            # případ) zabírá celý zbytek bloku – jeho velikost se do u16 nemusí vejít.
            uncompressed_size, header_size, _chunk_size, chunk_count, _unknown = struct.unpack("<IIhhI", r.take(16))
            chunk_sizes = struct.unpack("<8H", r.take(16))
            if header_size != 32:
                raise ValueError(f"nepodporovaná komprese (hlavička {header_size} B – Oodle?)")
            payload = r.take(size - header_size)
            if chunk_count <= 1:
                block = zlib.decompress(payload)
            else:
                if sum(chunk_sizes[:chunk_count]) > len(payload):
                    raise ValueError("nesedí velikost komprimovaného bloku")
                parts, start = [], 0
                for chunk in chunk_sizes[:chunk_count]:
                    parts.append(zlib.decompress(payload[start:start + chunk]))
                    start += chunk
                block = b"".join(parts)
            if len(block) != uncompressed_size:
                raise ValueError("nesedí velikost rozbaleného bloku")
        else:
            block = r.take(size)
        out += block
    return bytes(out)


def resource_names(xml_text: str, count: int) -> list[str]:
    """Názvy resources z XML na konci archivu (SourceDataDescription), ve stejném pořadí jako data."""
    names = [""] * count
    try:
        root = ET.fromstring(xml_text.strip("\0").strip())
    except ET.ParseError:
        return names
    for index, info in enumerate(root.iter("ResourceInfo")):
        if index >= count:
            break
        names[index] = (info.findtext("SourceDataDescription") or "").strip()
    return names


def read_archive(path: Path) -> list[Resource]:
    data = unwrap_encrypted(path.read_bytes())
    if data[:4] != SDS_MAGIC:
        raise ValueError(f"{path.name}: není SDS archiv (možná je šifrovaný)")
    version = struct.unpack_from("<I", data, 4)[0]
    if version != 19 or data[8:12] != b"PC\0\0":
        raise ValueError(f"{path.name}: podporována je jen verze 19 pro PC (Mafia II), ne {version}")

    # Za každou „bezpečně“ čtenou hlavičkou následuje 4bajtový FNV32 hash: 12 B + hash, pak 52 B + hash
    header = Reader(data, 16)
    type_table, block_table, xml_offset = header.u32(), header.u32(), header.u32()
    header.take(4 * 5 + 16)  # nároky na RAM/VRAM, flags, neznámých 16 B
    resource_count = header.u32()

    types_reader = Reader(data, type_table)
    types = {}
    for _ in range(types_reader.u32()):
        type_id = types_reader.u32()
        types[type_id] = types_reader.str_u32()
        types_reader.u32()  # parent

    blocks = Reader(read_blocks(data, block_table))
    xml_text = data[xml_offset:].decode("latin-1", errors="replace")
    names = resource_names(xml_text, resource_count)

    resources = []
    for index in range(resource_count):
        type_id, size, res_version = blocks.u32(), blocks.u32(), blocks.u16()
        blocks.take(16)  # nároky na RAM/VRAM
        blocks.take(4)  # FNV32 hash hlavičky resource (26 B + 4 B = 30 B v `size`)
        if size < 30:
            raise ValueError("neplatná velikost resource")
        resources.append(Resource(types.get(type_id, f"type{type_id}"), res_version, blocks.take(size - 30), names[index]))
    return resources


@dataclass
class Script:
    name: str
    data: bytes


def parse_script_resource(resource: Resource) -> tuple[str, list[Script]]:
    """Resource typu Script: cesta balíku a seznam Lua souborů (většinou zkompilovaný bytecode Lua 5.1)."""
    r = Reader(resource.data)
    package_path = r.str_u16()
    scripts = []
    for _ in range(r.u32()):
        if resource.version == 2:
            r.u64()  # hash názvu
            r.u64()  # hash dat
        name = r.str_u16()
        scripts.append(Script(name, r.take(r.u32())))
    return package_path, scripts


def parse_memfile(resource: Resource) -> tuple[str, bytes]:
    """Resource typu MemFile: vložený soubor (`/tables/TextDatabase.dat`, titulky `…_subtitles.bin`…)."""
    r = Reader(resource.data)
    name = r.str_u32()
    r.u32()  # verze
    return name, r.take(r.u32())


def parse_sound(resource: Resource) -> tuple[str, bytes]:
    """Resource typu Sound: název a zvuk ve formátu FMOD FSB4 (převod na WAV např. vgmstream-cli)."""
    r = Reader(resource.data)
    name = r.take(r.u8()).decode("latin-1")
    return name, r.take(r.u32())


def text_lines(data: bytes) -> list[tuple[str, str]] | None:
    """Textová tabulka hry (UTF-8 s BOM, řádky `ID:text`) – texty GUI, cíle misí i titulky dialogů."""
    if not data.startswith(b"\xef\xbb\xbf"):
        return None
    rows = []
    for line in data[3:].decode("utf-8", errors="replace").splitlines():
        key, sep, text = line.partition(":")
        if sep and key.strip():
            rows.append((key.strip(), text))
    return rows or None


SAFE = re.compile(r"[^A-Za-z0-9._/-]+")


def safe_path(name: str) -> Path:
    """Cesty z archivu (s \\ a disky) na bezpečnou relativní cestu."""
    cleaned = SAFE.sub("_", name.replace("\\", "/").replace(":", "")).strip("/")
    parts = [part for part in cleaned.split("/") if part not in ("", ".", "..")]
    return Path(*parts) if parts else Path("unnamed")


def cmd_list(archive: Path) -> None:
    for index, res in enumerate(read_archive(archive)):
        print(f"{index:4d}  {res.type_name:<22} v{res.version:<3} {len(res.data):>10} B  {res.name}")
        if res.type_name == "Script":
            _, scripts = parse_script_resource(res)
            for script in scripts:
                kind = "bytecode" if script.data.startswith(LUA_MAGIC) else "text"
                print(f"        └ {script.name}  ({len(script.data)} B, {kind})")


def extract_scripts(archive: Path, out_dir: Path) -> int:
    count = 0
    for res in read_archive(archive):
        if res.type_name != "Script":
            continue
        _, scripts = parse_script_resource(res)
        for script in scripts:
            target = out_dir / safe_path(script.name)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(script.data)
            count += 1
    return count


def cmd_extract(archive: Path, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for index, res in enumerate(read_archive(archive)):
        base = safe_path(res.name) if res.name else Path(f"{index:04d}")
        target = out_dir / res.type_name / f"{index:04d}_{base.name}.bin"
        data = res.data
        # Vložené soubory a zvuky bez obalu resource, pod svým názvem (.dat, .bin, .fsb)
        if res.type_name == "MemFile":
            name, data = parse_memfile(res)
            target = out_dir / res.type_name / safe_path(name)
        elif res.type_name == "Sound":
            name, data = parse_sound(res)
            target = out_dir / res.type_name / f"{safe_path(name)}.fsb"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    scripts = extract_scripts(archive, out_dir / "lua")
    print(f"{archive.name}: rozbaleno do {out_dir} (skriptů: {scripts})")


def cmd_scripts(sds_dir: Path, out_dir: Path) -> None:
    total = 0
    for archive in sorted(sds_dir.rglob("*.sds")):
        try:
            count = extract_scripts(archive, out_dir)
        except (ValueError, zlib.error) as error:
            print(f"přeskočeno {archive.relative_to(sds_dir)}: {error}", file=sys.stderr)
            continue
        if count:
            print(f"{archive.relative_to(sds_dir)}: {count} skriptů")
            total += count
    print(f"Celkem {total} skriptů do {out_dir}")


def cmd_texts(sds_dir: Path, out_dir: Path) -> None:
    """Všechny textové tabulky (MemFile s řádky `ID:text`) jako .txt + souhrnný all_texts.tsv."""
    out_dir.mkdir(parents=True, exist_ok=True)
    rows: dict[str, tuple[str, str]] = {}
    tables = 0
    for archive in sorted(sds_dir.rglob("*.sds")):
        try:
            resources = read_archive(archive)
        except (ValueError, zlib.error) as error:
            print(f"přeskočeno {archive.relative_to(sds_dir)}: {error}", file=sys.stderr)
            continue
        for res in resources:
            if res.type_name != "MemFile":
                continue
            name, data = parse_memfile(res)
            lines = text_lines(data)
            if not lines:
                continue
            target = out_dir / f"{safe_path(name)}.txt"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("\n".join(f"{key}:{text}" for key, text in lines) + "\n", encoding="utf-8")
            tables += 1
            for key, text in lines:
                rows.setdefault(key, (text, name))
    with (out_dir / "all_texts.tsv").open("w", encoding="utf-8") as tsv:
        tsv.write("id\ttext\tsource\n")
        for key, (text, source) in sorted(rows.items()):
            tsv.write(f"{key}\t{text.replace(chr(9), ' ')}\t{source}\n")
    print(f"Textových tabulek: {tables}, unikátních textů: {len(rows)} → {out_dir}/all_texts.tsv")


def main(argv: list[str]) -> None:
    if len(argv) == 3 and argv[1] == "list":
        cmd_list(Path(argv[2]))
    elif len(argv) == 4 and argv[1] == "extract":
        cmd_extract(Path(argv[2]), Path(argv[3]))
    elif len(argv) == 4 and argv[1] == "scripts":
        cmd_scripts(Path(argv[2]), Path(argv[3]))
    elif len(argv) == 4 and argv[1] == "texts":
        cmd_texts(Path(argv[2]), Path(argv[3]))
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv)
