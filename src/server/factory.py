import os

import engine
from flask import Flask, Response, render_template, request

from . import pages, smart, util

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fail(code, title, message):
    if request.blueprint == "smart":
        return Response(message + "\n", status=code, mimetype="text/plain")
    return render_template("error.html", code=code, title=title, message=message), code


def create(data_dir, max_body):
    os.makedirs(data_dir, exist_ok=True)
    app = Flask(
        __name__,
        template_folder=os.path.join(ROOT, "templates"),
        static_folder=os.path.join(ROOT, "static"),
    )
    app.config["MAX_CONTENT_LENGTH"] = max_body
    app.config["STORE"] = engine.Store(os.path.join(data_dir, "forge.db"))
    app.jinja_env.filters["size"] = util.size
    app.jinja_env.filters["ago"] = util.ago
    app.jinja_env.filters["stamp"] = util.stamp
    app.register_blueprint(smart.bp)
    app.register_blueprint(pages.bp)

    @app.errorhandler(engine.NotFound)
    def not_found(exc):
        return fail(404, "Not found", str(exc))

    @app.errorhandler(404)
    def missing(exc):
        return fail(404, "Not found", "This page does not exist.")

    @app.errorhandler(403)
    def forbidden(exc):
        return fail(403, "Forbidden", "This request was refused.")

    @app.errorhandler(413)
    def too_large(exc):
        return fail(413, "Too large", "The request is larger than the configured limit.")

    @app.errorhandler(engine.Exists)
    def exists(exc):
        return fail(409, "Conflict", str(exc))

    @app.errorhandler(engine.Invalid)
    def invalid(exc):
        return fail(400, "Bad request", str(exc))

    @app.errorhandler(engine.Error)
    def failure(exc):
        return fail(500, "Error", str(exc))

    return app
