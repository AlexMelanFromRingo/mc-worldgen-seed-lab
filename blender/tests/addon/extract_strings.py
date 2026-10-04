#!/usr/bin/env python3
"""Собирает все пользовательские строки интерфейса аддона (AST): name/description/text, bl_label/bl_description, iface_()/rpt_(),
элементы enum, report(). Используется тестом покрытия перевода и для подготовки словаря ui/translations.py."""
import ast
import os
import sys

ADDON = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'mcgen_addon')
FILES = ['ui/props.py', 'ui/ops.py', 'ui/panels.py', 'render/edit_ops.py', 'ui/presets.py', 'ui/__init__.py', '__init__.py', 'core/pack.py', 'core/gpu_build.py', 'core/lib.py', 'core/jobs.py', 'core/mock.py',
         'core/backend.py', 'core/catalog.py']
KW = {'name', 'description', 'text', 'heading'}
ATTR = {'bl_label', 'bl_description', 'status_title'}
FUNCS = {'iface_', 'rpt_', 'rpt', 'pgettext_iface', 'pgettext_rpt'}
FIRST_ARG = {'PackError', 'NeedJava', 'EulaNotAccepted'}          # исключения ядра с переводимым шаблоном первым аргументом
SECOND_ARG = {'McError', 'McCancelled', 'rep', 'report_progress'}  # шаблон вторым аргументом (McError(code, msg), rep(frac, msg))


def _const(n):
    return n.value if isinstance(n, ast.Constant) and isinstance(n.value, str) else None


def extract(path):
    tree = ast.parse(open(path, encoding='utf-8').read())
    out = {}

    def add(s, where):
        if s and any(ch.isalpha() for ch in s):
            out.setdefault(s, where)

    class V(ast.NodeVisitor):
        def visit_Call(self, node):
            fn = node.func
            fname = fn.id if isinstance(fn, ast.Name) else (fn.attr if isinstance(fn, ast.Attribute) else '')
            if fname in FUNCS and node.args:
                add(_const(node.args[0]), node.lineno)
            if fname == 'report' and len(node.args) >= 2:
                add(_const(node.args[1]), node.lineno)
            if fname in FIRST_ARG and node.args:
                add(_const(node.args[0]), node.lineno)
            if fname in SECOND_ARG and len(node.args) >= 2:
                add(_const(node.args[1]), node.lineno)
            for k in node.keywords:
                if k.arg in KW:
                    add(_const(k.value), node.lineno)
                if k.arg == 'items' and isinstance(k.value, (ast.List, ast.Tuple)):
                    for el in k.value.elts:
                        if isinstance(el, ast.Tuple) and len(el.elts) >= 3:
                            add(_const(el.elts[1]), node.lineno)
                            add(_const(el.elts[2]), node.lineno)
            self.generic_visit(node)

        def visit_Assign(self, node):
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id in ATTR:
                    add(_const(node.value), node.lineno)
                if isinstance(t, ast.Name) and t.id in ('_NEED_JAVA', '_note'):
                    add(_const(node.value), node.lineno)
                if isinstance(t, ast.Tuple) and isinstance(node.value, ast.Tuple) and len(t.elts) == len(node.value.elts):      # a, b = 'x', 'y'
                    for te, ve in zip(t.elts, node.value.elts):
                        if isinstance(te, ast.Attribute) and te.attr in ('message', 'error_text'):
                            add(_const(ve), node.lineno)
                if isinstance(t, ast.Attribute) and t.attr == 'message':
                    add(_const(node.value), node.lineno)
            self.generic_visit(node)

        def visit_ClassDef(self, node):
            for st in node.body:
                if isinstance(st, ast.Assign) and any(isinstance(t, ast.Name) and t.id in ATTR for t in st.targets):
                    add(_const(st.value), st.lineno)
            self.generic_visit(node)

        def visit_List(self, node):                      # SUBPANELS = [('world', 'Version & World', …), …]
            for el in node.elts:
                if isinstance(el, ast.Tuple) and len(el.elts) >= 3 and _const(el.elts[0]) and _const(el.elts[1]) and isinstance(el.elts[2], ast.Name):
                    add(_const(el.elts[1]), node.lineno)
            self.generic_visit(node)

    V().visit(tree)
    return out


def all_strings():
    res = {}
    for f in FILES:
        p = os.path.join(ADDON, f)
        if os.path.exists(p):
            for s, ln in extract(p).items():
                res.setdefault(s, f'{f}:{ln}')
    return res


if __name__ == '__main__':
    for s, w in sorted(all_strings().items(), key=lambda kv: kv[1]):
        print(f'{w}\t{s}')
