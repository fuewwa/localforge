import re
import time
from datetime import datetime, timedelta, timezone

NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
RESERVED = {"static", "new", "import", "api"}


def valid_name(name):
    return bool(NAME.match(name)) and name.lower() not in RESERVED


def size(value):
    value = float(value)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return f"{int(value)} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024


def ago(ts):
    delta = max(0, int(time.time()) - int(ts))
    if delta < 60:
        return "just now"
    for seconds, unit in ((31536000, "year"), (2592000, "month"), (86400, "day"), (3600, "hour"), (60, "minute")):
        if delta >= seconds:
            count = delta // seconds
            return f"{count} {unit}{'s' if count != 1 else ''} ago"
    return "just now"


def stamp(ts, offset=0):
    zone = timezone(timedelta(minutes=offset))
    return datetime.fromtimestamp(ts, zone).strftime("%Y-%m-%d %H:%M %z")


def diff_lines(patch):
    out = []
    in_hunk = False
    for line in patch.split("\n"):
        if line.startswith("diff --git"):
            in_hunk = False
            kind = "file"
        elif line.startswith("@@"):
            in_hunk = True
            kind = "hunk"
        elif not in_hunk:
            kind = "meta"
        elif line.startswith("+"):
            kind = "add"
        elif line.startswith("-"):
            kind = "del"
        else:
            kind = "ctx"
        out.append((kind, line))
    if out and out[-1][1] == "":
        out.pop()
    return out
