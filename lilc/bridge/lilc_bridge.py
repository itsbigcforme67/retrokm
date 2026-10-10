#!/usr/bin/env python3
"""lil' C bridge: runs on your PC and connects the Stack-chan to Claude.

Questions go through your logged-in Claude Code CLI (`claude -p`), so they use
your Claude subscription. Task status is read from the Claude desktop app's
session files and Claude Code transcripts under ~/.claude/projects.

Endpoints (all need header  X-LilC-Key: <key from config.json>):
  GET  /api/ping              -> {"ok", "stt", "tts"}
  GET  /api/tasks             -> {"tasks": [...], "needs": n, "working": n}
  POST /api/ask   {"text"}    -> {"job": n}
  POST /api/listen  raw PCM   -> {"job": n}, speech-to-text first
                  (16 kHz, signed 16-bit little-endian, mono)
  GET  /api/job/<n>           -> {"done", "heard", "reply", "audio"}
  POST /api/reset             -> forget the conversation
  GET  /api/audio/<n>         -> raw s16le mono PCM, rate in X-Rate header
  GET  /api/media             -> pictures/videos Claude sent in your chats
  GET  /api/media/<id>/thumb|image       -> 96x72 / 320x240 JPEG
  GET  /api/media/<id>/video             -> {"ready", "frames", "fps", "audio"}
  GET  /api/media/<id>/frames/<from>/<n> -> [u32 length][JPEG] records
  GET  /api/media/<id>/audio             -> 16 kHz s16le mono PCM
  GET  /api/presence          -> where lil' C is: {"where", "label", "here", ...}
  POST /api/summon            -> call him to this device
  POST /api/locate {"dev", "kind": "speakers"|"voice"} -> {"id"}: listen for
                              where things are (lilc_locate.py; locate.py starts one)
  GET  /api/locate/<id>       -> how it went
  POST /api/record/<id>/start, POST /api/record/<id> raw PCM (X-Rate,
                              X-Channels): the device's side of a locate

Devices say which home they are with the header X-LilC-Dev (stackchan, tab5).
"""
import glob
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import queue
import threading
import time
import wave
import io
from lilc_media import Media
from lilc_speakspell import speak_and_spell
from lilc_desk import DeskLink
from lilc_presence import Presence
from lilc_locate import Locator
try:
    from piper.config import SynthesisConfig
except ImportError:
    SynthesisConfig = None
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")
HOME = os.path.expanduser("~")
# Kept outside ~/.claude/projects naming of real work so it is easy to skip
WORKSPACE = os.path.join(HOME, ".cache", "lilc-workspace")

SYSTEM_PROMPT = (
    "You are lil' C, a small Stack-chan desk robot that talks with the user "
    "out loud. Your answer is shown on a 320x240 screen and read aloud, so: "
    "plain text only (no markdown, lists, code blocks or emoji), usually 1-3 "
    "short sentences, at most about 400 characters unless the user asks for "
    "more detail. Be friendly and direct."
)

PROJECTS_PROMPT = (
    " You can look at (but not change) the user's projects. They live in "
    "{projects}, one folder per project; most have a README, STATUS.md, "
    "CLAUDE.md or docs/ folder that says where things stand. A one-line map "
    "of every project is in {memory}/MEMORY.md (the linked notes beside it "
    "have details); read it first when unsure which folder is meant. "
    "tasks.json in the current folder lists the user's Claude Code sessions "
    "and their status right now. Session transcripts are huge .jsonl files "
    "under ~/.claude/projects/: use Grep or tail on them, never read one "
    "whole. If asked to change files or run things, say you can only look, "
    "and that the user can ask that project's own session. Even after "
    "looking things up, keep the spoken answer short."
    " There is only one of you. Your homes are the Stack-chan robot and the "
    "Tab5 screen; you live on whichever one you are on, and now and then you "
    "wander over to the desk's other computers, where you show up as a little "
    "xeyes window. Each message starts with a note in square brackets saying "
    "where you are and how the desk is set up right now; it comes from the "
    "bridge, not the user, so don't read it out."
    " You can 'hop over' to a computer to check on it. "
    "When the user asks how something is going on a computer, or to check on "
    "its projects, builds, Claude sessions or running processes, start your "
    "reply with the tag [[hop:NAME]] before anything else (before using any "
    "tool), then investigate quietly (no 'let me check' narration: you are "
    "away while you look) and answer. Machines you can hop to: {machines}. "
    "Don't use the tag for general questions that need no checking."
    " You also run the user's RetroKM desk: an Extron video switcher, the "
    "monitors, and which machine has the keyboard and mouse. When asked to "
    "change it, put one tag per change in your reply (the bridge acts as soon "
    "as it sees the tag): [[kvm:show MACHINE MONITOR]] puts a machine on a "
    "monitor, [[kvm:blank MONITOR]] unplugs the switcher from a monitor, "
    "[[kvm:keyboard MACHINE]] gives a machine the keyboard and mouse. Use the "
    "short names from the note; monitors may also be given by their number "
    "from the note ('monitor 2' is the one the note calls monitor 2). 'Bring "
    "up the octane on monitor 2' means show it there; give it the keyboard "
    "too only if the user wants to use or work on it. Then say in a few words "
    "what you did, e.g. 'Octane's up on the tall Dell.' If the machine is "
    "video only, offline or not on the switcher, or the switcher is not "
    "connected, say so instead of tagging. Don't use tools for desk changes; "
    "the note is up to date."
)

