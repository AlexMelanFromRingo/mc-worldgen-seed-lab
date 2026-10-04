"""Фоновые задачи без bpy: поток + прогресс + отмена. Операторы Blender опрашивают задачу таймером (modal), тесты — циклом."""
import threading
import time
import traceback


class Cancelled(Exception):
    """Задача отменена пользователем (Esc / кнопка)."""


class Task:
    """Запускает fn(task, *args, **kw) в потоке. fn вызывает task.report(доля, текст) и task.check() (бросает Cancelled при отмене)."""

    def __init__(self, title, fn, *args, **kw):
        self.title = title
        self._fn, self._args, self._kw = fn, args, kw
        self._lock = threading.Lock()
        self._cancel = threading.Event()
        self._fraction = 0.0
        self._message = ''
        self.state = 'new'          # new -> running -> done | error | cancelled
        self.result = None
        self.error = None           # исключение (если state == 'error')
        self.error_text = ''
        self.t_start = self.t_end = 0.0
        self.log = []
        self._thread = None

    def start(self):
        self.state = 'running'
        self.t_start = time.perf_counter()
        self._thread = threading.Thread(target=self._run, name=f'mcgen-{self.title}', daemon=True)
        self._thread.start()
        return self

    def _run(self):
        try:
            self.result = self._fn(self, *self._args, **self._kw)
            self.state = 'cancelled' if self._cancel.is_set() and self.result is None else 'done'
        except Cancelled:
            self.state = 'cancelled'
        except BaseException as e:      # noqa: BLE001 - исключение уходит в интерфейс, поток не должен падать молча
            if type(e).__name__ == 'McCancelled':
                self.state = 'cancelled'
            else:
                self.error = e
                self.error_text = ''.join(traceback.format_exception_only(type(e), e)).strip()
                self.log.append(traceback.format_exc())
                self.state = 'error'
        finally:
            self.t_end = time.perf_counter()

    # --- вызывается из рабочего потока ---
    def report(self, fraction=None, message=None):
        with self._lock:
            if fraction is not None:
                self._fraction = max(0.0, min(1.0, float(fraction)))
            if message is not None:
                self._message = message

    def check(self):
        if self._cancel.is_set():
            raise Cancelled()

    def should_cancel(self):
        return self._cancel.is_set()

    @property
    def cancel_requested(self):
        return self._cancel.is_set()

    # --- вызывается из главного потока ---
    @property
    def fraction(self):
        with self._lock:
            return self._fraction

    @property
    def message(self):
        with self._lock:
            return self._message

    def cancel(self):
        self._cancel.set()

    @property
    def finished(self):
        return self.state in ('done', 'error', 'cancelled')

    @property
    def elapsed(self):
        return (self.t_end or time.perf_counter()) - self.t_start if self.t_start else 0.0

    def join(self, timeout=None):
        if self._thread:
            self._thread.join(timeout)
        return self.finished

    def run_blocking(self, poll=0.02, on_tick=None):
        """Для скриптов/тестов: запустить и дождаться, вызывая on_tick(task)."""
        if self.state == 'new':
            self.start()
        while not self._thread_done():
            if on_tick:
                on_tick(self)
            time.sleep(poll)
        return self

    def _thread_done(self):
        return self._thread is not None and not self._thread.is_alive()
