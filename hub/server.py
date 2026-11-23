"""Kode hub: de digitale Kode Dot.

Elke WebSocket-sessie start een eigen sim-proces (een Kode-app gebouwd voor
ESP-IDF linux op de echte kode-sdk, met het bord gesimuleerd eronder), in een
bubblewrap-sandbox zonder netwerk. Het proces praat met deze server over een
unix-socket (zie sim/components/khal/src/kdotsim_bridge.c):

  sim -> server: [u8 type][u32 len LE][payload]
      F frame (u16 w, u16 h, RGB565), E led rgb, B helderheid, H haptic ms,
      T toon (u32 hz, u32 ms, u8 vol), P pcm (u32 rate, s16...), L log, X exit
      N link-bericht (tekst) voor het gekoppelde apparaat
  server -> sim: tekstregels "b <mask>", "t <0|1> <x> <y>", "q",
      "n <tekst>" (link-bericht), "m <w> <h> <base64>" (spiegelbeeld)

Kode Link: een Dot-sessie en een Watch-sessie van dezelfde gebruiker worden
gekoppeld via de hub hieronder. Op echte hardware is dit BLE; de apps zien
hetzelfde: tekstberichten heen en weer, plus "hub peer 0|1" en "sys ..." van
de hub zelf. "hub mirror 1" van de Dot vraagt een spiegelbeeld van het horloge.

Naar de browser: binair 'J'+jpeg of 'P'+rate(u32)+pcm, en JSON-tekst voor de rest.
"""
import asyncio
import collections
import base64
import datetime
import io
import json
import logging
import os
import pathlib
import re
import secrets
import shutil
import hashlib
import uuid
import struct
import tempfile
import time
from pathlib import Path

import numpy as np
from aiohttp import web, WSMsgType
from PIL import Image

ROOT = Path(__file__).parent
STATIC = ROOT / "static"
PORT = int(os.environ.get("KODE_PORT", "8000"))
BUILDS = Path(os.environ.get("KODE_BUILDS", "/opt/kode/builds"))
DATA = Path(os.environ.get("KODE_DATA", "/opt/kode/data"))
MAX_SESSIONS = int(os.environ.get("KODE_MAX_SESSIONS", "4"))
FPS = 30
JPEG_Q = 80

log = logging.getLogger("kode")
BTN_BITS = {"up": 1, "down": 2, "left": 4, "right": 8, "top": 16, "bot": 32}
sessions = set()
hub = {}           # gebruiker -> {"dot": Session, "watch": Session}
MIRROR_GAP = 0.15  # spiegelbeeld hooguit ~6 keer per seconde


@web.middleware
async def no_cache(request, handler):
    resp = await handler(request)
    if request.path.endswith((".html", ".js", ".css")) or request.path in ("/", "/watch", "/docs", "/watch-docs"):
        resp.headers["Cache-Control"] = "no-cache"
    return resp


def app_list():
    out = []
    for d in sorted(BUILDS.glob("*/kode_sim_app.elf")):
        name = d.parent.name
        if name.startswith(EDIT_PREFIX):
            continue
        meta = {}
        mf = d.parent / "kode-sim.json"
        if mf.exists():
            try:
                meta = json.loads(mf.read_text())
            except ValueError:
                meta = {}
        look = {}
        aj = LOCAL_APPS / name / "appdata" / "app.json"
        if aj.exists():
            try:
                look = json.loads(aj.read_text())
            except ValueError:
                look = {}
        out.append({"id": name, "name": meta.get("name", name), "repo": meta.get("repo", ""),
                    "color": look.get("color", ""), "desc": look.get("description", ""),
                    "icon": (d.parent / "icon.png").exists() or (LOCAL_APPS / name / "appdata" / "icon.png").exists(),
                    "built": int(d.stat().st_mtime)})
    return out


def mirror_line(buf, w, h):
    px = np.frombuffer(buf, dtype="<u2", count=w * h).reshape(h, w)[::2, ::2]
    mh, mw = px.shape
    return f"m {mw} {mh} " + base64.b64encode(np.ascontiguousarray(px).astype("<u2").tobytes()).decode()


def wall_clock_line():
    now = datetime.datetime.now().astimezone()
    off = int(now.utcoffset().total_seconds()) if now.utcoffset() else 0
    try:
        from zoneinfo import ZoneInfo
        off = int(datetime.datetime.now(ZoneInfo("Europe/Amsterdam")).utcoffset().total_seconds())
    except Exception:  # noqa: BLE001
        pass
    return f"n sys time {int(time.time())} {off}"


def rgb565_to_jpeg(buf, w, h):
    px = np.frombuffer(buf, dtype="<u2", count=w * h).reshape(h, w)
    rgb = np.empty((h, w, 3), dtype=np.uint8)
    rgb[..., 0] = ((px >> 11) & 0x1F) * 255 // 31
    rgb[..., 1] = ((px >> 5) & 0x3F) * 255 // 63
    rgb[..., 2] = (px & 0x1F) * 255 // 31
    bio = io.BytesIO()
    Image.fromarray(rgb, "RGB").save(bio, "JPEG", quality=JPEG_Q)
    return bio.getvalue()