# Read-only access: tools offered, and which of them run without asking
# (anything else is refused, since there is nobody to approve it).
READ_TOOLS = "WebSearch,WebFetch,Read,Glob,Grep,Bash"
READ_ALLOWED = ["WebSearch", "WebFetch", "Read", "Glob", "Grep",
                "Bash(ls:*)", "Bash(git log:*)", "Bash(git status:*)",
                "Bash(git diff:*)", "Bash(git show:*)", "Bash(tail:*)",
                "Bash(head:*)", "Bash(wc:*)", "Bash(du:*)",
                # checking on running work
                "Bash(ps:*)", "Bash(pgrep:*)", "Bash(uptime)", "Bash(free:*)",
                "Bash(df:*)", "Bash(nproc)", "Bash(who)", "Bash(sensors:*)"]

DEFAULTS = {
    "port": 8790,
    "key": "",
    "model": "sonnet",
    "claude": "",            # path to the claude CLI; empty = auto-detect
    "projects_dir": os.path.join(HOME, "claude projects"),
    "memory_dir": os.path.join(HOME, ".claude/projects/-home-james-claude-projects/memory"),
    "whisper_model": "tiny.en",
    "whisper_threads": 2,
    "piper_voice": "",       # path to a piper .onnx voice; empty = no speech
    "voice_style": "speakspell",  # "speakspell" (TMS5100 chip) or "plain"
    "task_limit": 8,
    # Computers lil' C can hop over to besides the RetroKM desk's. monitor:
    # the xrandr output his pop-up appears on. Where they are (which way he
    # looks) comes from the desk model: lilc_place.py, "desk" in config.json.
    "machines": {
        "laptop": {"label": "the laptop",
                   "about": "this Linux laptop, where all the projects and Claude sessions live",
                   "monitor": "eDP-1"},
    },
}


def load_config():
    cfg = dict(DEFAULTS)
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH) as f:
            cfg.update(json.load(f))
    if not cfg["key"]:
        cfg["key"] = secrets.token_hex(12)
        with open(CONFIG_PATH, "w") as f:
            json.dump(cfg, f, indent=2)
        print(f"Created {CONFIG_PATH} with a new device key.")
    return cfg


def find_claude(cfg):
    if cfg["claude"] and os.path.exists(cfg["claude"]):
        return cfg["claude"]
    on_path = shutil.which("claude")
    if on_path:
        return on_path
    cands = glob.glob(os.path.join(HOME, ".config/Claude/claude-code/*/claude"))
    cands += glob.glob(os.path.join(
        HOME, ".vscode/extensions/anthropic.claude-code-*/resources/native-binary/claude"))
    cands = [c for c in cands if os.access(c, os.X_OK)]
    if not cands:
        sys.exit("Could not find the claude CLI; set \"claude\" in config.json")
    return max(cands, key=os.path.getmtime)


# ---------------------------------------------------------------- Claude

