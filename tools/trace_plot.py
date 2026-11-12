"""Draw traced plays from rbmod.log as top-down PNG pictures.

The step 1 script's Trace section logs "s1: TS" at the snap, then "s1: TB" (ball)
and "s1: TP" (players) snapshots. Each traced play becomes one PNG, with the
offense always attacking to the right.

    python tools/trace_plot.py path/to/rbmod.log [out_dir] [--last N]
    python tools/trace_plot.py --selftest
"""
import os
import re
import sys

from PIL import Image, ImageDraw

LINE = re.compile(r"^\[(\d+)\] script: s1: (TS|TB|TP)\s*(.*)$")
COLORS = {1: (230, 180, 0), 2: (235, 110, 20), 3: (40, 110, 230), 4: (120, 120, 120), 5: (210, 40, 40)}
NAMES = {1: "QB", 2: "back", 3: "receiver", 4: "lineman", 5: "defender"}


def parse(lines):
    """Return a list of plays: {stamp, los, fwd, top, bottom, down, ball: [(t,x,y,state)], paths: {(role,i): [(t,x,y)]}}."""
    plays = []
    for raw in lines:
        m = LINE.match(raw.strip())
        if not m:
            continue
        kind, vals = m.group(2), [float(v) for v in m.group(3).split()]
        if kind == "TS":
            los, fwd, top, bottom, down = vals
            plays.append({"stamp": int(m.group(1)), "los": los, "fwd": fwd, "top": top, "bottom": bottom,
                          "down": int(down), "ball": [], "paths": {}})
        elif plays and kind == "TB":
            plays[-1]["ball"].append(tuple(vals))
        elif plays and kind == "TP":
            t, role, i, x, y = vals
            plays[-1]["paths"].setdefault((int(role), int(i)), []).append((t, x, y))
    return plays


def draw(play, path, scale=2.0):
    pts = [(x, y) for p in play["paths"].values() for _, x, y in p] + [(x, y) for _, x, y, _ in play["ball"]]
    xs = [x for x, _ in pts] + [play["los"]]
    pad = 40
    x0, x1 = min(xs) - pad, max(xs) + pad
    y0, y1 = play["top"] - 10, play["bottom"] + 10
    flip = play["fwd"] < 0  # mirror so the offense always goes to the right

    def px(x, y):
        xx = (x1 - x) if flip else (x - x0)
        return xx * scale, (y - y0) * scale

    w, h = int((x1 - x0) * scale), int((y1 - y0) * scale) + 40
    img = Image.new("RGB", (w, h), (40, 120, 50))
    d = ImageDraw.Draw(img)
    d.rectangle([0, (play["top"] - y0) * scale, w - 1, (play["bottom"] - y0) * scale], outline=(255, 255, 255), width=2)
    lx = px(play["los"], 0)[0]
    d.line([lx, 0, lx, h - 40], fill=(255, 255, 255), width=2)
    for (role, _), p in sorted(play["paths"].items()):
        c = COLORS.get(role, (0, 0, 0))
        xy = [px(x, y) for _, x, y in p]
        if len(xy) > 1:
            d.line(xy, fill=c, width=3)
        sx, sy = xy[0]
        d.ellipse([sx - 5, sy - 5, sx + 5, sy + 5], outline=c, width=2)  # start: ring
        ex, ey = xy[-1]
        d.ellipse([ex - 5, ey - 5, ex + 5, ey + 5], fill=c)  # end: dot
    bxy = [px(x, y) for _, x, y, _ in play["ball"]]
    for bx, by in bxy:
        d.ellipse([bx - 2, by - 2, bx + 2, by + 2], fill=(255, 255, 255))
    t_end = max([t for p in play["paths"].values() for t, _, _ in p] or [0])
    d.text((6, h - 34), f"down {play['down']}  {t_end:.1f}s  ring=start dot=end  white dots=ball  line=scrimmage", fill=(255, 255, 255))
    x = 6
    for k in sorted(NAMES):
        d.rectangle([x, h - 16, x + 10, h - 6], fill=COLORS[k])
        d.text((x + 14, h - 18), NAMES[k], fill=(255, 255, 255))
        x += 14 + 8 * len(NAMES[k]) + 10
    img.save(path)


def main(argv):
    if argv[:1] == ["--selftest"]:
        return selftest()
    if not argv:
        print(__doc__)
        return 1
    last = None
    if "--last" in argv:
        k = argv.index("--last")
        last = int(argv[k + 1])
        argv = argv[:k] + argv[k + 2:]
    log = argv[0]
    out = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(log)), "traces")
    os.makedirs(out, exist_ok=True)
    with open(log, encoding="utf8", errors="replace") as f:
        plays = parse(f)
    if last:
        plays = plays[-last:]
    for n, play in enumerate(plays, 1):
        p = os.path.join(out, f"play_{play['stamp']}_{n}.png")
        draw(play, p)
        print(p, f"({len(play['paths'])} players, {len(play['ball'])} snapshots)")
    if not plays:
        print("no traced plays in", log)
    return 0


def selftest():
    import tempfile
    log = ["[100] script: s1: TS 1866 -1 141 411 2",
           "[100] script: s1: TB 0.0 1866 276 5",
           "[100] script: s1: TP 0.0 1 0 1880 276",
           "[100] script: s1: TP 0.0 3 1 1870 160",
           "[100] script: s1: TP 0.0 5 0 1840 160",
           "[100] script: s1: TB 0.5 1866 276 5",
           "[100] script: s1: TP 0.5 1 0 1890 276",
           "[100] script: s1: TP 0.5 3 1 1830 165",
           "[100] script: s1: TP 0.5 5 0 1835 170",
           "[100] rules: unrelated line",
           "[101] script: s1: TS 1700 1 141 411 1"]
    plays = parse(log)
    assert len(plays) == 2, plays
    assert len(plays[0]["paths"]) == 3 and len(plays[0]["ball"]) == 2
    assert plays[0]["paths"][(3, 1)][-1] == (0.5, 1830.0, 165.0)
    out = os.path.join(tempfile.mkdtemp(), "t.png")
    draw(plays[0], out)
    assert os.path.getsize(out) > 0
    draw(plays[1], out)  # a play with no snapshots still draws
    print("selftest ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