class Session:
    """One browser tab, one sim process."""

    def __init__(self, ws, app_id, role="dot", user=""):
        self.ws = ws
        self.app_id = app_id
        self.role = role
        self.user = user
        self.mirror = False
        self.last_mirror = 0.0
        self.proc = None
        self.reader = None
        self.writer = None
        self.tmp = None
        self.frame = None          # latest RGB565 frame (w, h, bytes)
        self.frame_evt = asyncio.Event()
        self.closed = False
        self.buttons = 0

    @property
    def peer(self):
        slot = hub.get(self.user, {})
        if self.role == "dot":
            p = slot.get("watch")
            return p if p is not None and not p.closed else None
        p = slot.get("dot")
        return p if p is not None and not p.closed else None

    def join_hub(self):
        old = hub.setdefault(self.user, {}).get(self.role)
        hub[self.user][self.role] = self
        if old is not None and old is not self:
            old.send_line("n hub peer 0")
        p = self.peer
        self.send_line(wall_clock_line())
        self.send_line(f"n hub peer {1 if p else 0}")
        if p:
            p.send_line("n hub peer 1")

    def leave_hub(self):
        slot = hub.get(self.user, {})
        if slot.get(self.role) is self:
            del slot[self.role]
            p = self.peer
            if p:
                p.mirror = False
                p.send_line("n hub peer 0")

    async def on_link(self, text):
        """Een link-bericht uit deze sim: voor de hub zelf, of door naar het andere apparaat."""
        if text.startswith("hub mirror "):
            self.mirror = text.endswith("1")
            return
        if not text.startswith("st "):
            await self.send_json(t="link", dir="uit", s=text)
        p = self.peer
        if p:
            p.send_line("n " + text)
            if not text.startswith("st "):
                await p.send_json(t="link", dir="in", s=text)

    async def send_mirror(self, buf, w, h):
        p = self.peer
        if not p or not p.mirror:
            return
        now = time.monotonic()
        if now - self.last_mirror < MIRROR_GAP:
            return
        self.last_mirror = now
        line = await asyncio.get_running_loop().run_in_executor(None, mirror_line, buf, w, h)
        p.send_line(line)

    async def send_json(self, **kw):
        if not self.ws.closed:
            await self.ws.send_str(json.dumps(kw))

    async def start(self):
        elf = BUILDS / self.app_id / "kode_sim_app.elf"
        if not elf.exists():
            raise web.HTTPNotFound(text="app not built")
        self.tmp = Path(tempfile.mkdtemp(prefix="kode-"))
        os.chmod(self.tmp, 0o777)
        sock_path = self.tmp / "sim.sock"
        data = DATA / self.app_id
        data.mkdir(parents=True, exist_ok=True)

        connected = asyncio.get_running_loop().create_future()

        async def on_conn(reader, writer):
            if connected.done():
                writer.close()
                return
            connected.set_result((reader, writer))

        server = await asyncio.start_unix_server(on_conn, path=str(sock_path))
        os.chmod(sock_path, 0o777)
        cmd = [
            "bwrap", "--ro-bind", "/usr", "/usr", "--ro-bind", "/lib", "/lib",
            "--ro-bind-try", "/lib64", "/lib64", "--ro-bind-try", "/lib32", "/lib32",
            "--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache",
            "--ro-bind", str(elf), "/app.elf",
            "--bind", str(self.tmp), "/run/sim",
            "--bind", str(data), "/data",
            "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
            "--unshare-all", "--die-with-parent", "--new-session",
            "--setenv", "KODE_SIM_SOCK", "/run/sim/sim.sock",
            "--setenv", "KODE_SIM_DATA", "/data",
            "--chdir", "/data",
            "/app.elf",
        ]
        self.proc = await asyncio.create_subprocess_exec(
            "prlimit", "--as=1500000000", "--cpu=3600", "--", *cmd,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        try:
            self.reader, self.writer = await asyncio.wait_for(connected, 10)
        except asyncio.TimeoutError:
            err = b""
            if self.proc.stderr:
                try:
                    err = await asyncio.wait_for(self.proc.stderr.read(2000), 1)
                except asyncio.TimeoutError:
                    pass
            raise RuntimeError("sim did not start: " + err.decode(errors="replace"))
        finally:
            server.close()

    async def pump_sim(self):
        """Records from the sim to the browser."""
        try:
            while True:
                hdr = await self.reader.readexactly(5)
                typ = chr(hdr[0])
                (n,) = struct.unpack("<I", hdr[1:])
                body = await self.reader.readexactly(n)
                if typ == "F":
                    w, h = struct.unpack("<HH", body[:4])
                    self.frame = (w, h, body[4:])
                    self.frame_evt.set()
                    if self.role == "watch":
                        await self.send_mirror(body[4:], w, h)
                elif typ == "E":
                    await self.send_json(t="led", r=body[0], g=body[1], b=body[2])
                elif typ == "B":
                    await self.send_json(t="bright", v=body[0])
                elif typ == "H":
                    await self.send_json(t="haptic", ms=struct.unpack("<I", body)[0])
                elif typ == "T":
                    hz, ms = struct.unpack("<II", body[:8])
                    await self.send_json(t="tone", hz=hz, ms=ms, vol=body[8])
                elif typ == "P":
                    if not self.ws.closed:
                        await self.ws.send_bytes(b"P" + body)
                elif typ == "L":
                    await self.send_json(t="log", s=body.decode(errors="replace"))
                elif typ == "N":
                    await self.on_link(body.decode(errors="replace").replace("\n", " "))
                elif typ == "X":
                    await self.send_json(t="exit", why=body.decode(errors="replace"))
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        rc = await self.proc.wait() if self.proc else None
        err = b""
        if self.proc and self.proc.stderr:
            try:
                err = await asyncio.wait_for(self.proc.stderr.read(4000), 1)
            except asyncio.TimeoutError:
                pass
        await self.send_json(t="ended", rc=rc, err=err.decode(errors="replace")[-1500:])

    async def pump_frames(self):
        """Latest frame to the browser, at most FPS a second, dropping what it cannot keep up with."""
        loop = asyncio.get_running_loop()
        gap = 1.0 / FPS
        last = 0.0
        while not self.closed:
            await self.frame_evt.wait()
            self.frame_evt.clear()
            wait = gap - (time.monotonic() - last)
            if wait > 0:
                await asyncio.sleep(wait)
            w, h, buf = self.frame
            jpg = await loop.run_in_executor(None, rgb565_to_jpeg, buf, w, h)
            last = time.monotonic()
            if self.ws.closed:
                break
            await self.ws.send_bytes(b"J" + jpg)

    def send_line(self, line):
        if self.writer and not self.writer.is_closing():
            self.writer.write((line + "\n").encode())

    def on_browser(self, msg):
        t = msg.get("t")
        if t == "btn":
            bit = BTN_BITS.get(msg.get("b"))
            if bit:
                self.buttons = (self.buttons | bit) if msg.get("d") else (self.buttons & ~bit)
                self.send_line(f"b {self.buttons}")
        elif t == "touch":
            down = 0 if msg.get("k") == "up" else 1
            x = int(msg.get("x", 0))
            y = int(msg.get("y", 0))
            self.send_line(f"t {down} {x} {y}")
        elif t == "dock":
            self.send_line(f"n sys dock {1 if msg.get('v') else 0}")
        elif t == "notify":
            text = str(msg.get("s", ""))[:200].replace("\n", " ")
            target = self if self.role == "watch" else self.peer
            if text and target:
                target.send_line("n notify " + text)

    async def stop(self):
        self.leave_hub()
        self.closed = True
        self.frame_evt.set()
        if self.writer:
            try:
                self.send_line("q")
                self.writer.close()
            except Exception:
                pass
        if self.proc and self.proc.returncode is None:
            try:
                self.proc.kill()
            except ProcessLookupError:
                pass
            try:
                await asyncio.wait_for(self.proc.wait(), 3)
            except asyncio.TimeoutError:
                pass
        if self.tmp:
            shutil.rmtree(self.tmp, ignore_errors=True)


# ---- bouwen vanaf Git ---------------------------------------------------------

WORK = Path(os.environ.get("KODE_WORK", "/opt/kode/work"))
SIM = Path(os.environ.get("KODE_SIM", "/opt/kode/sim"))
SDK = Path(os.environ.get("KODE_SDK", "/opt/kode/src/kode-sdk"))
REPO_RE = re.compile(r"^https://(github\.com|gitlab\.com|codeberg\.org)/[\w.-]+/[\w.-]+?(\.git)?/?$")
LOCAL_APPS = Path(os.environ.get("KODE_LOCAL_APPS", "/opt/kode/apps"))
build_lock = asyncio.Lock()
jobs = {}


def app_id_for(repo):
    if repo.startswith("local:"):
        return repo[6:]
    name = repo.rstrip("/").rsplit("/", 1)[-1]
    name = re.sub(r"\.git$", "", name)
    name = re.sub(r"^kode-app-", "", name)
    name = re.sub(r"[^a-z0-9_]", "_", name.lower())[:31].strip("_")
    return name or "app"


class Job:
    def __init__(self, repo, ref, app=None, code=None, guest=False, client=""):
        self.id = uuid.uuid4().hex[:10]
        self.repo = repo
        self.ref = ref
        self.app = app or app_id_for(repo)
        self.code = code          # edited app_main.c (repo "edit:<base>")
        self.guest = guest
        self.client = client
        self.state = "waiting"
        self.lines = []
        self.started = time.time()

    def add(self, text):
        for line in text.splitlines():
            self.lines.append(line)
        if len(self.lines) > 3000:
            self.lines = self.lines[-3000:]


async def run_logged(job, cmd, timeout):
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    try:
        async def pump():
            while True:
                line = await proc.stdout.readline()
                if not line:
                    break
                job.add(line.decode(errors="replace").rstrip("\n"))
        await asyncio.wait_for(pump(), timeout)
        return await proc.wait()
    except asyncio.TimeoutError:
        proc.kill()
        job.add(f"!! took too long (> {timeout} s), stopped")
        return -1


async def build_job(job):
    async with build_lock:
        job.state = "building"
        app_dir = WORK / "app"
        out = WORK / "build"
        try:
            shutil.rmtree(app_dir, ignore_errors=True)
            if job.repo.startswith("edit:"):
                src = LOCAL_APPS / job.repo[5:]
                job.add("== your code, on the " + job.repo[5:] + " app skeleton")
                shutil.copytree(src, app_dir, copy_function=shutil.copy)
                (app_dir / "main" / "app_main.c").write_text(job.code)
            elif job.repo.startswith("local:"):
                src = LOCAL_APPS / job.repo[6:]
                job.add(f"== local app {job.repo[6:]}")
                shutil.copytree(src, app_dir, copy_function=shutil.copy)
            else:
                job.add(f"== fetching {job.repo}" + (f" ({job.ref})" if job.ref else ""))
                cmd = ["git", "clone", "--depth", "1", "--recurse-submodules", "--shallow-submodules"]
                if job.ref:
                    cmd += ["--branch", job.ref]
                env_cmd = ["env", "GIT_TERMINAL_PROMPT=0"] + cmd + [job.repo, str(app_dir)]
                rc = await run_logged(job, env_cmd, 180)
                if rc != 0:
                    job.state = "failed"
                    job.add("!! git clone failed (private repo? wrong branch?)")
                    return
            (WORK / "home").mkdir(exist_ok=True)
            sandbox = [
                "bwrap", "--ro-bind", "/usr", "/usr",
                "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib32", "/lib32",
                "--symlink", "usr/lib64", "/lib64", "--symlink", "usr/bin", "/bin",
                "--symlink", "usr/sbin", "/sbin", "--ro-bind", "/etc", "/etc",
                "--ro-bind", "/opt/esp", "/opt/esp",
                "--ro-bind", str(SIM), str(SIM), "--ro-bind", str(SDK), str(SDK),
                "--bind", str(WORK), str(WORK),
                "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
                "--unshare-all", "--die-with-parent", "--new-session",
                "--setenv", "HOME", str(WORK / "home"),
                str(SIM / "build.sh"), str(app_dir), str(out),
            ]
            rc = await run_logged(job, ["prlimit", "--as=4000000000", "--cpu=300", "--"] + sandbox, 180 if job.guest else 900)
            elf = out / "kode_sim_app.elf"
            if rc != 0 or not elf.exists():
                job.state = "failed"
                return
            dest = BUILDS / job.app
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copy2(elf, dest / "kode_sim_app.elf.new")
            os.replace(dest / "kode_sim_app.elf.new", dest / "kode_sim_app.elf")
            name = job.app.replace("_", " ").title()
            m = re.search(r'KODE_APP_MANIFEST\(\s*"([^"]+)"', "".join(
                p.read_text(errors="replace") for p in (app_dir / "main").glob("*.c")))
            if m:
                name = m.group(1)
            icon = app_dir / "appdata" / "icon.png"
            if icon.exists() and icon.stat().st_size < 2_000_000:
                shutil.copy(icon, dest / "icon.png")
            (dest / "kode-sim.json").write_text(json.dumps(
                {"name": name, "repo": job.repo, "ref": job.ref, "guest": job.guest}))
            job.state = "done"
            job.add(f"== {name} is ready")
        except Exception as e:  # noqa: BLE001
            log.exception("build")
            job.state = "failed"
            job.add(f"!! {e}")


async def api_build(request):
    body = await request.json()
    repo = str(body.get("repo", "")).strip()
    ref = str(body.get("ref", "")).strip()
    local = str(body.get("local", "")).strip()
    if local:
        if not re.match(r"^[a-z0-9_]{1,31}$", local) or not (LOCAL_APPS / local / "main").is_dir():
            return web.json_response({"error": "unknown local app"}, status=400)
        repo = "local:" + local
    elif not REPO_RE.match(repo):
        return web.json_response({"error": "only https URLs from GitHub, GitLab or Codeberg"}, status=400)
    if ref and not re.match(r"^[\w./-]{1,100}$", ref):
        return web.json_response({"error": "invalid branch/tag"}, status=400)
    job = Job(repo, ref)
    jobs[job.id] = job
    for old in sorted(jobs.values(), key=lambda j: j.started)[:-20]:
        if old.state not in ("waiting", "building"):
            jobs.pop(old.id, None)
    asyncio.create_task(build_job(job))
    return web.json_response({"job": job.id, "app": job.app})


async def api_job(request):
    job = jobs.get(request.match_info["id"])
    if not job or not (job.guest or request.headers.get("Remote-User")):
        raise web.HTTPNotFound()
    since = int(request.query.get("since", "0"))
    return web.json_response({"state": job.state, "app": job.app,
                              "lines": job.lines[since:], "next": len(job.lines)})


# ---- edit the code in the browser (public) ----------------------------------

EDIT_PREFIX = "g_"
PRIVATE_APPS = {"kodewatch", "watchremote"}   # need the Kode Link pages, which are private
MAX_CODE = 64 * 1024
MAX_GUEST_QUEUE = 4
GUEST_KEEP_S = 6 * 3600
STARTER = r"""/* My first Kode app.
 * Edit this code and press "Build & run" (Ctrl+Enter).
 * The drawing kit is kodekit: see the kk.h tab for every function.
 */
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("My App", "games");

void app_main(void)
{
    kk_init();
    int x = KK_W / 2, y = KK_H / 2, presses = 0;
    for ( ;; )
    {
        kk_begin();                       /* read the buttons, start a frame */

        uint32_t const h = kk_held();     /* move with the d-pad */
        if ( h & KK_LEFT )  { x -= 4; }
        if ( h & KK_RIGHT ) { x += 4; }
        if ( h & KK_UP )    { y -= 4; }
        if ( h & KK_DOWN )  { y += 4; }

        int tx, ty;                       /* or tap the glass */
        if ( kk_tapped(&tx, &ty) ) { x = tx; y = ty; }

        if ( kk_pressed(KK_TOP) ) { presses++; kk_tone(880, 60); kk_led(0, 40, 80); }
        if ( kk_pressed(KK_BOT) ) { kk_led(0, 0, 0); kk_buzz(40); }

        if ( x < 60 ) { x = 60; }
        if ( x > KK_W - 60 ) { x = KK_W - 60; }
        if ( y < 140 ) { y = 140; }
        if ( y > KK_H - 70 ) { y = KK_H - 70; }

        kk_vgrad(0, 0, KK_W, KK_H, KK_RGB(20, 24, 32), KK_RGB(6, 8, 12));
        kk_text_c(56, "HELLO KODE", 3, KK_WHITE);
        char b[32];
        (void)snprintf(b, sizeof(b), "TOP PRESSED %d", presses);
        kk_text_c(96, b, 1, KK_GREY);
        kk_disc(x, y, 26, KK_ORANGE);
        kk_text_c(462, "D-PAD OR TAP TO MOVE", 1, KK_GREY);

        kk_end();                         /* show the frame */
    }
}
"""


def editable_apps():
    out = []
    for d in sorted(LOCAL_APPS.glob("*/main/app_main.c")):
        app = d.parent.parent.name
        if app in PRIVATE_APPS:
            continue
        name = app
        m = re.search(r'KODE_APP_MANIFEST\(\s*"([^"]+)"', d.read_text(errors="replace"))
        if m:
            name = m.group(1)
        out.append({"id": app, "name": name})
    return out


def client_of(request):
    return (request.headers.get("Cf-Connecting-Ip")
            or request.headers.get("X-Forwarded-For", "").split(",")[0].strip()
            or (request.remote or ""))


def clean_guest_builds():
    cutoff = time.time() - GUEST_KEEP_S
    for root in (BUILDS, DATA):
        for d in root.glob(EDIT_PREFIX + "*"):
            try:
                if d.stat().st_mtime < cutoff and not any(s.app_id == d.name for s in sessions):
                    shutil.rmtree(d, ignore_errors=True)
            except OSError:
                pass


async def api_icon(request):
    app = request.match_info["id"]
    if not re.match(r"^[a-z0-9_]{1,31}$", app):
        raise web.HTTPNotFound()
    for f in (BUILDS / app / "icon.png", LOCAL_APPS / app / "appdata" / "icon.png"):
        if f.exists():
            return web.FileResponse(f, headers={"Cache-Control": "max-age=3600"})
    raise web.HTTPNotFound()


async def api_sources(request):
    return web.json_response({"apps": editable_apps()})


async def api_source(request):
    app = request.match_info["id"]
    kk_h = (LOCAL_APPS / "slang" / "main" / "kk.h").read_text(errors="replace")
    if app == "new":
        return web.json_response({"id": "new", "name": "My App", "code": STARTER, "kk_h": kk_h})
    if app in PRIVATE_APPS or not re.match(r"^[a-z0-9_]{1,31}$", app):
        raise web.HTTPNotFound()
    f = LOCAL_APPS / app / "main" / "app_main.c"
    if not f.exists():
        raise web.HTTPNotFound()
    return web.json_response({"id": app, "name": app, "code": f.read_text(errors="replace"), "kk_h": kk_h})


async def api_play(request):
    """Build the code from the editor on a local app's skeleton. Anyone may do this."""
    try:
        body = await request.json()
    except ValueError:
        body = {}
    base = str(body.get("base", "new"))
    code = body.get("code", "")
    if not isinstance(code, str) or not code.strip():
        return web.json_response({"error": "no code"}, status=400)
    if len(code.encode()) > MAX_CODE:
        return web.json_response({"error": "code too long (max 64 KB)"}, status=400)
    if base == "new" or base in PRIVATE_APPS or not (LOCAL_APPS / base / "main").is_dir() \
            or not re.match(r"^[a-z0-9_]{1,31}$", base):
        base = "slang"
    app = EDIT_PREFIX + hashlib.sha1((base + "\0" + code).encode()).hexdigest()[:12]
    if (BUILDS / app / "kode_sim_app.elf").exists():
        os.utime(BUILDS / app)
        return web.json_response({"job": None, "app": app, "cached": True})
    user = request.headers.get("Remote-User", "")
    client = user or client_of(request)
    busy = [j for j in jobs.values() if j.state in ("waiting", "building")]
    if any(j.client == client for j in busy):
        return web.json_response({"error": "you already have a build running"}, status=429)
    if not user and sum(1 for j in busy if j.guest) >= MAX_GUEST_QUEUE:
        return web.json_response({"error": "the build queue is full, try again in a minute"}, status=429)
    clean_guest_builds()
    job = Job("edit:" + base, "", app=app, code=code, guest=not user, client=client)
    jobs[job.id] = job
    for old in sorted(jobs.values(), key=lambda j: j.started)[:-40]:
        if old.state not in ("waiting", "building"):
            jobs.pop(old.id, None)
    asyncio.create_task(build_job(job))
    return web.json_response({"job": job.id, "app": app})


async def index(request):
    return web.FileResponse(STATIC / "index.html")


async def watch_page(request):
    return web.FileResponse(STATIC / "watch.html")


async def docs_page(request):
    return web.FileResponse(STATIC / "docs.html")


async def watch_docs_page(request):
    return web.FileResponse(STATIC / "watch-docs.html")


async def api_notify(request):
    """Een melding naar het horloge, bv. vanuit Home Assistant: POST {"text": "..."}."""
    try:
        body = await request.json()
    except ValueError:
        body = {}
    text = str(body.get("text", "")).strip()[:200].replace("\n", " ")
    if not text:
        return web.json_response({"error": "no text"}, status=400)
    sent = 0
    for slot in hub.values():
        w = slot.get("watch")
        if w and not w.closed:
            w.send_line("n notify " + text)
            sent += 1
    return web.json_response({"sent": sent})


async def status(request):
    return web.json_response({
        "user": request.headers.get("Remote-User", ""),
        "sessions": len(sessions),
        "apps": [a for a in app_list()
                 if request.headers.get("Remote-User") or a["id"] not in PRIVATE_APPS],
    })


DEVICES = ROOT / "devices.json"   # sleutel -> {"user", "role", "name"}; alleen leesbaar voor kode


def load_devices():
    try:
        return json.loads(DEVICES.read_text())
    except (OSError, ValueError):
        return {}


def save_devices(d):
    tmp = DEVICES.with_suffix(".tmp")
    tmp.write_text(json.dumps(d, indent=1))
    os.chmod(tmp, 0o600)
    tmp.replace(DEVICES)


async def api_devkey(request):
    """De apparaatsleutel van het horloge voor de ingelogde gebruiker (achter Authelia)."""
    user = request.headers.get("Remote-User", "")
    if not user:
        return web.json_response({"error": "login nodig"}, status=401)
    d = load_devices()
    for k, v in d.items():
        if v.get("user") == user and v.get("role") == "watch":
            return web.json_response({"key": k})
    k = secrets.token_urlsafe(24)
    d[k] = {"user": user, "role": "watch", "name": "kode-watch"}
    save_devices(d)
    return web.json_response({"key": k})


class DevicePeer:
    """Een echt apparaat (het Kode-watch) als hub-partner, via /ws-dev met een eigen sleutel.

    Tekstframes zijn link-berichten, beide kanten op, zonder "n " ervoor.
    """

    mirror = False

    def __init__(self, ws, user, role):
        self.ws = ws
        self.user = user
        self.role = role
        self.closed = False

    def send_line(self, line):
        if line.startswith("n ") and not self.ws.closed:
            asyncio.get_running_loop().create_task(self.ws.send_str(line[2:]))

    async def send_json(self, **kw):
        return  # geen browser aan deze kant

    @property
    def peer(self):
        p = hub.get(self.user, {}).get("dot")
        return p if p is not None and not p.closed else None

    def join(self):
        old = hub.setdefault(self.user, {}).get(self.role)
        hub[self.user][self.role] = self
        if old is not None and old is not self:
            old.send_line("n hub peer 0")
        p = self.peer
        self.send_line(wall_clock_line())
        self.send_line(f"n hub peer {1 if p else 0}")
        if p:
            p.send_line("n hub peer 1")

    def leave(self):
        self.closed = True
        slot = hub.get(self.user, {})
        if slot.get(self.role) is self:
            del slot[self.role]
            p = self.peer
            if p:
                p.mirror = False
                p.send_line("n hub peer 0")

    async def from_device(self, text):
        text = text.strip().replace("\n", " ")[:500]
        if not text or text.startswith("hub "):
            return
        if not text.startswith("st "):
            log.info("device %s: %s", self.user, text[:60])
        if text.startswith("sd "):
            watch_state.setdefault(self.user, {})["sd"] = text[3:][:60]
            return
        if text.startswith("fs "):
            fs_feed(self.user, text)
            return
        if text.startswith(("boot ", "crash ")):
            watch_state.setdefault(self.user, {})["boot"] = text[:240]
            watch_log.setdefault(self.user, collections.deque(maxlen=600)).append(time.strftime("%H:%M:%S ") + text[:240])
            logging.info("device %s: %s", self.user, text[:240])
            return
        if text.startswith("log "):
            dq = watch_log.setdefault(self.user, collections.deque(maxlen=600))
            dq.append(time.strftime("%H:%M:%S ") + text[4:][:200])
            return
        if text.startswith(("store ", "app ")):
            return
        if text.startswith("st "):
            p = text.split()
            if len(p) >= 9:
                try:
                    watch_state.setdefault(self.user, {})["st"] = {"time": p[1], "bat": int(p[2]), "dock": p[3] == "1",
                                                                  "steps": int(p[5]), "notifs": int(p[6])}
                    asyncio.get_running_loop().create_task(ha_steps(int(p[5]), int(p[2]), p[3] == "1"))
                except ValueError:
                    pass
        if text.startswith(("ver ", "ota ", "kody ")):
            st = watch_state.setdefault(self.user, {})
            parts = text.split()
            if parts[0] == "ver" and len(parts) >= 3:
                st["ver"], st["slot"] = parts[1][:16], parts[2][:16]
                st.pop("ota", None)
            elif parts[0] in ("ota", "kody"):
                st["ota"] = (parts[0] + " " + " ".join(parts[1:]))[:40]
            return
        m = re.fullmatch(r"ph (\?|ring)", text)
        if m:
            await ph_cmd(self, m.group(1))
            return
        m = re.fullmatch(r"mu (\?|pp|next|prev|vol\+|vol-|sel)", text)
        if m:
            await mu_cmd(self, m.group(1))
            return
        m = re.fullmatch(r"home ([0-3]) ([01])", text)
        if m:
            await home_switch(int(m.group(1)), m.group(2) == "1")
            await asyncio.sleep(0.6)
            self.home_last = None
            await home_push(self)
        p = self.peer
        if p:
            p.send_line("n " + text)
            if not text.startswith("st "):
                await p.send_json(t="link", dir="in", s=text)


# ---- Home Assistant for the watch's Home app ---------------------------------
# The watch sends "home <i> <0|1>" (room i on/off) and "home 3 0" (all off);
# the hub switches the room's entities in Home Assistant and answers with
# "hs <bits>" (bit i: room i has a light on) and the room names ("hn <i> <name>").
# Rooms and entities: home.json next to this file. Token: ha_token (mode 600).
HOME_CFG = pathlib.Path(__file__).with_name("home.json")
HA_TOKEN = pathlib.Path(__file__).with_name("ha_token")


def home_cfg(key, default):
    """A setting from home.json (calendars, music_prefer, phones, public_url)."""
    try:
        return json.loads(HOME_CFG.read_text()).get(key, default)
    except (OSError, ValueError):
        return default


def home_rooms():
    try:
        c = json.loads(HOME_CFG.read_text())
        return c.get("ha_url", "").rstrip("/"), c.get("rooms", [])[:3]
    except (OSError, ValueError):
        return "", []


CAL_IDS = home_cfg("calendars", [])   # HA calendar entity ids


def cal_ascii(t):
    import unicodedata
    t = unicodedata.normalize("NFKD", t or "").encode("ascii", "ignore").decode()
    return " ".join(t.split())[:39] or "(no title)"


async def cal_push(peer):
    """The coming week's events from HA's calendars to the watch: cal clr / cal <ymd> <s> <e> <c> <title> / cal end."""
    import urllib.parse
    now = datetime.datetime.now().astimezone()
    d0 = now.replace(hour=0, minute=0, second=0, microsecond=0)
    d2 = d0 + datetime.timedelta(days=7)
    q = "?start=%s&end=%s" % (urllib.parse.quote(d0.isoformat()), urllib.parse.quote(d2.isoformat()))
    evs = []
    for ci, cid in enumerate(CAL_IDS):
        r = await ha_call("GET", "/api/calendars/" + cid + q)
        if r is None:
            continue
        for e in r:
            st, en = e.get("start", {}), e.get("end", {})
            try:
                if "dateTime" in st:
                    a = datetime.datetime.fromisoformat(st["dateTime"]).astimezone()
                    b = datetime.datetime.fromisoformat(en.get("dateTime", st["dateTime"])).astimezone()
                    days = [(a, a.hour * 60 + a.minute, (b.hour * 60 + b.minute) if b.date() == a.date() else 1440)]
                else:
                    a = datetime.date.fromisoformat(st["date"])
                    b = datetime.date.fromisoformat(en.get("date", st["date"]))
                    days = []
                    x = a
                    while x < b or x == a:
                        days.append((x, -1, 1440))
                        x += datetime.timedelta(days=1)
                for dd, s, e2 in days:
                    dd = dd.date() if hasattr(dd, "date") and callable(dd.date) else dd
                    if d0.date() <= dd < d2.date():
                        evs.append((int(dd.strftime("%Y%m%d")), s, e2, ci, cal_ascii(e.get("summary"))))
            except (KeyError, ValueError) as ex:
                log.info("cal: %s", ex)
    evs.sort(key=lambda v: (v[0], v[1], v[3]))
    log.info("cal: %d events to the watch", len(evs))
    peer.send_line("n cal clr")
    for v in evs[:14]:
        peer.send_line("n cal %d %d %d %d %s" % v)
    peer.send_line("n cal end")


async def wx_push(peer):
    """Home Assistant's first weather entity to the watch: "wx <temp> <condition>"."""
    try:
        for d in await ha_call("GET", "/api/states") or []:
            if d.get("entity_id", "").startswith("weather."):
                t = d.get("attributes", {}).get("temperature")
                if t is not None:
                    peer.send_line("n wx %s %s" % (t, d.get("state", "cloudy")))
                return
    except Exception as e:  # noqa: BLE001 - no HA: no weather on the watch
        log.info("wx: %s", e)


ha_steps_sent = {}


async def ha_steps(steps, bat, dock):
    """The watch's steps and battery as Home Assistant sensors, when they change (at most once a minute)."""
    now = time.time()
    last = ha_steps_sent.get("v")
    if last == (steps, bat, dock) or now - ha_steps_sent.get("t", 0) < 60:
        return
    ha_steps_sent.update(v=(steps, bat, dock), t=now)
    try:
        await ha_call("POST", "/api/states/sensor.kode_watch_steps",
                      {"state": steps, "attributes": {"friendly_name": "Kode Watch steps", "unit_of_measurement": "steps",
                                                      "icon": "mdi:shoe-print", "state_class": "total_increasing"}})
        await ha_call("POST", "/api/states/sensor.kode_watch_battery",
                      {"state": bat, "attributes": {"friendly_name": "Kode Watch battery", "unit_of_measurement": "%",
                                                    "device_class": "battery", "charging": dock}})
    except Exception as e:  # noqa: BLE001 - HA away: try again with the next status
        ha_steps_sent.pop("v", None)
        log.info("ha steps: %s", e)


async def ha_call(method, path, body=None):
    url, _ = home_rooms()
    try:
        tok = HA_TOKEN.read_text().strip()
    except OSError as e:
        log.warning("home assistant: no token (%s)", e)
        return None
    if not url or not tok:
        return None
    import aiohttp
    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=8)) as cs:
            async with cs.request(method, url + path, json=body,
                                  headers={"Authorization": "Bearer " + tok}) as r:
                if r.status >= 300:
                    log.warning("home assistant %s %s: HTTP %s", method, path, r.status)
                    return None
                if method == "POST":
                    log.info("home assistant %s: HTTP %s", path, r.status)
                return await r.json()
    except Exception as e:  # noqa: BLE001 - a dead HA must not break the hub
        log.warning("home assistant %s %s: %s", method, path, e)
        return None