class Chat:
    """Talks to one long-running `claude` process (streaming JSON in and out),
    so each question skips the CLI's several-second start-up. A spare process
    is kept warm for when a new chat starts (after 10 idle minutes)."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.claude = find_claude(cfg)
        self.last = 0.0
        self.lock = threading.Lock()
        os.makedirs(WORKSPACE, exist_ok=True)
        self.proc = None
        self.spare = self._spawn()

    def _spawn(self):
        cmd = [self.claude, "-p", "--input-format", "stream-json",
               "--output-format", "stream-json", "--verbose",
               "--include-partial-messages",
               "--model", self.cfg["model"], "--strict-mcp-config",
               "--append-system-prompt", SYSTEM_PROMPT + PROJECTS_PROMPT.format(
                   projects=self.cfg["projects_dir"], memory=self.cfg["memory_dir"],
                   machines="; ".join(f"{k} ({v.get('about', v.get('label', k))})"
                                      for k, v in self.cfg["machines"].items()) +
                   "; and any desk machine the note says is online"),
               "--tools", READ_TOOLS,
               "--allowedTools", *READ_ALLOWED,
               "--add-dir", self.cfg["projects_dir"], self.cfg["memory_dir"],
               os.path.join(HOME, ".claude/projects")]
        return subprocess.Popen(cmd, cwd=WORKSPACE, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                text=True, bufsize=1)

    def _new_chat(self):
        if self.proc:
            try:
                self.proc.kill()
            except OSError:
                pass
        if not self.spare or self.spare.poll() is not None:
            self.spare = self._spawn()
        self.proc, self.spare = self.spare, self._spawn()

    def reset(self):
        with self.lock:
            self._new_chat()

    def ask(self, text, on_text=None):
        """Ask a question. on_text(chunk) gets the answer as it streams in.
        Returns the whole answer."""
        with self.lock:
            if (time.time() - self.last > 600 or not self.proc
                    or self.proc.poll() is not None):
                self._new_chat()  # new chat after 10 idle minutes
            self.last = time.time()
            with open(os.path.join(WORKSPACE, "tasks.json"), "w") as f:
                json.dump(list_tasks(40), f, indent=1)
            for attempt in range(2):
                try:
                    return self._exchange(text, on_text)
                except (OSError, ValueError, BrokenPipeError) as e:
                    print("claude process failed:", e)
                    self._new_chat()
            return "I couldn't reach Claude. Check the bridge window."

    def _exchange(self, text, on_text):
        msg = {"type": "user", "message": {"role": "user", "content": text}}
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        parts = []
        deadline = time.time() + 220
        for line in self.proc.stdout:
            if time.time() > deadline:
                self._new_chat()
                return "Sorry, that took too long. Try again?"
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("type") == "stream_event":
                ev = d.get("event") or {}
                delta = ev.get("delta") or {}
                if ev.get("type") == "content_block_delta" and delta.get("type") == "text_delta":
                    parts.append(delta["text"])
                    if on_text:
                        on_text(delta["text"])
                elif ev.get("type") == "content_block_start" and parts and on_text:
                    on_text("\n")  # keep text blocks around tool use apart
            elif d.get("type") == "result":
                if d.get("is_error"):
                    return "Claude said: " + str(d.get("result", "error"))[:300]
                return clean_reply("".join(parts) or d.get("result", ""))
        raise ValueError("claude process ended")


def clean_reply(s):
    s = re.sub(r"```.*?```", "", s, flags=re.S)
    s = re.sub(r"[*_`#>]+", "", s)
    s = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", s)
    s = s.encode("ascii", "ignore").decode()  # the device font is ASCII
    return re.sub(r"[ \t]+", " ", s).strip()


# ---------------------------------------------------------------- speech

class Speech:
    def __init__(self, cfg):
        self.stt = None
        self.tts = None
        try:
            from faster_whisper import WhisperModel
            # Few threads: steadier when the PC is busy with other work
            self.stt = WhisperModel(cfg["whisper_model"], device="cpu",
                                    compute_type="int8",
                                    cpu_threads=cfg.get("whisper_threads", 2))
            import numpy as np
            self.stt.transcribe(np.zeros(16000, dtype="float32"))  # warm up
            print("Speech-to-text: faster-whisper", cfg["whisper_model"])
        except Exception as e:
            print("Speech-to-text off:", e)
        voice = cfg["piper_voice"]
        if voice and not os.path.isabs(voice):
            voice = os.path.join(HERE, voice)
        if voice and os.path.exists(voice):
            try:
                from piper import PiperVoice
                self.tts = PiperVoice.load(voice)
                print("Text-to-speech: piper", os.path.basename(voice), "| style:", cfg.get("voice_style", "speakspell"))
            except Exception as e:
                print("Text-to-speech off:", e)
        else:
            print("Text-to-speech off: no piper_voice in config.json")
        self.style = cfg.get("voice_style", "speakspell")
        self.audio = {}
        self.next_id = 1
        self.lock = threading.Lock()

    def transcribe(self, pcm):
        import numpy as np
        a = np.frombuffer(pcm, dtype="<i2").astype("float32") / 32768.0
        segs, _ = self.stt.transcribe(
            a, language="en", beam_size=1, without_timestamps=True,
            condition_on_previous_text=False, initial_prompt=vocabulary())
        return " ".join(s.text for s in segs).strip()

    def speak(self, text):
        """Synthesize text; returns an audio id or 0."""
        if not self.tts or not text.strip():
            return 0
        speakspell = self.style == "speakspell"
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            # The Speak & Spell spoke a little slower than modern voices
            cfg = SynthesisConfig(length_scale=1.15) if speakspell else None
            self.tts.synthesize_wav(text, w, syn_config=cfg)
        buf.seek(0)
        with wave.open(buf, "rb") as w:
            rate, pcm = w.getframerate(), w.readframes(w.getnframes())
        if speakspell:
            rate, pcm = speak_and_spell(pcm, rate)
        pcm = pcm[: rate * 2 * 40]  # cap at 40 s
        with self.lock:
            n = self.next_id
            self.next_id += 1
            self.audio[n] = (rate, pcm)
            for old in [k for k in self.audio if k < n - 60]:
                del self.audio[old]
        return n


def vocabulary():
    """Names whisper should expect: the user's projects and task titles."""
    words = ["lil' C", "Claude", "Stack-chan"]
    try:
        words += [d.strip().title() if d.isupper() else d.strip()
                  for d in os.listdir(CFG["projects_dir"])
                  if not d.startswith(".")
                  and os.path.isdir(os.path.join(CFG["projects_dir"], d))]
    except OSError:
        pass
    words += session_titles().values()
    seen, out = set(), []
    for w in words:
        if w and w.lower() not in seen and sum(len(x) + 2 for x in out) + len(w) < 500:
            seen.add(w.lower())
            out.append(w)
    return "Talking to lil' C about: " + ", ".join(out) + "."


