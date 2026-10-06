#!/usr/bin/env python3
"""Запись расписания генерации настоящего сервера Minecraft и перевод его в файл .mcsched для libmcgen / аддона Blender.

Игра генерирует декорации в порядке, который зависит от гонки потоков (docs/blender/nondeterminism.md): два прогона одного мира расходятся на ≈0,15–0,25 % блоков.
Чтобы получить мир ТОЧНО таким, как на вашем сервере (0 расхождений на записанных прогонах), нужно записать расписание этого прогона:

  1) python3 tools/gt/record_schedule.py flags [--dir каталог]
        печатает JVM-флаги (и кладёт chunkgen.jfc в каталог): добавьте их в команду запуска сервера перед -jar, например
        java -Xmx4g <флаги> -jar server.jar nogui
  2) сгенерируйте на сервере нужный мир (forceload или подойдите к области), остановите сервер (stop) — запись .jfr сохранится при выходе;
  3) python3 tools/gt/record_schedule.py convert запись.jfr --dim overworld --out мир.mcsched
  4) в Blender: Layers ▸ Features ▸ «Server schedule» — выберите мир.mcsched (область и сиды те же, что на сервере); либо mcgen_world_set_schedule() / MCGEN_* в mcgen-cli.

Нужны JDK 17+ (инструмент `jfr` из JDK) для шага 3. Запись содержит только позиции чанков, статусы и время шагов — ничего личного.
"""
import argparse, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
JFC = ('<?xml version="1.0" encoding="UTF-8"?>\n<configuration version="2.0" label="mcgen-chunkgen" description="minecraft.ChunkGeneration + ChunkRegionRead" provider="mcgen">\n'
       '  <event name="minecraft.ChunkGeneration">\n    <setting name="enabled">true</setting>\n    <setting name="threshold">0 ms</setting>\n  </event>\n'
       '  <event name="minecraft.ChunkRegionRead">\n    <setting name="enabled">true</setting>\n    <setting name="threshold">0 ms</setting>\n  </event>\n</configuration>\n')


def cmd_flags(a):
    d = os.path.abspath(a.dir or os.getcwd())
    os.makedirs(d, exist_ok=True)
    jfc = os.path.join(d, 'chunkgen.jfc')
    open(jfc, 'w').write(JFC)
    jfr = os.path.join(d, a.name + '.jfr')
    print(f'-XX:StartFlightRecording=filename={jfr},settings={jfc},dumponexit=true')
    print(f'# конфигурация записана: {jfc}; запись появится в {jfr} после остановки сервера', file=sys.stderr)


def cmd_convert(a):
    dim = {'overworld': 'overworld', 'nether': 'the_nether', 'the_nether': 'the_nether', 'end': 'the_end', 'the_end': 'the_end'}[a.dim]
    r = subprocess.run([sys.executable, os.path.join(HERE, 'jfr_order.py'), a.jfr, '--dim', dim, '--sched', a.out], capture_output=True, text=True)
    sys.stdout.write(r.stdout[-400:]); sys.stderr.write(r.stderr[-400:])
    if r.returncode != 0:
        sys.exit(r.returncode)
    # заголовок измерения для проверки при загрузке
    txt = open(a.out).read()
    if '# dim ' not in txt:
        open(a.out, 'w').write(f'# dim {dim}\n' + txt)
    print(f'готово: {a.out}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    f = sp.add_parser('flags', help='JVM-флаги для записи расписания'); f.add_argument('--dir'); f.add_argument('--name', default='schedule'); f.set_defaults(fn=cmd_flags)
    c = sp.add_parser('convert', help='.jfr → .mcsched'); c.add_argument('jfr'); c.add_argument('--dim', default='overworld', choices=['overworld', 'nether', 'end', 'the_nether', 'the_end'])
    c.add_argument('--out', required=True); c.set_defaults(fn=cmd_convert)
    a = ap.parse_args(); a.fn(a)


if __name__ == '__main__':
    main()
