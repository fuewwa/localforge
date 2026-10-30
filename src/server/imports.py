import threading


def run(store, name, url):
    try:
        store.fetch(name, url)
        store.set_status(name, "ready", "")
    except Exception as exc:
        try:
            store.set_status(name, "failed", str(exc)[:500])
        except Exception:
            pass


def start(store, name, url):
    worker = threading.Thread(target=run, args=(store, name, url), daemon=True)
    worker.start()