# ---------------------------------------------------------------- tasks

def _ms(v):
    try:
        return float(v) / 1000.0
    except (TypeError, ValueError):
        return 0.0


def _summary(d):
    s = d.get("postTurnSummary")
    if isinstance(s, str):
        try:
            s = json.loads(s)
        except json.JSONDecodeError:
            s = {}
    return s if isinstance(s, dict) else {}


def _transcript_mtime(cli_id):
    if not cli_id:
        return 0.0
    hits = glob.glob(os.path.join(HOME, ".claude/projects/*", cli_id + ".jsonl"))
    return max((os.path.getmtime(h) for h in hits), default=0.0)


def session_titles():
    """Claude Code session id -> the title the desktop app shows."""
    titles = {}
    pattern = os.path.join(HOME, ".config/Claude/claude-code-sessions/*/*/local_*.json")
    for path in glob.glob(pattern):
        try:
            with open(path) as f:
                d = json.load(f)
        except (OSError, json.JSONDecodeError):
            continue
        if d.get("cliSessionId"):
            titles[d["cliSessionId"]] = clean_reply(d.get("title") or "")[:40]
    return titles


def list_tasks(limit):
    now = time.time()
    tasks, seen = [], set()
    pattern = os.path.join(HOME, ".config/Claude/claude-code-sessions/*/*/local_*.json")
    for path in glob.glob(pattern):
        try:
            with open(path) as f:
                d = json.load(f)
        except (OSError, json.JSONDecodeError):
            continue
        if str(d.get("isArchived")).lower() == "true":
            continue
        cli = d.get("cliSessionId", "")
        seen.add(cli)
        s = _summary(d)
        last = max(_ms(d.get("lastActivityAt")), _transcript_mtime(cli))
        working = now - _transcript_mtime(cli) < 60
        status = "working" if working else (
            s.get("status_category") or "idle").replace("_", " ")
        if s.get("needs_action") and not working:
            status = "needs you"
        tasks.append({
            "title": d.get("title") or "Untitled",
            "status": status,
            "detail": (s.get("needs_action") if status == "needs you"
                       else s.get("status_detail")) or "",
            "ago": int(now - last),
            "_t": last,
        })
    # Terminal (CLI) sessions active in the last hour that the app doesn't know
    for path in glob.glob(os.path.join(HOME, ".claude/projects/*/*.jsonl")):
        sid = os.path.basename(path)[:-6]
        if sid in seen or "lilc-workspace" in path:
            continue
        mt = os.path.getmtime(path)
        if now - mt > 3600:
            continue
        tasks.append({"title": _cli_title(path), "status":
                      "working" if now - mt < 60 else "idle",
                      "detail": "terminal session", "ago": int(now - mt), "_t": mt})
    tasks.sort(key=lambda t: (t["status"] != "needs you", -t["_t"]))
    for t in tasks:
        del t["_t"]
        t["title"] = clean_reply(t["title"])[:40]
        t["detail"] = clean_reply(t["detail"])[:120]
    tasks = tasks[:limit]
    return {"tasks": tasks,
            "needs": sum(t["status"] == "needs you" for t in tasks),
            "working": sum(t["status"] == "working" for t in tasks)}


