#!/usr/bin/env python3
"""Výpis a úprava edit/sdsconfig.bin z Mafie II – rozpočty paměťových slotů pro SDS archivy.

  python3 tools/sdsconfig.py show  <sdsconfig.bin>
  python3 tools/sdsconfig.py count <sdsconfig.bin> <skupina> <počet>   # např. city 4 (záloha .orig)

Hra si při startu předalokuje pro každou skupinu (city, city_ground, shop…) daný počet slotů.
Svět M2O (Empire Bay) drží obě městská místa („city“ ×2), takže načtení Sicílie (sicily01 + sicily02)
skončí nedostatkem paměti. Víc slotů = víc paměti při startu (city: ~11 MB + ~40 MB na slot).

Formát (verze 2, „psds“): u32 magic, u32 verze, u32 délka tabulky řetězců, tabulka řetězců
(bajty negované, oddělené nulou), i16 počet šablon; šablona = i16 jméno, i16 počet skupin,
skupiny (i16 jméno, u16 počet, u16 typ, u16 priorita, u32 paměť, u32 aux), i16 počet virtuálních
slotů, slot = i16 jméno, u32, u32, i16 počet položek, položka = i16 jméno, u32, u32, u8 příznak,
i16 počet referencí, reference = jako skupina.
"""
from __future__ import annotations

import shutil
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"psds"
VERSION = 2


@dataclass
class Group:
    name: str
    count: int
    type_id: int
    priority: int
    memory: int
    aux: int
    count_offset: int  # pozice u16 počtu v souboru (pro úpravu)


class Reader:
    def __init__(self, data: bytes, pos: int) -> None:
        self.data, self.pos = data, pos

    def take(self, fmt: str) -> int:
        value = struct.unpack_from(fmt, self.data, self.pos)[0]
        self.pos += struct.calcsize(fmt)
        return value


def parse(data: bytes) -> tuple[list[Group], list[tuple[str, int, int, list]]]:
    if data[:4] != MAGIC or struct.unpack_from("<I", data, 4)[0] != VERSION:
        raise ValueError("není sdsconfig.bin verze 2")
    pool_size = struct.unpack_from("<I", data, 8)[0]
    pool = bytes(~b & 0xFF for b in data[12:12 + pool_size])

    def name(offset: int) -> str:
        return pool[offset:pool.index(b"\0", offset)].decode("latin1")

    r = Reader(data, 12 + pool_size)

    def group() -> Group:
        name_offset = r.take("<h")
        count_offset = r.pos
        return Group(name(name_offset), r.take("<H"), r.take("<H"), r.take("<H"), r.take("<I"), r.take("<I"),
                     count_offset)

    groups: list[Group] = []
    slots: list[tuple[str, int, int, list]] = []
    for _ in range(r.take("<h")):
        r.take("<h")  # jméno šablony („default“)
        groups += [group() for _ in range(r.take("<h"))]
        for _ in range(r.take("<h")):
            slot_name, budget1, budget2 = name(r.take("<h")), r.take("<I"), r.take("<I")
            items = []
            for _ in range(r.take("<h")):
                item_name, _size1, _size2, flags = name(r.take("<h")), r.take("<I"), r.take("<I"), r.take("<B")
                refs = [group() for _ in range(r.take("<h"))]
                items.append((item_name, flags, refs))
            slots.append((slot_name, budget1, budget2, items))
    if r.pos != len(data):
        raise ValueError(f"nečekaná délka souboru ({r.pos} != {len(data)})")
    return groups, slots


def cmd_show(path: Path) -> None:
    groups, slots = parse(path.read_bytes())
    for g in groups:
        print(f"{g.name:28} ×{g.count}  typ {g.type_id:2}  priorita {g.priority:3}  "
              f"paměť {g.memory / 1048576:7.2f} MB  aux {g.aux / 1048576:7.2f} MB")
    for slot_name, budget1, budget2, items in slots:
        print(f"virtuální slot {slot_name}: {budget1 / 1048576:.2f} MB / {budget2 / 1048576:.2f} MB")
        for item_name, flags, refs in items:
            layout = ", ".join(f"{ref.name} ×{ref.count}" for ref in refs)
            print(f"  {'*' if flags == 1 else ' '} {item_name}: {layout}")


def cmd_count(path: Path, group_name: str, count: int) -> None:
    data = bytearray(path.read_bytes())
    groups, _ = parse(bytes(data))
    group = next((g for g in groups if g.name == group_name), None)
    if group is None:
        raise SystemExit(f"skupina {group_name!r} v souboru není")
    backup = path.with_name(path.name + ".orig")
    if not backup.exists():
        shutil.copy2(path, backup)
    struct.pack_into("<H", data, group.count_offset, count)
    path.write_bytes(data)
    print(f"{group_name}: {group.count} → {count} slotů (záloha {backup.name})")


def main(argv: list[str]) -> None:
    if len(argv) == 3 and argv[1] == "show":
        cmd_show(Path(argv[2]))
    elif len(argv) == 5 and argv[1] == "count":
        cmd_count(Path(argv[2]), argv[3], int(argv[4]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
