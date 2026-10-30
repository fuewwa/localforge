import zlib

from flask import Blueprint, Response, abort, current_app, request

bp = Blueprint("smart", __name__)

NO_CACHE = {
    "Cache-Control": "no-cache, max-age=0, must-revalidate",
    "Pragma": "no-cache",
    "Expires": "Fri, 01 Jan 1980 00:00:00 GMT",
}


def store():
    return current_app.config["STORE"]


def payload():
    data = request.get_data()
    if request.headers.get("Content-Encoding", "").lower() in ("gzip", "x-gzip"):
        data = zlib.decompress(data, 31)
    return data


@bp.route("/<name>.git/info/refs")
@bp.route("/<name>/info/refs")
def info_refs(name):
    service = request.args.get("service", "")
    if service not in ("git-upload-pack", "git-receive-pack"):
        abort(403)
    body = store().advertise(name, service[4:])
    return Response(body, mimetype=f"application/x-{service}-advertisement", headers=NO_CACHE)


@bp.route("/<name>.git/git-upload-pack", methods=["POST"])
@bp.route("/<name>/git-upload-pack", methods=["POST"])
def upload_pack(name):
    body = store().upload_pack(name, payload())
    return Response(body, mimetype="application/x-git-upload-pack-result", headers=NO_CACHE)


@bp.route("/<name>.git/git-receive-pack", methods=["POST"])
@bp.route("/<name>/git-receive-pack", methods=["POST"])
def receive_pack(name):
    body = store().receive_pack(name, payload())
    return Response(body, mimetype="application/x-git-receive-pack-result", headers=NO_CACHE)
