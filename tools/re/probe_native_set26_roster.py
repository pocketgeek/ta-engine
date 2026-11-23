#!/usr/bin/env python3
"""Audit shipped SET26 death callbacks through native owner teardown.

The roster finder selects literal ``SET_UNIT_VALUE(26, 1)`` writes in each
COB's ``Dying`` function. For each writer, the existing headless ICD fixture
starts that script's native Killed/Dying handlers, advances the real 0x51d3e0
owner updater, and checks the next-update owner/list teardown. No retail GUI is
launched. `crebomb` has mutually exclusive fast and delayed writers; both
branches are run by varying its queried unit value 17.

Example:
    PYTHONPATH=tools/re python3 tools/re/probe_native_set26_roster.py \
      --scripts /tmp/tak-native-scripts/scripts --world-binary build-o2/animation_roster_test
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
import re
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "tools/re/probe_native_set31_lifecycle.py"
SET26_PATTERN = (0x10021001, 26, 0x10021001, 1, 0x10082000)
WRITE_RE = re.compile(r"writes SET26 at tick (\d+); .* at tick (\d+)")


@dataclass(frozen=True)
class Writer:
    path: Path
    sites: tuple[int, ...]


def cob_functions(path: Path, ops: dict[int, tuple[str, int]]):
    data = path.read_bytes()
    header = struct.unpack_from("<10I", data)
    count, code_words, index_offset, name_offset, code_offset = (
        header[1], header[3], header[6], header[7], header[9]
    )
    names = []
    for index in range(count):
        offset = struct.unpack_from("<I", data, name_offset + index * 4)[0]
        names.append(data[offset:data.index(b"\0", offset)].decode("ascii"))
    starts = struct.unpack_from(f"<{count}I", data, index_offset)
    code = struct.unpack_from(f"<{code_words}I", data, code_offset)
    ordered_starts = sorted(set(starts) | {code_words})

    functions = []
    for index, name in enumerate(names):
        if name.lower() != "dying":
            continue
        start = starts[index]
        end = next((value for value in ordered_starts if value > start), code_words)
        pc = start
        sites = []
        while pc < end:
            instruction = code[pc]
            if instruction not in ops:
                raise ValueError(f"{path.name}: unknown COB opcode {instruction:#x} at word {pc}")
            if instruction == 0x10082000 and pc - start >= 4 and \
                    code[pc - 4:pc + 1] == SET26_PATTERN:
                sites.append(pc)
            pc += 1 + ops[instruction][1]
        functions.append(tuple(sites))
    return tuple(site for function in functions for site in function)


def inventory(scripts: Path) -> tuple[list[Writer], list[str]]:
    metadata = (ROOT / "src/cob/cob.cpp").read_text()
    ops = {int(op, 16): (name, int(argc)) for op, name, argc in re.findall(
        r'\{(0x[0-9A-Fa-f]+),\s*"([^"]+)",\s*(\d+)\}', metadata
    )}
    writers = []
    nonwriters = []
    for path in sorted(scripts.glob("*.cob")):
        sites = cob_functions(path, ops)
        if sites:
            writers.append(Writer(path, sites))
        else:
            nonwriters.append(path.name)
    return writers, nonwriters


def run_writer(writer: Writer, scripts: Path, binary: Path, value17: int):
    command = [
        sys.executable, str(PROBE), "--native-set26-roster", "--scripts", str(scripts),
        "--script", writer.path.stem, "--world-binary", str(binary),
        "--get-unit-value-17", str(value17),
    ]
    result = subprocess.run(command, text=True, capture_output=True, timeout=30)
    output = result.stdout + result.stderr
    if result.returncode:
        return writer, value17, None, output.strip()
    if output.startswith("NO SET26:"):
        return writer, value17, None, output.strip()
    match = WRITE_RE.search(output)
    if not match:
        return writer, value17, None, f"unrecognized probe result: {output.strip()}"
    write_tick, retirement_tick = map(int, match.groups())
    return writer, value17, (write_tick, retirement_tick), output.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scripts", type=Path, default=ROOT / "assets/extracted/all/scripts")
    parser.add_argument("--world-binary", type=Path, default=ROOT / "build-o2/animation_roster_test")
    parser.add_argument("--workers", type=int, default=3)
    args = parser.parse_args()
    scripts = args.scripts.resolve()
    binary = args.world_binary.resolve()
    if not scripts.is_dir():
        parser.error(f"script directory does not exist: {scripts}")
    if not binary.is_file():
        parser.error(f"animation_roster_test does not exist: {binary}")

    writers, nonwriters = inventory(scripts)
    total_sites = sum(len(writer.sites) for writer in writers)
    print(f"Static Dying inventory: {len(writers)} scripts, {total_sites} SET26 sites, "
          f"{len(nonwriters)} scripts without a literal SET26 site")
    print("Writer site classes:", ", ".join(
        f"{writer.path.name}={len(writer.sites)}" for writer in writers if len(writer.sites) > 1
    ) or "all remaining scripts have one site")
    print("No literal SET26 writer:", ", ".join(Path(name).stem for name in nonwriters))

    jobs = [(writer, 0) for writer in writers]
    # Two mutually exclusive cremomb sites share one handler. GET_UNIT_VALUE 17
    # defaults to zero in the fixture (the delayed countdown branch); one tests
    # its immediate branch as well.
    crebomb = next((writer for writer in writers if writer.path.stem.lower() == "crebomb"), None)
    if crebomb:
        jobs.append((crebomb, 1))

    results = []
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = [pool.submit(run_writer, writer, scripts, binary, value17)
                   for writer, value17 in jobs]
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            writer, value17, timing, detail = result
            if timing:
                print(f"PASS {writer.path.name} value17={value17}: "
                      f"write {timing[0]}, retire {timing[1]}", flush=True)
            elif detail.startswith("NO SET26:"):
                print(f"NO RUNTIME WRITE {writer.path.name} value17={value17}: {detail}", flush=True)
            else:
                print(f"FAIL {writer.path.name} value17={value17}: {detail}", flush=True)

    failures = [result for result in results if result[2] is None and
                not result[3].startswith("NO SET26:")]
    no_writes = [result for result in results if result[2] is None and result[3].startswith("NO SET26:")]
    timings = [result for result in results if result[2] is not None]
    offsets = {}
    for _, value17, (write_tick, retire_tick), _ in timings:
        offsets.setdefault((write_tick, retire_tick), []).append(value17)
    print(f"Runtime result: {len(timings)} writer runs passed, {len(no_writes)} candidate runs "
          f"did not write SET26 under the default native host profile, {len(failures)} errors")
    print("Distinct write/retirement offsets:", "; ".join(
        f"{write}/{retire}: {len(values)} run(s)" for (write, retire), values in sorted(offsets.items())
    ))
    return bool(failures or no_writes)


if __name__ == "__main__":
    raise SystemExit(main())
