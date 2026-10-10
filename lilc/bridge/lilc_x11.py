"""X11 helpers for the laptop the bridge runs on."""
import os
import re
import subprocess

HOME = os.path.expanduser("~")


def x11_env():
    env = dict(os.environ)
    env.setdefault("DISPLAY", ":0")
    env.setdefault("XAUTHORITY", os.path.join(HOME, ".Xauthority"))
    return env


def _xrandr():
    try:
        return subprocess.run(["xrandr", "--query"], capture_output=True, text=True,
                              timeout=5, env=x11_env()).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


_OUTPUT = r"^(\S+) connected (?:primary )?(\d+)x(\d+)\+(\d+)\+(\d+)"


def monitor_geometry(name):
    """(x, y, w, h) of an xrandr output, or the first connected one."""
    first = None
    for m in re.finditer(_OUTPUT, _xrandr(), re.M):
        g = (int(m.group(4)), int(m.group(5)), int(m.group(2)), int(m.group(3)))
        if m.group(1) == name:
            return g
        first = first or g
    return first or (0, 0, 1920, 1080)


def all_monitors():
    """Every connected monitor as "x,y,w,h;x,y,w,h" for the pop-up."""
    return ";".join(f"{m[3]},{m[4]},{m[1]},{m[2]}" for m in re.findall(_OUTPUT, _xrandr(), re.M))
