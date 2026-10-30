import os
import re
from urllib.parse import quote, urlparse

import engine
from flask import (
    Blueprint,
    Response,
    abort,
    current_app,
    jsonify,
    redirect,
    render_template,
    request,
    url_for,
)

from . import imports, util

bp = Blueprint("pages", __name__)

PER_PAGE = 30
BLOB_LIMIT = 1024 * 1024
PATCH_LIMIT = 512 * 1024
IMAGES = {
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".bmp": "image/bmp",
    ".ico": "image/x-icon",
}
SOURCE = re.compile(r"^(https?://|git://|file://|/|[A-Za-z]:[\\/])")
HEX = re.compile(r"^[0-9a-fA-F]{40}$")
NAME_HELP = (
    "Use 1 to 64 letters, digits, dashes or underscores, starting with a letter or digit. "
    "The names new, import, api and static are reserved."
)


def store():
    return current_app.config["STORE"]


@bp.before_request
def same_origin():
    if request.method == "POST":
        origin = request.headers.get("Origin")
        if origin and urlparse(origin).netloc != request.host:
            abort(403)


def context(info, tab, **extra):
    base = request.host_url.rstrip("/")
    data = {
        "info": info,
        "tab": tab,
        "clone_url": f"{base}/{info.name}.git",
        "refs": None,
        "ref": None,
        "rq": {},
    }
    data.update(extra)
    return data


def attach_refs(info, data):
    if info.status != "ready" or info.empty:
        return False
    refs = store().refs(info.name)
    requested = request.args.get("ref", "")
    if requested:
        if requested not in refs.branches and requested not in refs.tags and not HEX.match(requested):
            abort(404)
        ref = requested
    elif info.branch in refs.branches:
        ref = info.branch
    elif refs.branches:
        ref = refs.branches[0]
    else:
        return False
    data["refs"] = refs
    data["ref"] = ref
    data["rq"] = {"ref": ref} if ref != info.branch else {}
    return True


def crumbs(path):
    out = []
    parts = [p for p in path.split("/") if p]
    for i, part in enumerate(parts):
        out.append((part, "/".join(parts[: i + 1])))
    return out


def index_page(error=None, status=200):
    return render_template("index.html", repos=store().list(), error=error), status


@bp.route("/")
def index():
    return index_page()


@bp.route("/new", methods=["POST"])
def new():
    name = request.form.get("name", "").strip()
    description = request.form.get("description", "").strip()[:500]
    branch = request.form.get("branch", "").strip() or "main"
    if not util.valid_name(name):
        return index_page(NAME_HELP, 400)
    try:
        store().create(name, description, branch, "ready")
    except (engine.Exists, engine.Invalid) as exc:
        return index_page(str(exc), 400)
    return redirect(url_for("pages.repo", name=name))


@bp.route("/import", methods=["POST"])
def import_repo():
    name = request.form.get("name", "").strip()
    source = request.form.get("source", "").strip()
    description = request.form.get("description", "").strip()[:500]
    if not util.valid_name(name):
        return index_page(NAME_HELP, 400)
    if not SOURCE.match(source):
        return index_page("The source must be an http(s) or git URL, or an absolute path on this machine.", 400)
    try:
        store().create(name, description, "main", "importing")
    except (engine.Exists, engine.Invalid) as exc:
        return index_page(str(exc), 400)
    imports.start(store(), name, source)
    return redirect(url_for("pages.repo", name=name))


@bp.route("/api/status/<name>")
def status(name):
    info = store().info(name)
    return jsonify(status=info.status, error=info.error)


@bp.route("/<name>")
def repo(name):
    store().info(name)
    return redirect(url_for("pages.preview", name=name))


