#!/usr/bin/env python3
"""Plot OBSERVED crowdbench unit positions (never planned paths) for visual review.

    crowdbench --mode M --scenario S --units N --ticks T --trace run.trace.csv
    navigation_plot.py run.trace.csv [more traces...] --output sheet.png \\
        [--ticks 0 600 1500 3000 6000] [--heatmap] [--frames DIR] [--crop X0 Z0 X1 Z1]

Each input is the --trace path (the .units.csv and .walls.csv sidecars are read
next to it; passing the .units.csv itself also works). The contact sheet has one
row per trace and one column per tick; --heatmap adds a column of where units
spent their time (30-tick samples, sqrt scale), which shows clumps, door-mouth
queues and lanes. --frames also writes one PNG per trace and tick. --crop
restricts every panel to a window in map CELLS (16 px each).

Colours (state at that tick, from the trace -- not the benchmark's arrival rule):
  light grey  never commanded (the non-moving share of the population)
  blue    moving (speed > 0)
  purple  stopped with orders, waiting for a queued route (pending search)
  red     stopped with orders, no route pending (stalled / blocked)
  green   orders retired (stopped; legality and goal area not checked here)
Grey: barriers in force at that tick. Black cross/ring: goals (<= 4 distinct).
Dark lines: the last 600 ticks of a sample of units. Unit samples are written on
control changes and every 30 ticks, so multiples of 30 show every unit exactly.
Needs Pillow (PIL); no matplotlib.
"""
import argparse
import csv
from pathlib import Path

STATE_COLOURS = {"idle": (185, 185, 185), "moving": (44, 111, 187), "pending": (142, 68, 173), "stalled": (208, 49, 45),
                 "retired": (46, 155, 79)}
WALL = (120, 120, 120)
BACKGROUND = (250, 250, 248)
HEAT_STOPS = ((0, (0, 0, 4)), (.25, (87, 16, 110)), (.5, (188, 55, 84)), (.75, (249, 142, 9)),
              (1, (252, 255, 164)))


def trace_paths(path):
    text = str(path)
    if text.endswith(".units.csv"):
        text = text[: -len(".units.csv")]
    return Path(text + ".units.csv"), Path(text + ".walls.csv")


def load_walls(path):
    """[(tick, [rects])] in pixels, and the map extent in pixels."""
    sets, extent = [], None
    if not path.exists():
        return sets, extent
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            x, z, w, h = (int(row[k]) * 16 for k in ("x", "z", "w", "h"))
            if row["kind"] == "map":
                extent = (w, h)
            elif row["kind"] == "set":
                sets.append((int(row["tick"]), []))
            elif row["kind"] == "wall":
                sets[-1][1].append((x, z, w, h))
    return sets, extent


def walls_at(sets, tick):
    current = []
    for start, rects in sets:
        if start <= tick:
            current = rects
    return current


def state_of(sample):
    if int(sample["epoch"]) == 0:
        return "idle"
    if int(sample["orders"]) == 0:
        return "retired"
    if int(sample["speed_raw"]) != 0:
        return "moving"
    return "pending" if sample["pending"] in ("1", "true") else "stalled"


