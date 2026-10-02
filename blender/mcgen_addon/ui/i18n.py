"""Перевод динамических сообщений (report(), статус-бар, ошибки ядра): шаблоны английские, параметры подставляются после перевода."""
import re

from bpy.app.translations import pgettext_rpt


def rpt(template, **kw):
    t = pgettext_rpt(template)
    return t.format(**kw) if kw else t


def exc_text(e):
    """Текст исключения для пользователя: PackError/McError несут template+kw -> переводим шаблон; прочие исключения как есть."""
    tpl = getattr(e, 'template', None)
    if tpl:
        kw = getattr(e, 'kw', {}) or {}
        t = pgettext_rpt(tpl)
        where = getattr(e, 'where', '')
        txt = t.format(**kw) if kw else t
        if hasattr(e, 'code'):
            from ..core.lib import ERROR_NAMES
            return (f'{where}: ' if where else '') + f'{txt} ({ERROR_NAMES.get(e.code, e.code)})'
        return txt
    return f'{type(e).__name__}: {e}' if not str(e) or type(e).__name__ not in ('PackError', 'NeedJava', 'EulaNotAccepted') else str(e)


_PATTERNS = [(re.compile(r'^Building scene (\d+)%$'), 'Building scene {p}%')]


def progress_text(msg):
    """Перевод текста прогресса: точные совпадения и шаблон «Building scene N%»."""
    for rx, tpl in _PATTERNS:
        m = rx.match(msg)
        if m:
            return rpt(tpl, p=m.group(1))
    return pgettext_rpt(msg)
