#!/usr/bin/env python3
"""Клиент эталона (oracle serve: реальный код Mojang). Одна JVM на версию, команды построчно, ответ — JSON-строка."""
import json, os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


class Oracle:
    def __init__(self, version):
        self.version = version
        self.p = subprocess.Popen([os.path.join(ROOT, "oracle", "run.sh"), version, "serve"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  text=True, bufsize=1)
        r = self.cmd("info")
        assert r.get("ok") and r.get("version") == version, r

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        while True:
            out = self.p.stdout.readline()
            if not out:
                raise RuntimeError("oracle завершился")
            out = out.strip()
            if out.startswith("{"):
                r = json.loads(out)
                if not r.get("ok", False):
                    raise RuntimeError("oracle: %s" % r)
                return r

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()