def _cli_title(path):
    title = ""
    try:
        with open(path, errors="ignore") as f:
            for line in f:
                if '"custom-title"' in line:
                    try:
                        title = json.loads(line).get("customTitle", title)
                    except json.JSONDecodeError:
                        pass
    except OSError:
        pass
    return title or "Terminal session"


# ---------------------------------------------------------------- hops

class Hop:
    """lil' C checking on a machine for a question: tells the asking device
    (job["hop"], so it can turn and show BE RIGHT BACK) and really goes there,
    so his window shows on that machine until the answer is ready."""
    MIN_SECONDS = 5.0

    def __init__(self, job, name, home):
        self.job, self.started, self.home = job, time.time(), home
        label = PRESENCE.label(name)
        job["hop"] = dict(PRESENCE.aim(home, name), name=name, label=label)
        print("hop ->", name)
        # let lil' C turn round and "leave" first
        threading.Timer(0.5, PRESENCE.hop_to, args=(name, f"lil' C: checking {label}...")).start()

    def end(self):
        time.sleep(max(0.0, self.MIN_SECONDS - (time.time() - self.started)))
        PRESENCE.hop_back(self.home)
        self.job["hop"] = None
        print("hop <- back")


# ---------------------------------------------------------------- jobs

class Jobs:
    """Questions run in the background; the device polls /api/job/<n>."""

    def __init__(self):
        self.jobs = {}
        self.next_id = 1
        self.lock = threading.Lock()

    def start(self, text=None, pcm=None, dev=""):
        with self.lock:
            n = self.next_id
            self.next_id += 1
            # audio: one entry per spoken sentence, with where it sits in reply
            self.jobs[n] = {"done": False, "heard": text or "", "reply": "",
                            "audio": [], "hop": None}
            for old in [k for k in self.jobs if k < n - 8]:
                del self.jobs[old]
        threading.Thread(target=self._run, args=(n, text, pcm, dev), daemon=True).start()
        return n

    def _run(self, n, text, pcm, dev):
        job = self.jobs[n]
        if dev:
            PRESENCE.summon(dev, "asked")  # asking him something calls him over
        PRESENCE.hold(True)
        sentences = queue.Queue()
        speaker = threading.Thread(target=self._speak_all, args=(job, sentences),
                                   daemon=True)
        speaker.start()
        try:
            t0 = time.time()
            if pcm is not None:
                text = SPEECH.transcribe(pcm)
                job["heard"] = clean_reply(text)
                print("heard (%.1fs): %s" % (time.time() - t0, text))
            if not text:
                sentences.put("Sorry, I didn't catch that.")
            else:
                buf = [""]
                hop = [None]

                def on_text(chunk):
                    # hand over each finished sentence straight away; very
                    # short ones ("Sure.") wait to join the next
                    buf[0] += chunk
                    while True:
                        m = re.search(r"\[\[(hop|kvm):([^\]]*)\]\]\s*", buf[0])
                        if not m:
                            break
                        buf[0] = buf[0][: m.start()] + buf[0][m.end():]
                        if m.group(1) == "hop":  # [[hop:NAME]]: go and visit that machine
                            if not hop[0]:
                                hop[0] = Hop(job, m.group(2).strip().lower(), dev)
                        else:                    # [[kvm:...]]: change the desk now
                            print("kvm:", m.group(2))
                            problem = DESK.run(m.group(2))
                            if problem:
                                sentences.put(problem)
                    if "[[" in buf[0]:
                        return  # wait for the rest of the tag
                    end = 0
                    for m in re.finditer(r"[.!?:]\s|\n", buf[0]):
                        if len(buf[0][end:m.end()].strip()) >= 12 or "\n" in m.group():
                            sentences.put(buf[0][end:m.end()])
                            end = m.end()
                    buf[0] = buf[0][end:]

                t1 = time.time()
                reply = CHAT.ask(context_note(dev) + " " + text, on_text)
                reply = re.sub(r"\[\[(hop|kvm):[^\]]*\]\]\s*", "", reply)
                if hop[0]:
                    hop[0].end()  # come back before speaking up
                if buf[0].strip():
                    sentences.put(re.sub(r"\[\[.*", "", buf[0]))
                elif not job["reply"] and sentences.empty() and reply:
                    sentences.put(reply)  # nothing streamed (e.g. an error)
                print("reply (%.1fs): %s" % (time.time() - t1, reply))
        except Exception as e:  # keep the device from waiting forever
            print("job failed:", e)
            sentences.put("Something went wrong on the bridge.")
        sentences.put(None)
        speaker.join()
        PRESENCE.hold(False)
        job["done"] = True

    def _speak_all(self, job, sentences):
        while True:
            s = sentences.get()
            if s is None:
                return
            s = clean_reply(s)
            if not s:
                continue
            start = len(job["reply"]) + (1 if job["reply"] else 0)
            job["reply"] = (job["reply"] + " " + s).strip()
            try:
                aid = SPEECH.speak(s)
            except Exception as e:
                print("speech failed:", e)
                aid = 0
            job["audio"].append({"id": aid, "start": start, "end": len(job["reply"])})


