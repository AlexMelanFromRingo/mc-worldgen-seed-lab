#!/usr/bin/env python3
"""Клиент oracle serve (реальный код Mojang): один процесс JVM на версию, команды построчно, ответ — 1 JSON-строка."""
import json
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class Oracle:
    def __init__(self, version):
        self.version = version
        self.p = subprocess.Popen([f'{ROOT}/oracle/run.sh', version, 'serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def call(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        while True:
            out = self.p.stdout.readline()
            if not out:
                raise RuntimeError('oracle закрыл stdout')
            out = out.strip()
            if out.startswith('{'):
                r = json.loads(out)
                if not r.get('ok', False):
                    raise RuntimeError(f'oracle: {r}')
                return r

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()