async def home_bits():
    _, rooms = home_rooms()
    states = await ha_call("GET", "/api/states")
    if states is None:
        return None
    on = {st["entity_id"] for st in states if st.get("state") == "on"}
    return sum(1 << i for i, r in enumerate(rooms) if any(e in on for e in r.get("entities", [])))


async def home_switch(i, want):
    _, rooms = home_rooms()
    targets = [r for k, r in enumerate(rooms) if k == i] if i < 3 else rooms
    for r in targets:
        for e in r.get("entities", []):
            dom = e.split(".")[0]
            await ha_call("POST", f"/api/services/{dom}/turn_{'on' if want else 'off'}", {"entity_id": e})


async def home_push(peer, names=False):
    if names:
        _, rooms = home_rooms()
        for i, r in enumerate(rooms):
            peer.send_line(f"n hn {i} {str(r.get('name', ''))[:19]}")
    b = await home_bits()
    if b is not None and b != getattr(peer, "home_last", None):
        peer.home_last = b
        peer.send_line(f"n hs {b}")


# ---- Music: the watch's Music app drives HA media players ------------------
# Watch -> hub: "mu ?|pp|next|prev|vol+|vol-|sel". Hub -> watch:
# "mu <state> <vol 0-100|-1> <i> <n> <player>|<title>|<artist>".
MU_PREFER = home_cfg("music_prefer", "")   # preferred HA media_player
mu_sel = {}   # user -> entity_id