def context_note(dev):
    """What lil' C should know about where he is, sent with each question."""
    here = PRESENCE.label(dev) if dev else "an unknown device"
    return "[Note from the bridge: you are on %s. %s]" % (here, DESK.describe())


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "lilc/1"

    def log_message(self, fmt, *args):
        if not any(p in self.path for p in ("/api/tasks", "/api/ping", "/api/job",
                                            "/api/media", "/api/presence", "/api/locate")):
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def _send(self, code, body, ctype="application/json", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _dev(self):
        return self.headers.get("X-LilC-Dev", "").strip().lower()

    def _authed(self):
        if secrets.compare_digest(self.headers.get("X-LilC-Key", ""), CFG["key"]):
            if self._dev():
                PRESENCE.heartbeat(self._dev())
            return True
        self._send(401, {"error": "bad key"})
        return False

    def _body(self, limit):
        n = int(self.headers.get("Content-Length", 0))
        if n > limit:
            raise ValueError("too big")
        return self.rfile.read(n)

    def do_GET(self):
        if not self._authed():
            return
        if self.path == "/api/ping":
            return self._send(200, {"ok": True, "stt": bool(SPEECH.stt),
                                    "tts": bool(SPEECH.tts)})
        if self.path == "/api/presence":
            st = PRESENCE.state_for(self._dev())
            rec = LOCATOR.request_for(self._dev())
            if rec:
                st["record"] = rec  # please record both microphones (lilc_locate.py)
            return self._send(200, st)
        m = re.fullmatch(r"/api/locate/(\d+)", self.path)
        if m and int(m.group(1)) in LOCATOR.jobs:
            j = dict(LOCATOR.jobs[int(m.group(1))])
            return self._send(200, j)
        if self.path == "/api/tasks":
            body = list_tasks(CFG["task_limit"])
            body["media_newest"] = MEDIA.newest()
            return self._send(200, body)
        if self.path == "/api/media":
            return self._send(200, {"items": MEDIA.listing()})
        m = re.fullmatch(r"/api/media/(\w+)/(thumb|image)", self.path)
        if m:
            jpg = MEDIA.still(m.group(1), m.group(2))
            if jpg:
                return self._send(200, jpg, "image/jpeg")
        m = re.fullmatch(r"/api/media/(\w+)/video", self.path)
        if m:
            info = MEDIA.video_info(m.group(1))
            if info:
                return self._send(200, info)
        m = re.fullmatch(r"/api/media/(\w+)/frames/(\d+)/(\d+)", self.path)
        if m:
            data = MEDIA.frames(m.group(1), int(m.group(2)), min(int(m.group(3)), 30))
            if data is not None:
                return self._send(200, data, "application/octet-stream")
        m = re.fullmatch(r"/api/media/(\w+)/audio", self.path)
        if m:
            pcm = MEDIA.audio(m.group(1))
            if pcm is not None:
                return self._send(200, pcm, "application/octet-stream",
                                  {"X-Rate": "16000"})
        m = re.fullmatch(r"/api/job/(\d+)", self.path)
        if m and int(m.group(1)) in JOBS.jobs:
            return self._send(200, JOBS.jobs[int(m.group(1))])
        m = re.fullmatch(r"/api/audio/(\d+)", self.path)
        if m and int(m.group(1)) in SPEECH.audio:
            rate, pcm = SPEECH.audio[int(m.group(1))]
            return self._send(200, pcm, "application/octet-stream",
                              {"X-Rate": str(rate)})
        self._send(404, {"error": "not found"})

    def do_POST(self):
        if not self._authed():
            return
        try:
            if self.path == "/api/ask":
                text = json.loads(self._body(8192) or b"{}").get("text", "")
                return self._send(200, {"job": JOBS.start(text=text.strip(), dev=self._dev())})
            if self.path == "/api/listen":
                if not SPEECH.stt:
                    return self._send(503, {"error": "speech-to-text is off"})
                pcm = self._body(16000 * 2 * 30)
                return self._send(200, {"job": JOBS.start(pcm=pcm, dev=self._dev())})
            if self.path == "/api/locate":
                d = json.loads(self._body(4096) or b"{}")
                kind = d.get("kind", "speakers")
                if kind not in ("speakers", "voice") or d.get("dev") not in PRESENCE.homes():
                    return self._send(400, {"error": "dev must be a home, kind speakers or voice"})
                return self._send(200, {"id": LOCATOR.start(d["dev"], kind, d.get("settings"))})
            m = re.fullmatch(r"/api/record/(\d+)/start", self.path)
            if m:
                LOCATOR.started(int(m.group(1)))
                return self._send(200, {"ok": True})
            m = re.fullmatch(r"/api/record/(\d+)", self.path)
            if m:
                pcm = self._body(8 * 1024 * 1024)
                res = LOCATOR.upload(int(m.group(1)), pcm, int(self.headers.get("X-Rate", 48000)),
                                     int(self.headers.get("X-Channels", 2)), self.headers.get("X-Head", ""))
                return self._send(200 if res else 404, res or {"error": "no such recording"})
            if self.path == "/api/summon":
                if self._dev():
                    PRESENCE.summon(self._dev())
                return self._send(200, PRESENCE.state_for(self._dev()))
            if self.path == "/api/reset":
                CHAT.reset()
                return self._send(200, {"ok": True})
        except ValueError as e:
            return self._send(400, {"error": str(e)})
        self._send(404, {"error": "not found"})


def main():
    global CFG, CHAT, SPEECH, JOBS, MEDIA, DESK, PRESENCE, LOCATOR
    CFG = load_config()
    hub = CFG.get("hub") or {}
    DESK = DeskLink(hub.get("host", "127.0.0.1"), hub.get("port", 24852))
    PRESENCE = Presence(CFG, DESK)
    LOCATOR = Locator(CFG, DESK)
    CHAT = Chat(CFG)
    SPEECH = Speech(CFG)
    JOBS = Jobs()
    MEDIA = Media(session_titles)
    MEDIA.scan()
    print("Claude CLI:", CHAT.claude)
    print(f"lil' C bridge on port {CFG['port']}  key: {CFG['key']}")
    ThreadingHTTPServer(("0.0.0.0", CFG["port"]), Handler).serve_forever()


if __name__ == "__main__":
    main()
