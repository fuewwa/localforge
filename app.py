import argparse
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "src"))


def main():
    parser = argparse.ArgumentParser(description="Local git hosting backed by a single SQLite file")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--data", default=os.path.join(ROOT, "data"))
    parser.add_argument("--max-body-mb", type=int, default=512)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    try:
        from server import factory
    except ImportError as exc:
        sys.exit(
            f"Cannot start: {exc}\n"
            "Build the core first: cmake -S src/core -B build && cmake --build build"
        )
    app = factory.create(args.data, args.max_body_mb * 1024 * 1024)
    app.run(host=args.host, port=args.port, threaded=True, debug=args.debug)


if __name__ == "__main__":
    main()