def mu_clean(t, n):
    import unicodedata
    t = unicodedata.normalize("NFKD", str(t or "")).encode("ascii", "ignore").decode()
    return " ".join(t.replace("|", "/").split())[:n]


async def mu_players():
    st = await ha_call("GET", "/api/states") or []
    return [d for d in st if d["entity_id"].startswith("media_player.") and d.get("state") != "unavailable"
            and not d["entity_id"].startswith("media_player.undefined")]


async def mu_push(peer, force=False):
    ps = await mu_players()
    if not ps:
        return
    ids = [d["entity_id"] for d in ps]
    cur = mu_sel.get(peer.user)
    if cur not in ids:
        playing = [d["entity_id"] for d in ps if d.get("state") == "playing"]
        cur = playing[0] if playing else (MU_PREFER if MU_PREFER in ids else ids[0])
        mu_sel[peer.user] = cur
    d = ps[ids.index(cur)]
    at = d.get("attributes", {})
    vol = at.get("volume_level")
    line = "n mu %s %d %d %d %s|%s|%s" % (mu_clean(d.get("state"), 11) or "off", -1 if vol is None else round(vol * 100),
                                          ids.index(cur), len(ids), mu_clean(at.get("friendly_name", cur), 30),
                                          mu_clean(at.get("media_title"), 60), mu_clean(at.get("media_artist"), 46))
    if force or line != getattr(peer, "mu_last", None):
        peer.mu_last = line
        peer.send_line(line)


