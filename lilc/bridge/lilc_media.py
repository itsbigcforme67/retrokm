"""Pictures and videos Claude sent in your Claude Code chats, for lil' C.

Finds SendUserFile calls in the session transcripts under ~/.claude/projects,
and converts the files with ffmpeg into what the 320x240 screen can show:
  thumb  96x72 JPEG          image  320x240 JPEG (letterboxed)
  video  10 fps 320x240 JPEG frames + 16 kHz mono PCM audio
"""
import calendar
import glob
import hashlib
import json
import os
import shutil
import subprocess
import threading
import time

HOME = os.path.expanduser("~")
CACHE = os.path.join(HOME, ".cache", "lilc-media")
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"}
VIDEO_EXT = {".mp4", ".webm", ".mov", ".mkv", ".avi"}
VIDEO_FPS = 10
VIDEO_MAX_S = 90


def _fit(w, h):
    return (f"scale={w}:{h}:force_original_aspect_ratio=decrease,"
            f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:black")


def _ffmpeg(*args):
    return subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *args],
                          capture_output=True, timeout=300).returncode == 0


class Media:
    def __init__(self, titles_fn):
        self.titles_fn = titles_fn   # () -> {cli session id: title}
        self.items = {}              # id -> item
        self.offsets = {}            # transcript path -> bytes already read
        self.lock = threading.Lock()
        self.converting = set()
        os.makedirs(CACHE, exist_ok=True)

    # ------------------------------------------------------------ index

    def scan(self):
        """Read new lines of every transcript, picking up sent files."""
        for path in glob.glob(os.path.join(HOME, ".claude/projects/*/*.jsonl")):
            if "lilc-workspace" in path:
                continue
            size = os.path.getsize(path)
            start = self.offsets.get(path, 0)
            if size < start:
                start = 0
            if size == start:
                continue
            with open(path, "rb") as f:
                f.seek(start)
                data = f.read()
            end = data.rfind(b"\n") + 1  # leave a half-written line for later
            self.offsets[path] = start + end
            for line in data[:end].split(b"\n"):
                if b"SendUserFile" in line:
                    self._parse(line)

    def _parse(self, line):
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            return
        content = (d.get("message") or {}).get("content")
        if d.get("type") != "assistant" or not isinstance(content, list):
            return
        for b in content:
            if b.get("type") != "tool_use" or b.get("name") != "SendUserFile":
                continue
            inp = b.get("input") or {}
            for p in inp.get("files") or []:
                if not os.path.isabs(p):
                    p = os.path.join(d.get("cwd") or HOME, p)
                ext = os.path.splitext(p)[1].lower()
                kind = ("image" if ext in IMAGE_EXT else
                        "video" if ext in VIDEO_EXT else None)
                if not kind:
                    continue
                mid = hashlib.sha1(p.encode()).hexdigest()[:10]
                with self.lock:
                    self.items[mid] = {
                        "id": mid, "path": p, "kind": kind,
                        "caption": inp.get("caption") or os.path.basename(p),
                        "session": d.get("sessionId", ""),
                        "t": _ts(d.get("timestamp")),
                    }

    def listing(self, limit=40):
        self.scan()
        titles = self.titles_fn()
        now = time.time()
        with self.lock:
            items = sorted(self.items.values(), key=lambda i: -i["t"])
        out = []
        for i in items:
            if not os.path.exists(i["path"]):
                continue
            out.append({"id": i["id"], "kind": i["kind"],
                        "caption": i["caption"],
                        "title": titles.get(i["session"], "Claude"),
                        "ago": int(now - i["t"])})
            if len(out) >= limit:
                break
        return out

    def newest(self):
        lst = self.listing(1)
        return lst[0]["id"] if lst else ""

    # ------------------------------------------------------------ files

    def _dir(self, mid):
        item = self.items.get(mid)
        if not item:
            return None, None
        st = os.stat(item["path"])
        d = os.path.join(CACHE, f"{mid}-{int(st.st_mtime)}")
        return item, d

    def still(self, mid, size):
        """JPEG bytes: 'thumb' (96x72) or 'image' (320x240)."""
        item, d = self._dir(mid)
        if not item:
            return None
        out = os.path.join(d, size + ".jpg")
        if not os.path.exists(out):
            os.makedirs(d, exist_ok=True)
            w, h = (96, 72) if size == "thumb" else (320, 240)
            # For a video, take a frame a second in
            pre = ["-ss", "1"] if item["kind"] == "video" else []
            if not _ffmpeg(*pre, "-i", item["path"], "-frames:v", "1",
                           "-vf", _fit(w, h), "-q:v", "4", out):
                if pre and not _ffmpeg("-i", item["path"], "-frames:v", "1",
                                       "-vf", _fit(w, h), "-q:v", "4", out):
                    return None
        with open(out, "rb") as f:
            return f.read()

    def video_info(self, mid):
        """Start converting a video if needed; report progress."""
        item, d = self._dir(mid)
        if not item or item["kind"] != "video":
            return None
        done = os.path.join(d, "done")
        if os.path.exists(done):
            frames = len(glob.glob(os.path.join(d, "f*.jpg")))
            audio = os.path.join(d, "audio.pcm")
            return {"ready": True, "frames": frames, "fps": VIDEO_FPS,
                    "audio": os.path.getsize(audio) if os.path.exists(audio) else 0}
        with self.lock:
            if mid not in self.converting:
                self.converting.add(mid)
                threading.Thread(target=self._convert, args=(item, d, mid),
                                 daemon=True).start()
        return {"ready": False}

    def _convert(self, item, d, mid):
        tmp = d + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        src = item["path"]
        _ffmpeg("-i", src, "-t", str(VIDEO_MAX_S), "-an",
                "-vf", f"fps={VIDEO_FPS}," + _fit(320, 240),
                "-q:v", "7", os.path.join(tmp, "f%05d.jpg"))
        _ffmpeg("-i", src, "-t", str(VIDEO_MAX_S), "-vn", "-ac", "1",
                "-ar", "16000", "-f", "s16le", os.path.join(tmp, "audio.pcm"))
        for name in ("thumb.jpg", "image.jpg"):  # keep stills made earlier
            if os.path.exists(os.path.join(d, name)):
                shutil.copy(os.path.join(d, name), tmp)
        open(os.path.join(tmp, "done"), "w").close()
        shutil.rmtree(d, ignore_errors=True)
        os.rename(tmp, d)
        with self.lock:
            self.converting.discard(mid)

    def frames(self, mid, start, count):
        """Frames start..start+count-1 as [u32 LE length][JPEG] records."""
        item, d = self._dir(mid)
        if not item:
            return None
        out = bytearray()
        for n in range(start, start + count):
            p = os.path.join(d, "f%05d.jpg" % (n + 1))
            if not os.path.exists(p):
                break
            with open(p, "rb") as f:
                jpg = f.read()
            out += len(jpg).to_bytes(4, "little") + jpg
        return bytes(out)

    def audio(self, mid):
        item, d = self._dir(mid)
        p = os.path.join(d, "audio.pcm") if d else ""
        if not os.path.exists(p):
            return None
        with open(p, "rb") as f:
            return f.read()


def _ts(s):
    try:
        return float(calendar.timegm(time.strptime(s[:19], "%Y-%m-%dT%H:%M:%S")))
    except (TypeError, ValueError):
        return 0.0
