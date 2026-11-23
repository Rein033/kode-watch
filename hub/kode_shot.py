"""kode_shot.py <app> <out-prefix> "<script>": run a built app headless and take screenshots.

Script steps, separated by ';':
  w <ms>            wait
  p <btn> [ms]      press a button (up down left right top bot) for ms (default 120)
  h <btn> / r <btn> hold / release
  t <x> <y> [ms]    tap the glass
  d <x0> <y0> <x1> <y1> [ms]   drag
  s <name>          screenshot -> <out-prefix>-<name>.png
"""
import asyncio, os, struct, sys, tempfile, shutil
import numpy as np
from PIL import Image

BITS = {"up": 1, "down": 2, "left": 4, "right": 8, "top": 16, "bot": 32}


async def main(app, prefix, script):
    tmp = tempfile.mkdtemp()
    sock = os.path.join(tmp, "s")
    got = asyncio.get_running_loop().create_future()
    srv = await asyncio.start_unix_server(lambda r, w: got.done() or got.set_result((r, w)), path=sock)
    data = os.path.join(tmp, "data")
    os.makedirs(data)
    env = dict(os.environ, KODE_SIM_SOCK=sock, KODE_SIM_DATA=data)
    proc = await asyncio.create_subprocess_exec(f"/opt/kode/builds/{app}/kode_sim_app.elf", env=env,
                                                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE)
    reader, writer = await asyncio.wait_for(got, 10)
    frame = {"img": None}
    logs = []

    async def pump():
        try:
            while True:
                h = await reader.readexactly(5)
                n = struct.unpack("<I", h[1:])[0]
                b = await reader.readexactly(n)
                if h[0] == ord("F"):
                    w, hh = struct.unpack("<HH", b[:4])
                    frame["img"] = (w, hh, b[4:])
                elif h[0] == ord("L"):
                    logs.append(b.decode(errors="replace"))
                elif h[0] == ord("X"):
                    logs.append("EXIT " + b.decode())
        except Exception:
            pass
    task = asyncio.create_task(pump())
    held = 0

    def send(line):
        writer.write((line + "\n").encode())

    for step in [s.strip() for s in script.split(";") if s.strip()]:
        a = step.split()
        if a[0] == "w":
            await asyncio.sleep(int(a[1]) / 1000)
        elif a[0] in ("p", "h", "r"):
            bit = BITS[a[1]]
            if a[0] in ("p", "h"):
                held |= bit; send(f"b {held}")
            if a[0] == "p":
                await asyncio.sleep((int(a[2]) if len(a) > 2 else 120) / 1000)
            if a[0] in ("p", "r"):
                held &= ~bit; send(f"b {held}")
            await asyncio.sleep(0.05)
        elif a[0] == "t":
            send(f"t 1 {a[1]} {a[2]}")
            await asyncio.sleep((int(a[3]) if len(a) > 3 else 120) / 1000)
            send(f"t 0 {a[1]} {a[2]}")
            await asyncio.sleep(0.05)
        elif a[0] == "d":
            x0, y0, x1, y1 = map(int, a[1:5]); ms = int(a[5]) if len(a) > 5 else 400
            n = max(2, ms // 20)
            for i in range(n + 1):
                x = x0 + (x1 - x0) * i // n; y = y0 + (y1 - y0) * i // n
                send(f"t 1 {x} {y}"); await asyncio.sleep(0.02)
            send(f"t 0 {x1} {y1}"); await asyncio.sleep(0.05)
        elif a[0] == "s":
            await asyncio.sleep(0.1)
            if frame["img"]:
                w, h, buf = frame["img"]
                px = np.frombuffer(buf, dtype="<u2").reshape(h, w)
                rgb = np.stack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31], -1).astype(np.uint8)
                Image.fromarray(rgb).save(f"{prefix}-{a[1]}.png")
                print("shot", f"{prefix}-{a[1]}.png")
            else:
                print("no frame yet")
    proc.kill()
    await proc.wait()
    err = (await proc.stderr.read()).decode(errors="replace")
    task.cancel()
    srv.close()
    shutil.rmtree(tmp, ignore_errors=True)
    bad = [l for l in "".join(logs).splitlines() if l.startswith("!E") or "PANIC" in l or "assert" in l.lower()]
    if bad:
        print("\n".join(bad[-8:]))
    if "Assertion" in err or "Segmentation" in err:
        print("STDERR:", err[-800:])

asyncio.run(main(sys.argv[1], sys.argv[2], sys.argv[3]))