async def mu_cmd(peer, cmd):
    if cmd == "sel":
        ids = [d["entity_id"] for d in await mu_players()]
        if ids:
            cur = mu_sel.get(peer.user)
            mu_sel[peer.user] = ids[(ids.index(cur) + 1) % len(ids)] if cur in ids else ids[0]
    elif cmd != "?":
        eid = mu_sel.get(peer.user)
        svc = {"pp": "media_play_pause", "next": "media_next_track", "prev": "media_previous_track",
               "vol+": "volume_up", "vol-": "volume_down"}.get(cmd)
        if eid and svc:
            await ha_call("POST", "/api/services/media_player/" + svc, {"entity_id": eid})
            await asyncio.sleep(0.8)
    await mu_push(peer, force=True)


# ---- Find iPhone: the watch asks HA to ring the user's phone -----------------
# Watch -> hub: "ph ?" (state) / "ph ring". Hub -> watch: "ph <battery> <where>", "ph rang".
PHONES = home_cfg("phones", {})   # user -> HA mobile_app device name


async def ph_cmd(peer, cmd):
    dev = PHONES.get(peer.user)
    if not dev:
        peer.send_line("n ph -1 no phone set up")
        return
    if cmd == "ring":
        r = await ha_call("POST", "/api/services/notify/mobile_app_" + dev, {
            "title": "Kode Watch", "message": "Here I am!",
            "data": {"push": {"sound": {"name": "default", "critical": 1, "volume": 1.0}}}})
        if r is not None:
            peer.send_line("n ph rang")
    bat = await ha_call("GET", "/api/states/sensor.%s_battery_level" % dev) or {}
    where = await ha_call("GET", "/api/states/device_tracker." + dev) or {}
    w = str(where.get("state", "unknown"))
    w = {"home": "at home", "not_home": "away"}.get(w, w.replace("_", " "))
    try:
        b = int(float(bat.get("state")))
    except (TypeError, ValueError):
        b = -1
    peer.send_line("n ph %d %s" % (b, mu_clean(w, 22) or "unknown"))