def load_units(path, ticks, heat_bin):
    snapshots = {}
    trajectories, heat, goals = {}, {}, set()
    queue = sorted(set(ticks))
    current = {}
    extent = [0, 0]
    with path.open(newline="") as handle:
        for sample in csv.DictReader(handle):
            tick, unit = int(sample["tick"]), int(sample["id"])
            while queue and tick > queue[0]:
                snapshots[queue.pop(0)] = dict(current)
            x, z = int(sample["x_raw"]) / 65536, int(sample["z_raw"]) / 65536
            extent = [max(extent[0], x + 64), max(extent[1], z + 64)]
            current[unit] = (x, z, state_of(sample), int(sample["foot_x"]) * 16, int(sample["foot_z"]) * 16)
            trajectories.setdefault(unit, []).append((tick, x, z))
            goals.add((float(sample["goal_x"]), float(sample["goal_z"]), float(sample["goal_radius"])))
            if heat_bin and tick % 30 == 0:
                key = (int(x // heat_bin), int(z // heat_bin))
                heat[key] = heat.get(key, 0) + 1
    for tick in queue:
        snapshots[tick] = dict(current)
    return snapshots, trajectories, heat, goals, tuple(extent)


def heat_colour(value):
    for (a, ca), (b, cb) in zip(HEAT_STOPS, HEAT_STOPS[1:]):
        if value <= b:
            t = (value - a) / (b - a)
            return tuple(int(ca[i] + (cb[i] - ca[i]) * t) for i in range(3))
    return HEAT_STOPS[-1][1]


class Panel:
    def __init__(self, window, width):
        self.x0, self.z0, self.x1, self.z1 = window
        self.scale = width / (self.x1 - self.x0)
        self.size = (width, max(8, int((self.z1 - self.z0) * self.scale)))

    def point(self, x, z):
        return ((x - self.x0) * self.scale, (z - self.z0) * self.scale)

    def rect(self, x, z, w, h, minimum=1.0):
        px, pz = self.point(x, z)
        return [px, pz, px + max(minimum, w * self.scale) - 1, pz + max(minimum, h * self.scale) - 1]


def render(row, tick, window, width, font, heat_bin=0):
    from PIL import Image, ImageDraw
    label, sets, snapshots, trajectories, heat, goals, sample = row
    panel = Panel(window, width)
    title_height = 30
    image = Image.new("RGB", (panel.size[0], panel.size[1] + title_height), BACKGROUND)
    draw = ImageDraw.Draw(image)
    offset = lambda box: [box[0], box[1] + title_height, box[2], box[3] + title_height]
    if tick is None:
        draw.rectangle([0, title_height, *image.size], fill=HEAT_STOPS[0][1])
        peak = max(heat.values()) if heat else 1
        for (bx, bz), count in heat.items():
            draw.rectangle(offset(panel.rect(bx * heat_bin, bz * heat_bin, heat_bin, heat_bin)),
                           fill=heat_colour((count / peak) ** .5))
        for x, z, w, h in walls_at(sets, 1 << 30):
            draw.rectangle(offset(panel.rect(x, z, w, h)), outline=WALL)
        title = f"{label}\ntime-in-place, sqrt scale, all ticks"
    else:
        for x, z, w, h in walls_at(sets, tick):
            draw.rectangle(offset(panel.rect(x, z, w, h)), fill=WALL)
        for unit in sample:
            points = [panel.point(x, z) for t, x, z in trajectories.get(unit, ()) if tick - 600 <= t <= tick]
            if len(points) > 1:
                draw.line([(px, pz + title_height) for px, pz in points], fill=(40, 40, 40), width=1)
        counts = dict.fromkeys(STATE_COLOURS, 0)
        for x, z, state, w, h in snapshots[tick].values():
            counts[state] += 1
            draw.rectangle(offset(panel.rect(x - w / 2, z - h / 2, w, h, 1.5)), fill=STATE_COLOURS[state])
        if len(goals) <= 4:
            for gx, gz, radius in goals:
                px, pz = panel.point(gx, gz)
                pz += title_height
                r = radius * panel.scale
                draw.ellipse([px - r, pz - r, px + r, pz + r], outline=(0, 0, 0))
                draw.line([px - 4, pz - 4, px + 4, pz + 4], fill=(0, 0, 0), width=2)
                draw.line([px - 4, pz + 4, px + 4, pz - 4], fill=(0, 0, 0), width=2)
        title = f"{label}  tick {tick}\n" + "  ".join(f"{k} {v}" for k, v in counts.items())
    draw.multiline_text((3, 1), title, fill=(0, 0, 0), font=font, spacing=1)
    draw.rectangle([0, 0, image.size[0] - 1, image.size[1] - 1], outline=(200, 200, 200))
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path, help="contact sheet PNG")
    parser.add_argument("--ticks", nargs="+", type=int, default=[0, 600, 1500, 3000, 6000])
    parser.add_argument("--labels", nargs="+", help="row titles (default: trace file names)")
    parser.add_argument("--heatmap", action="store_true", help="add a time-in-place column")
    parser.add_argument("--frames", type=Path, help="also write one PNG per trace and tick here")
    parser.add_argument("--crop", nargs=4, type=float, metavar=("X0", "Z0", "X1", "Z1"),
                        help="window in map cells (16 px)")
    parser.add_argument("--trajectories", type=int, default=16, help="sampled units drawn with recent paths")
    parser.add_argument("--panel-width", type=int, default=520)
    parser.add_argument("--title", default="")
    args = parser.parse_args()
    from PIL import Image, ImageDraw, ImageFont
    try:
        font = ImageFont.load_default(size=11)
    except TypeError:
        font = ImageFont.load_default()
    heat_bin = 32 if args.heatmap else 0
    rows, extent = [], None
    for index, path in enumerate(args.traces):
        units_path, walls_path = trace_paths(path)
        sets, map_extent = load_walls(walls_path)
        snapshots, trajectories, heat, goals, seen = load_units(units_path, args.ticks, heat_bin)
        extent = map_extent or extent or seen
        ids = sorted(trajectories)
        step = max(1, len(ids) // max(1, args.trajectories))
        label = args.labels[index] if args.labels and index < len(args.labels) else \
            units_path.name.replace(".trace.csv.units.csv", "").replace(".units.csv", "")
        rows.append((label, sets, snapshots, trajectories, heat, goals, ids[::step][: args.trajectories]))
    window = tuple(v * 16 for v in args.crop) if args.crop else (0, 0, extent[0], extent[1])
    panels = []
    for row in rows:
        line = [render(row, tick, window, args.panel_width, font) for tick in args.ticks]
        if args.heatmap:
            line.append(render(row, None, window, args.panel_width, font, heat_bin))
        if args.frames:
            args.frames.mkdir(parents=True, exist_ok=True)
            for tick, image in zip(args.ticks, line):
                image.save(args.frames / f"{row[0]}-t{tick:05d}.png")
        panels.append(line)
    header = 36
    cell_w = max(p.size[0] for line in panels for p in line)
    cell_h = max(p.size[1] for line in panels for p in line)
    sheet = Image.new("RGB", (cell_w * len(panels[0]), header + cell_h * len(panels)), (255, 255, 255))
    for r, line in enumerate(panels):
        for c, image in enumerate(line):
            sheet.paste(image, (c * cell_w, header + r * cell_h))
    legend = ("light grey never commanded (static bodies) | blue moving | purple waiting for route | red stopped with orders | green orders retired "
              "(not the arrival rule) | grey barriers | lines: recent paths of sampled units")
    ImageDraw.Draw(sheet).multiline_text((4, 2), (args.title + "\n" if args.title else "") + legend,
                                         fill=(0, 0, 0), font=font)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.output)
    print(f"wrote {args.output} ({sheet.size[0]}x{sheet.size[1]})")


if __name__ == "__main__":
    main()
