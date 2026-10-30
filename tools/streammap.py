#!/usr/bin/env python3
"""Výpis a úprava StreamMapa.bin z Mafie II – co se načte při aktivaci řádku streamování.

  python3 tools/streammap.py lines  <StreamMapa.bin> [skupina]
  python3 tools/streammap.py active <StreamMapa.bin> <řádek>
  python3 tools/streammap.py make-line <StreamMapa.bin> <výstup> <řádek> <vzor> <sds> [<sds> …]

`make-line` přenastaví rozsahy loaderů tak, aby `game.sds:ActivateStreamMapLine(<řádek>)` načetl
přesně to, co je aktivní na řádku <vzor> (např. freeride M2O „free_joe_load“), plus uvedené SDS
(např. geometrii Sicílie) – bez skriptu mise, videí a postav. Mění jen čísla Start/End existujících
loaderů: chtěný loader, který řádek nepokrývá, se roztáhne až k němu (vybere se ten aktivní na
vzoru, jinak nejbližší); nechtěný se zkrátí, pokud řádek leží na okraji jeho rozsahu. Řádky mezi
tím mohou načítat víc než v původní hře – v M2O se nepoužívají.

Formát (magic „StrM“, verze 6; podle MIT knihovny illusion-toolkit): hlavička s počty a offsety
polí (skupiny @24/28, řádky @32/36, loadery @40/44, tabulka řetězců @68); řádek 56 B (jméno, ID,
skupina, …), loader 32 B (Start, End, typ, …, cesta @24, entita @28).
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path

MAGIC = 0x4D727453  # "StrM"
LINE_SIZE = 56
LOADER_SIZE = 32


@dataclass
class Line:
    name: str
    line_id: int
    group: int


@dataclass
class Loader:
    start: int
    end: int
    type_id: int
    path: str
    entity: str
    offset: int  # pozice záznamu v souboru

    def covers(self, line_id: int) -> bool:
        return self.start <= line_id <= self.end


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def parse(data: bytes) -> tuple[list[str], list[Line], list[Loader]]:
    if i32(data, 0) != MAGIC or i32(data, 4) != 6:
        raise ValueError("není StreamMapa.bin verze 6")
    pool = i32(data, 68)

    def text(offset: int) -> str:
        start = pool + offset
        return data[start:data.index(b"\0", start)].decode("latin1")

    groups = [text(struct.unpack_from("<q", data, i32(data, 28) + 8 * i)[0]) for i in range(i32(data, 24))]
    lines = []
    for i in range(i32(data, 32)):
        at = i32(data, 36) + LINE_SIZE * i
        lines.append(Line(text(i32(data, at)), i32(data, at + 4), i32(data, at + 8)))
    loaders = []
    for i in range(i32(data, 40)):
        at = i32(data, 44) + LOADER_SIZE * i
        loaders.append(Loader(i32(data, at), i32(data, at + 4), i32(data, at + 8), text(i32(data, at + 24)),
                              text(i32(data, at + 28)), at))
    return groups, lines, loaders


def line_id(lines: list[Line], name: str) -> int:
    for line in lines:
        if line.name == name:
            return line.line_id
    raise SystemExit(f"řádek {name!r} neexistuje")


def active(loaders: list[Loader], line: int) -> set[tuple[int, str]]:
    return {(loader.type_id, loader.path) for loader in loaders if loader.covers(line)}


def cmd_lines(path: Path, group: str | None) -> None:
    groups, lines, _ = parse(path.read_bytes())
    for line in lines:
        if group is None or groups[line.group] == group:
            print(f"{line.line_id:5}  {groups[line.group]:24}  {line.name}")


def cmd_active(path: Path, name: str) -> None:
    _, lines, loaders = parse(path.read_bytes())
    target = line_id(lines, name)
    for type_id, sds in sorted(active(loaders, target)):
        print(f"{type_id:3}  {sds}")


def cmd_make_line(path: Path, out: Path, name: str, like: str, changes: list[str]) -> None:
    data = bytearray(path.read_bytes())
    _, lines, loaders = parse(bytes(data))
    original = parse(bytes(data))[2]
    target, model = line_id(lines, name), line_id(lines, like)
    known = {loader.path for loader in loaders}
    add = [c[1:] for c in changes if c.startswith("+")]
    drop = [c[1:] for c in changes if c.startswith("-")]
    for sds in add + drop:
        if sds not in known:
            raise SystemExit(f"{sds} v StreamMapa.bin není (cesta musí sedět, např. /sds/city/sicily01.sds)")
    wanted = {(t, p) for t, p in active(loaders, model) if p not in drop}
    wanted |= {(l.type_id, l.path) for l in loaders if l.path in add}

    def write(loader: Loader, start: int, end: int) -> None:
        struct.pack_into("<ii", data, loader.offset, start, end)
        loader.start, loader.end = start, end

    # Nechtěné pryč z řádku – jde jen na okraji rozsahu
    for loader in loaders:
        if loader.covers(target) and (loader.type_id, loader.path) not in wanted:
            if loader.start == loader.end:
                write(loader, -1, -1)
            elif loader.end == target:
                write(loader, loader.start, target - 1)
            elif loader.start == target:
                write(loader, target + 1, loader.end)
            else:
                raise SystemExit(f"{loader.path} ({loader.start}–{loader.end}) nejde z řádku {target} vyjmout "
                                 "– zvol řádek na okraji rozsahů (např. poslední řádek skupiny)")
    # Chtěné na řádek – roztáhne se jeden loader (přednostně ten aktivní na vzoru)
    # Roztažení nesmí pokud možno zasáhnout řádky skupiny vzoru (freeride M2O načítá řádky postupně)
    model_group = next(l.group for l in lines if l.line_id == model)
    group_ids = [l.line_id for l in lines if l.group == model_group]

    def touches_group(loader: Loader) -> int:
        start, end = min(loader.start, target), max(loader.end, target)
        return sum(1 for i in group_ids if start <= i <= end and not loader.covers(i))

    for type_id, sds in sorted(wanted - active(loaders, target)):
        candidates = [l for l in loaders if (l.type_id, l.path) == (type_id, sds)]
        best = min(candidates, key=lambda l: (touches_group(l), min(abs(l.start - target), abs(l.end - target))))
        write(best, min(best.start, target), max(best.end, target))

    patched = parse(bytes(data))[2]
    result = active(patched, target)
    if result != wanted:
        raise SystemExit(f"nesedí: navíc {sorted(result - wanted)}, chybí {sorted(wanted - result)}")
    if active(patched, model) != active(original, model):
        raise SystemExit(f"úprava by změnila vzorový řádek {like} – zvol jiný cílový řádek")
    for line in lines:
        before, after = active(original, line.line_id), active(patched, line.line_id)
        if line.line_id != target and before != after:
            print(f"  pozn.: mění se i řádek {line.name} ({line.line_id}): +{len(after - before)} −{len(before - after)}")
    out.write_bytes(data)
    print(f"{name} (řádek {target}) teď načte {len(result)} SDS: vzor {like} {' '.join(changes)}")


def main(argv: list[str]) -> None:
    if len(argv) in (3, 4) and argv[1] == "lines":
        cmd_lines(Path(argv[2]), argv[3] if len(argv) == 4 else None)
    elif len(argv) == 4 and argv[1] == "active":
        cmd_active(Path(argv[2]), argv[3])
    elif len(argv) >= 7 and argv[1] == "make-line" and all(c[:1] in "+-" for c in argv[6:]):
        cmd_make_line(Path(argv[2]), Path(argv[3]), argv[4], argv[5], argv[6:])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