# ---- Pairing a watch without a cable ---------------------------------------
# A watch with no key opens /ws-dev as "Bearer pair". It gets a six-digit code
# and shows it; someone logged in (Authelia, Remote-User) types the code on
# /pair, and the waiting socket is sent that user's watch key. The code alone
# gives nothing: only a logged-in approval releases a key.
PAIRS = {}            # code -> {"ws", "until", "fut"}
PAIR_MAX = 4          # waiting watches at once
PAIR_TTL = 600        # seconds a code stands


def watch_key_for(user):
    d = load_devices()
    for k, v in d.items():
        if v.get("user") == user and v.get("role") == "watch":
            return k
    k = secrets.token_urlsafe(24)
    d[k] = {"user": user, "role": "watch", "name": "kode-watch"}
    save_devices(d)
    return k


async def pair_session(request):
    now = time.monotonic()
    for c in [c for c, v in PAIRS.items() if v["until"] < now or v["ws"].closed]:
        PAIRS.pop(c, None)
    if len(PAIRS) >= PAIR_MAX:
        return web.json_response({"error": "too many pairings"}, status=429)
    ws = web.WebSocketResponse(heartbeat=30, max_msg_size=1 << 10, compress=False)
    await ws.prepare(request)
    code = f"{secrets.randbelow(10 ** 6):06d}"
    while code in PAIRS:
        code = f"{secrets.randbelow(10 ** 6):06d}"
    fut = asyncio.get_running_loop().create_future()
    PAIRS[code] = {"ws": ws, "until": now + PAIR_TTL, "fut": fut}
    log.info("pairing: code handed out (%s waiting)", len(PAIRS))
    await ws.send_str(f"pair {code}")

    async def drain():
        async for _ in ws:
            pass

    dt = asyncio.create_task(drain())
    try:
        done, _ = await asyncio.wait({fut, dt}, timeout=PAIR_TTL, return_when=asyncio.FIRST_COMPLETED)
        if fut in done and not ws.closed:
            await ws.send_str(f"key {fut.result()}")
            log.info("pairing: key sent")
            await asyncio.sleep(0.5)
    finally:
        PAIRS.pop(code, None)
        dt.cancel()
        await ws.close()
    return ws