@bp.route("/<name>/preview", defaults={"path": ""})
@bp.route("/<name>/preview/<path:path>")
def preview(name, path):
    info = store().info(name)
    data = context(info, "preview", path=path.strip("/"), crumbs=crumbs(path), state="ready")
    if info.status != "ready":
        data["state"] = info.status
        return render_template("preview.html", **data)
    if not attach_refs(info, data):
        data["state"] = "empty"
        return render_template("preview.html", **data)
    ref = data["ref"]
    kind = store().kind(name, ref, path)
    data["kind"] = kind
    if kind == "tree":
        data["entries"] = store().tree(name, ref, path)
        data["readme"] = None
        if not path.strip("/"):
            for entry in data["entries"]:
                if entry.kind == "blob" and entry.name.lower().startswith("readme") and entry.size <= 262144:
                    file = store().blob(name, ref, entry.name, BLOB_LIMIT)
                    if not file.binary:
                        data["readme"] = (entry.name, file.data.decode("utf-8", errors="replace"))
                    break
    elif kind == "blob":
        file = store().blob(name, ref, path, BLOB_LIMIT)
        ext = os.path.splitext(path)[1].lower()
        data["file"] = file
        data["image"] = ext in IMAGES and file.binary
        data["text"] = "" if file.binary else file.data.decode("utf-8", errors="replace")
        data["line_count"] = data["text"].count("\n") + (0 if data["text"].endswith("\n") or not data["text"] else 1)
    return render_template("preview.html", **data)


@bp.route("/<name>/raw/<path:path>")
def raw(name, path):
    info = store().info(name)
    data = context(info, "preview")
    if not attach_refs(info, data):
        abort(404)
    file = store().blob(name, data["ref"], path, -1)
    ext = os.path.splitext(path)[1].lower()
    headers = {
        "X-Content-Type-Options": "nosniff",
        "Content-Security-Policy": "sandbox; default-src 'none'; img-src data:; style-src 'unsafe-inline'",
    }
    if ext in IMAGES and file.binary:
        mimetype = IMAGES[ext]
    elif not file.binary:
        mimetype = "text/plain; charset=utf-8"
    else:
        mimetype = "application/octet-stream"
        filename = quote(os.path.basename(path))
        headers["Content-Disposition"] = f"attachment; filename*=UTF-8''{filename}"
    return Response(file.data, mimetype=mimetype, headers=headers)


@bp.route("/<name>/logs")
def logs(name):
    info = store().info(name)
    data = context(info, "logs", state="ready")
    if info.status != "ready":
        data["state"] = info.status
        return render_template("logs.html", **data)
    if not attach_refs(info, data):
        data["state"] = "empty"
        return render_template("logs.html", **data)
    try:
        page = max(1, int(request.args.get("page", "1")))
    except ValueError:
        page = 1
    commits = store().log(name, data["ref"], (page - 1) * PER_PAGE, PER_PAGE + 1)
    data["more"] = len(commits) > PER_PAGE
    data["commits"] = commits[:PER_PAGE]
    data["page"] = page
    return render_template("logs.html", **data)


@bp.route("/<name>/commit/<oid>")
def commit(name, oid):
    info = store().info(name)
    detail = store().commit(name, oid, PATCH_LIMIT)
    data = context(info, "logs", detail=detail, lines=util.diff_lines(detail.patch))
    return render_template("commit.html", **data)


@bp.route("/<name>/settings", methods=["GET", "POST"])
def settings(name):
    info = store().info(name)
    if request.method == "POST":
        action = request.form.get("action", "")
        if action == "describe":
            store().set_description(name, request.form.get("description", "").strip()[:500])
        elif action == "branch":
            store().set_branch(name, request.form.get("branch", ""))
        elif action == "delete":
            if request.form.get("confirm", "") != name:
                data = context(info, "settings", error="Type the repository name to confirm deletion.")
                data["branches"] = []
                return render_template("settings.html", **data), 400
            store().remove(name)
            return redirect(url_for("pages.index"))
        return redirect(url_for("pages.settings", name=name))
    data = context(info, "settings")
    data["branches"] = store().refs(name).branches if info.status == "ready" and not info.empty else []
    return render_template("settings.html", **data)