PAIR_PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Pair a watch</title>
<style>body{font-family:system-ui,sans-serif;background:#0b0c0e;color:#e8eaed;display:grid;place-items:center;min-height:100vh;margin:0}
main{max-width:340px;padding:24px}h1{font-size:22px}input{font-size:32px;letter-spacing:6px;width:100%;box-sizing:border-box;padding:10px;border-radius:12px;border:1px solid #444;background:#16181c;color:#fff;text-align:center}
button{margin-top:14px;width:100%;font-size:18px;padding:12px;border:0;border-radius:12px;background:#33d17a;color:#000;font-weight:700}#m{margin-top:14px;min-height:1.4em}</style></head>
<body><main><h1>Pair a Kode-watch</h1><p>Type the six digits the watch shows.</p>
<input id="c" inputmode="numeric" maxlength="7" autocomplete="off" autofocus><button id="b">Pair</button><div id="m"></div></main>
<script>const c=document.getElementById("c"),m=document.getElementById("m");
document.getElementById("b").onclick=async()=>{const code=c.value.replace(/\\D/g,"");m.textContent="...";
const r=await fetch("api/pair",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({code})});
const j=await r.json().catch(()=>({}));m.textContent=r.ok?"Paired. The watch connects in a few seconds.":(j.error||("error "+r.status));};
c.onkeydown=e=>{if(e.key==="Enter")document.getElementById("b").click();};</script></body></html>"""


async def pair_page(request):
    if not request.headers.get("Remote-User"):
        return web.Response(status=401, text="login needed")
    return web.Response(text=PAIR_PAGE, content_type="text/html")


async def api_pair(request):
    user = request.headers.get("Remote-User", "")
    if not user:
        return web.json_response({"error": "login needed"}, status=401)
    try:
        code = str((await request.json()).get("code", "")).strip()
    except (ValueError, AttributeError):
        code = ""
    v = PAIRS.get(code)
    if not v or v["fut"].done() or v["ws"].closed:
        return web.json_response({"error": "no watch is waiting with that code"}, status=404)
    v["fut"].set_result(watch_key_for(user))
    log.info("pairing: approved by %s", user)
    return web.json_response({"ok": True})


# ---- Updates over Wi-Fi ------------------------------------------------------
# The watch says "ver <version> <slot>" when it connects and "ota <pct>|done|fail <e>"
# while it updates. /update (behind the login) lists the OS images in
# static/flash and sends the chosen one: "ota <https url>".
FLASH_DIR = pathlib.Path(__file__).with_name("static") / "flash"
OTA_BASE = home_cfg("public_url", "https://hub.example.com").rstrip("/") + "/static/flash/"
watch_state = {}      # user -> {"ver", "slot", "ota"}
watch_log = {}        # user -> deque of the console lines the watch sends ("log ...")


def os_images():
    # Only the 32 MB layout (0.9.0 and up): an older image has the old partition table.
    out = []
    for f in FLASH_DIR.glob("kw-*-app.bin"):
        try:
            v = tuple(int(x) for x in f.name[3:-8].split("."))
        except ValueError:
            continue
        if v >= (0, 9, 0):
            out.append((v, f.name))
    return [n for _, n in sorted(out, reverse=True)]


UPDATE_PAGE = pathlib.Path(__file__).with_name("update.html")   # behind the login, not in static/


async def api_log(request):
    """The watch's console, as it sends it ("log <line>"), newest last."""
    user = request.headers.get("Remote-User", "")
    if not user:
        return web.Response(status=401)
    lines = list(watch_log.get(user, []))
    return web.Response(text="\n".join(lines) + "\n", headers={"Cache-Control": "no-store"})


# ---- the watch's files (/sdcard, /data) for /update: "fs ..." over the device link ----
FS_MAX = 32 << 20
FS_CHUNK = 3072
fs_jobs = {}      # user -> the request on the air: {"ents", "data", "ack", "end", "ev"}
fs_locks = {}


def fs_feed(user, text):
    j = fs_jobs.get(user)
    if j is None:
        return
    if text.startswith("fs ent "):
        j["ents"].append(text[7:])
    elif text.startswith("fs size "):
        try:
            j["size"] = int(text[8:])
        except ValueError:
            pass
    elif text.startswith("fs ack "):
        try:
            j["ack"] = int(text[7:])
        except ValueError:
            pass
    elif text.startswith("fs end "):
        j["end"] = text[7:]
    j["ev"].set()


def fs_path_ok(p):
    return (len(p) < 200 and ".." not in p and "\n" not in p
            and (p in ("/sdcard", "/data") or p.startswith("/sdcard/") or p.startswith("/data/")))


async def fs_wait(j, until, idle=15.0):
    """Wait until until(j) is true; each answer from the watch restarts the idle clock."""
    while not until(j):
        j["ev"].clear()
        try:
            await asyncio.wait_for(j["ev"].wait(), idle)
        except asyncio.TimeoutError:
            return False
    return True


async def api_fs(request):
    user = request.headers.get("Remote-User", "")
    if not user:
        return web.Response(status=401)
    w = hub.get(user, {}).get("watch")
    if not isinstance(w, DevicePeer) or w.closed:
        return web.json_response({"error": "the watch is not connected"}, status=409)
    op = request.query.get("op", "")
    path = request.query.get("path", "")
    if not fs_path_ok(path):
        return web.json_response({"error": "only /sdcard and /data"}, status=400)
    lock = fs_locks.setdefault(user, asyncio.Lock())
    async with lock:
        j = {"ents": [], "data": bytearray(), "ack": -1, "end": None, "ev": asyncio.Event(), "size": -1}
        fs_jobs[user] = j
        try:
            if op == "ls":
                w.send_line("n fs ls " + path)
                if not await fs_wait(j, lambda j: j["end"] is not None, 10):
                    return web.json_response({"error": "the watch did not answer"}, status=504)
                if not j["end"].startswith("ls ") or j["end"] == "ls fail":
                    return web.json_response({"error": "cannot read " + path}, status=404)
                out = []
                for e in j["ents"]:
                    t, _, rest = e.partition(" ")
                    size, _, name = rest.partition(" ")
                    out.append({"name": name, "dir": t == "d", "size": int(size or 0)})
                return web.json_response({"path": path, "entries": out})
            if op == "get":
                w.send_line("n fs get " + path)
                ok = await fs_wait(j, lambda j: j["end"] is not None, 20)
                if not ok or j["end"] != "get ok":
                    return web.json_response({"error": "download failed"}, status=502)
                name = path.rsplit("/", 1)[-1] or "file"
                return web.Response(body=bytes(j["data"]), content_type="application/octet-stream",
                                    headers={"Content-Disposition": 'attachment; filename="%s"' % name.replace('"', ""),
                                             "Cache-Control": "no-store"})
            if request.method != "POST":
                return web.json_response({"error": "use POST"}, status=405)
            if op in ("rm", "mkdir"):
                w.send_line("n fs %s %s" % (op, path))
                ok = await fs_wait(j, lambda j: j["end"] is not None, 10)
                if not ok or j["end"] != op + " ok":
                    return web.json_response({"error": op + " failed"}, status=502)
                return web.json_response({"ok": True})
            if op == "put":
                data = bytearray()
                async for chunk in request.content.iter_chunked(65536):
                    data += chunk
                    if len(data) > FS_MAX:
                        return web.json_response({"error": "too big (32 MB at most)"}, status=413)
                w.send_line("n fs put %d %s" % (len(data), path))
                if not await fs_wait(j, lambda j: j["ack"] == 0 or j["end"] is not None, 10) or j["end"] is not None:
                    return web.json_response({"error": "the watch cannot write " + path}, status=502)
                sent = 0
                while sent < len(data):
                    part = bytes(data[sent:sent + FS_CHUNK])
                    await w.ws.send_bytes(b"KFS2" + part)
                    sent += len(part)
                    if not await fs_wait(j, lambda j, n=sent: j["ack"] >= n or j["end"] is not None):
                        return web.json_response({"error": "upload stalled"}, status=504)
                    if j["end"] is not None and sent < len(data):
                        return web.json_response({"error": "upload failed"}, status=502)
                if not await fs_wait(j, lambda j: j["end"] is not None, 20) or j["end"] != "put ok":
                    return web.json_response({"error": "upload failed"}, status=502)
                return web.json_response({"ok": True, "bytes": len(data)})
            return web.json_response({"error": "unknown op"}, status=400)
        finally:
            fs_jobs.pop(user, None)


async def api_shot(request):
    """The watch's screen as it streams it (KWS1 runs); asking for it keeps the stream going."""
    user = request.headers.get("Remote-User", "")
    if not user:
        return web.Response(status=401)
    w = hub.get(user, {}).get("watch")
    if isinstance(w, DevicePeer) and not w.closed:
        w.send_line("n shot")
    st = watch_state.get(user, {})
    if "shot" not in st:
        return web.Response(status=204)
    return web.Response(body=st["shot"], content_type="application/octet-stream",
                        headers={"Cache-Control": "no-store", "X-Shot": str(st.get("shot_n", 0))})


async def update_page(request):
    if not request.headers.get("Remote-User"):
        return web.Response(status=401, text="login needed")
    return web.Response(text=UPDATE_PAGE.read_text(), content_type="text/html")


async def api_update(request):
    user = request.headers.get("Remote-User", "")
    if not user:
        return web.json_response({"error": "login needed"}, status=401)
    w = hub.get(user, {}).get("watch")
    online = isinstance(w, DevicePeer) and not w.closed
    st = watch_state.get(user, {})
    if request.method == "GET":
        cat = []
        try:
            for line in (pathlib.Path(__file__).with_name("static") / "watchapps" / "index.txt").read_text().splitlines():
                f = line.split("|")
                if len(f) >= 6:
                    cat.append({"id": f[0], "name": f[1], "size": int(f[2]), "color": f[4], "desc": f[5], "pad": len(f) > 6 and "pad" in f[6]})
        except (OSError, ValueError):
            pass
        return web.json_response({"online": online, "images": os_images(), "catalog": cat,
                                  **{k: v for k, v in st.items() if k not in ("shot", "shot_n")}})
    try:
        body = await request.json()
        f = str(body.get("file", ""))
    except (ValueError, AttributeError):
        body, f = {}, ""
    tests = {
        "note": ["notify Test notification from the hub"],
        "alarm": ["timer reset", "timer add -297", "timer toggle"],   # rings in 3 s
        "find": ["find"],
        "wake": ["alarm ring"],
        "notifs": ["go 1"],
        "face": ["go 0"],
    }
    d = datetime.date.today()
    t1, t2 = int(d.strftime("%Y%m%d")), int((d + datetime.timedelta(days=1)).strftime("%Y%m%d"))
    tests["agenda"] = ["cal clr", "cal %d -1 1440 2 Papier container" % t1, "cal %d 1080 1140 0 Eten bij oma" % t1,
                       "cal %d 1260 1320 1 Gym met Sam" % t1, "cal %d 540 600 3 Verjaardag Alex (een hele lange titel)" % t2,
                       "cal end"]
    if body.get("test") in tests:
        if not online:
            return web.json_response({"error": "the watch is not connected"}, status=409)
        for line in tests[body["test"]]:
            w.send_line("n " + line)
        log.info("test %s sent to %s", body["test"], user)
        return web.json_response({"ok": True})
    ctl = body.get("ctl")
    if ctl:
        if not online:
            return web.json_response({"error": "the watch is not connected"}, status=409)
        def n(k, lo, hi):
            v = int(body.get(k, 0))
            return max(lo, min(hi, v))
        if ctl == "tap":
            line = f"rtap {n('x', 0, 409)} {n('y', 0, 501)}"
        elif ctl == "swipe":
            line = f"rswipe {n('x0', 0, 409)} {n('y0', 0, 501)} {n('x1', 0, 409)} {n('y1', 0, 501)}"
        elif ctl in ("top", "bot"):
            line = "rbtn " + ctl
        elif ctl == "os" and re.fullmatch(r"[A-Za-z][A-Za-z -]{0,19}", str(body.get("name", ""))):
            line = "os open " + body["name"]
        elif ctl == "open" and re.fullmatch(r"[a-z0-9_]{1,23}", str(body.get("id", ""))):
            line = "app open " + body["id"]
        elif ctl in ("remove", "get") and re.fullmatch(r"[a-z0-9_]{1,23}", str(body.get("id", ""))):
            line = "app " + ctl + " " + body["id"]
        else:
            return web.json_response({"error": "unknown control"}, status=400)
        w.send_line("n " + line)
        w.send_line("n shot")
        return web.json_response({"ok": True})
    if body.get("kody"):
        if not online:
            return web.json_response({"error": "the watch is not connected"}, status=409)
        kb = FLASH_DIR / "kody-2.bin"
        sha = hashlib.sha256(kb.read_bytes()).hexdigest()
        w.send_line(f"n kody {OTA_BASE}kody-2.bin {sha}")
        st["ota"] = "kody sent"
        watch_state[user] = st
        log.info("update: Kody worlds sent to %s", user)
        return web.json_response({"ok": True})
    if f not in os_images():
        return web.json_response({"error": "no such image"}, status=404)
    if not online:
        return web.json_response({"error": "the watch is not connected"}, status=409)
    w.send_line("n ota " + OTA_BASE + f)
    if body.get("drop"):
        # free the watch's RAM of this link's TLS while it downloads (Bluetooth builds are tight)
        async def drop():
            await asyncio.sleep(0.4)
            try:
                await w.ws.close()
            except Exception:  # noqa: BLE001
                pass
        asyncio.get_running_loop().create_task(drop())
    st["ota"] = "sent"
    watch_state[user] = st
    log.info("update: %s sent to %s", f, user)
    return web.json_response({"ok": True})


async def ws_dev(request):
    auth = request.headers.get("Authorization", "")
    key = auth[7:].strip() if auth.startswith("Bearer ") else ""
    if key == "pair":
        return await pair_session(request)
    dev = load_devices().get(key) if key else None
    if not dev:
        return web.json_response({"error": "onbekend apparaat"}, status=403)
    ws = web.WebSocketResponse(heartbeat=30, max_msg_size=1 << 18, compress=False)
    await ws.prepare(request)
    peer = DevicePeer(ws, dev["user"], dev.get("role", "watch"))
    peer.join()
    log.info("apparaat %s (%s) verbonden", dev.get("name"), dev["user"])

    async def clock():
        while not ws.closed:
            await asyncio.sleep(6 * 3600)
            peer.send_line(wall_clock_line())

    async def home_watch():
        await home_push(peer, names=True)
        await wx_push(peer)
        await cal_push(peer)
        k = 0
        while not ws.closed:
            await asyncio.sleep(20)
            await home_push(peer)
            await mu_push(peer)
            k += 1
            if k % 30 == 0:
                await wx_push(peer)
                await cal_push(peer)

    ct = asyncio.create_task(clock())
    ht = asyncio.create_task(home_watch())
    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                await peer.from_device(msg.data)
            elif msg.type == WSMsgType.BINARY and msg.data[:4] == b"KFS1":
                j = fs_jobs.get(peer.user)
                if j is not None and len(j["data"]) < FS_MAX:
                    j["data"] += msg.data[4:]
                    j["ev"].set()
            elif msg.type == WSMsgType.BINARY and msg.data[:4] == b"KWS1":
                st = watch_state.setdefault(peer.user, {})
                st["shot"] = bytes(msg.data)
                st["shot_n"] = st.get("shot_n", 0) + 1
            elif msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                break
    finally:
        ct.cancel()
        ht.cancel()
        peer.leave()
        log.info("apparaat %s weg", dev.get("name"))
    return ws


async def ws_handler(request):
    ws = web.WebSocketResponse(heartbeat=20, max_msg_size=1 << 20, compress=False)
    await ws.prepare(request)
    app_id = request.query.get("app", "")
    if not app_id or "/" in app_id or app_id.startswith("."):
        await ws.send_str(json.dumps({"t": "error", "s": "no app chosen"}))
        await ws.close()
        return ws
    if app_id in PRIVATE_APPS and not request.headers.get("Remote-User"):
        await ws.send_str(json.dumps({"t": "error", "s": "this app needs a login"}))
        await ws.close()
        return ws
    if len(sessions) >= MAX_SESSIONS:
        await ws.send_str(json.dumps({"t": "error", "s": "too many sims running right now, try again in a minute"}))
        await ws.close()
        return ws
    role = request.query.get("role", "dot")
    role = role if role in ("dot", "watch") else "dot"
    # Gasten (publiek, geen Authelia) krijgen elk een eigen hub, zodat ze elkaars Link niet zien.
    s = Session(ws, app_id, role, request.headers.get("Remote-User", "") or f"anon-{id(ws):x}")
    sessions.add(s)
    tasks = []
    try:
        await s.start()
        s.join_hub()
        await s.send_json(t="started", app=app_id, role=role)
        tasks = [asyncio.create_task(s.pump_sim()), asyncio.create_task(s.pump_frames())]
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    s.on_browser(json.loads(msg.data))
                except (ValueError, TypeError):
                    pass
            elif msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                break
    except Exception as e:  # noqa: BLE001 - the browser gets the reason
        log.exception("sessie %s", app_id)
        if not ws.closed:
            await ws.send_str(json.dumps({"t": "error", "s": str(e)}))
    finally:
        await s.stop()
        for t in tasks:
            t.cancel()
        sessions.discard(s)
        if not ws.closed:
            await ws.close()
    return ws


def make_app():
    app = web.Application(middlewares=[no_cache])
    app.router.add_get("/", index)
    app.router.add_get("/api/status", status)
    app.router.add_get("/watch", watch_page)
    app.router.add_get("/docs", docs_page)
    app.router.add_get("/watch-docs", watch_docs_page)
    app.router.add_post("/api/notify", api_notify)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/ws-dev", ws_dev)
    app.router.add_get("/api/devkey", api_devkey)
    app.router.add_get("/pair", pair_page)
    app.router.add_get("/update", update_page)
    app.router.add_get("/api/update", api_update)
    app.router.add_post("/api/update", api_update)
    app.router.add_get("/api/shot", api_shot)
    app.router.add_get("/api/log", api_log)
    app.router.add_get("/api/fs", api_fs)
    app.router.add_post("/api/fs", api_fs)
    app.router.add_post("/api/pair", api_pair)
    app.router.add_post("/api/build", api_build)
    app.router.add_get("/api/build/{id}", api_job)
    app.router.add_get("/api/sources", api_sources)
    app.router.add_get("/api/icon/{id}", api_icon)
    app.router.add_get("/api/source/{id}", api_source)
    app.router.add_post("/api/play", api_play)
    app.router.add_static("/static/", STATIC)
    return app


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    web.run_app(make_app(), host="0.0.0.0", port=PORT, access_log=None)
