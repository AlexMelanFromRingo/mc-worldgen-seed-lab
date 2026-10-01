#!/usr/bin/env python3
"""Извлекает из датапака версии (src/data-<V>/data/minecraft/worldgen) данные, нужные климат-сэмплеру:
  - параметры шумов (temperature, vegetation, continentalness, erosion, ridge, offset, *_large, nether/*)
  - для пресетов overworld / amplified / large_biomes: имена шумов, сплайн offset, константы depth/offset/ridges_folded
Выход: data/climate-<V>.txt (простой токенный формат, читается mc_data.h).

Формат (строки):
  noise <name> <fmt> <first_octave> <n> <base_amp> <normalize> <has_mod> a0 ... a{n-1}
       fmt=0: старый (firstOctave/amplitudes); fmt=1: новый (base_octave/octave_count/amplitude_modifiers/base_amplitude)
  preset <name> <temp_noise> <veg_noise> <cont_noise> <eros_noise> <ridge_noise> <offset_noise_for_shift>
  depth <preset> <from_y> <to_y> <from_value> <to_value> <offset_const>
  offset_spline <preset> <spline-tokens...>
      spline := S <coord 0..3> <n> { <loc> <deriv> ( C <value> | spline ) }*
  ridges_folded <c1> <c2> <c3>       (mul(add(abs(add(abs(ridges), c1)), c2), c3)
"""
import json, os, sys, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COORD = {'continents': 0, 'erosion': 1, 'ridges': 2, 'ridges_folded': 3}


def load(v, rel):
    with open(f'{ROOT}/src/data-{v}/data/minecraft/worldgen/{rel}') as f:
        return json.load(f)


def find_node(x, pred):
    """DFS: первый узел dict, удовлетворяющий pred"""
    if isinstance(x, dict):
        if pred(x):
            return x
        for val in x.values():
            r = find_node(val, pred)
            if r is not None:
                return r
    elif isinstance(x, list):
        for val in x:
            r = find_node(val, pred)
            if r is not None:
                return r
    return None


def spline_tokens(sp, out):
    if isinstance(sp, (int, float)):
        out += ['C', repr(float(sp))]
        return
    coord = sp['coordinate'].split('/')[-1]
    pts = sp['points']
    out += ['S', str(COORD[coord]), str(len(pts))]
    for p in pts:
        out += [repr(float(p['location'])), repr(float(p['derivative']))]
        val = p['value']
        if isinstance(val, dict):
            spline_tokens(val, out)
        else:
            out += ['C', repr(float(val))]


def noise_line(name, d):
    if 'firstOctave' in d:      # старый формат
        amps = d['amplitudes']
        return ['noise', name, '0', str(d['firstOctave']), str(len(amps)), '0', '0', '0'] + [repr(float(a)) for a in amps]
    n = d['octave_count']
    mods = d.get('amplitude_modifiers')
    norm = d.get('normalize', True)
    nv = 1 if norm is True else 0 if norm is False else 2   # "legacy" -> 2
    return ['noise', name, '1', str(d['base_octave']), str(n), repr(float(d.get('base_amplitude', 1.0))), str(nv),
            '1' if mods else '0'] + [repr(float(a)) for a in (mods if mods else [0.0] * n)]


def main():
    versions = sys.argv[1:] or ['26.1', '26.2', '26.3']
    for v in versions:
        lines = []
        noise_dir = f'{ROOT}/src/data-{v}/data/minecraft/worldgen/noise'
        names = ['temperature', 'vegetation', 'continentalness', 'erosion', 'ridge', 'offset',
                 'temperature_large', 'vegetation_large', 'continentalness_large', 'erosion_large',
                 'nether/temperature', 'nether/vegetation']
        for nm in names:
            lines.append(' '.join(noise_line(nm, load(v, f'noise/{nm}.json'))))
        presets = {'overworld': 'overworld', 'amplified': 'overworld_amplified', 'large_biomes': 'overworld_large_biomes'}
        for pname, dfdir in presets.items():
            ns = load(v, f'noise_settings/{pname}.json')
            router = ns['noise_router']

            def noise_of(fname):
                # климат-функция может быть ссылкой на density_function или инлайном
                fn = router[fname]
                if isinstance(fn, str):
                    fn = load(v, 'density_function/' + fn.split(':', 1)[1] + '.json')
                node = find_node(fn, lambda d: d.get('type') in ('minecraft:noise', 'minecraft:shifted_noise'))
                return node['noise'].split(':', 1)[1]
            tn, vn, cn, en, rn = (noise_of(k) for k in ('temperature', 'vegetation', 'continents', 'erosion', 'ridges'))
            lines.append(f'preset {pname} {tn} {vn} {cn} {en} {rn} offset')
            dep = load(v, f'density_function/{router["depth"].split(":",1)[1]}.json')
            g = find_node(dep, lambda d: d.get('type') in ('minecraft:y_clamped_gradient', 'minecraft:gradient'))
            if 'from_y' in g:
                fy, ty = g['from_y'], g['to_y']
            else:
                fy, ty = g['from_coordinate'], g['to_coordinate']
            off = load(v, f'density_function/{dfdir}/offset.json')
            spl = find_node(off, lambda d: d.get('type') == 'minecraft:spline')
            # константа в add(const, spline): узел add, у которого один аргумент — число, другой — spline
            def is_const_spline_add(d):
                if d.get('type') != 'minecraft:add':
                    return False
                args = [d.get(k) for k in ('argument1', 'argument2', 'left', 'right') if k in d]
                return any(isinstance(a, (int, float)) for a in args) and any(isinstance(a, dict) and a.get('type') == 'minecraft:spline' for a in args)
            addn = find_node(off, is_const_spline_add)
            const = [a for a in (addn.get(k) for k in ('argument1', 'argument2', 'left', 'right')) if isinstance(a, (int, float))][0]
            lines.append(f'depth {pname} {fy} {ty} {repr(float(g["from_value"]))} {repr(float(g["to_value"]))} {repr(float(const))}')
            toks = []
            spline_tokens(spl['spline'], toks)
            lines.append(f'offset_spline {pname} ' + ' '.join(toks))
        # ridges_folded (общая для пресетов)
        rf = load(v, 'density_function/overworld/ridges_folded.json')
        consts = []

        def collect(x):
            if isinstance(x, dict):
                for k in ('argument1', 'argument2', 'left', 'right'):
                    if k in x and isinstance(x[k], (int, float)):
                        consts.append(float(x[k]))
                for val in x.values():
                    collect(val)
        collect(rf)
        # порядок обхода: mul(-3.0), внешний add(-0.333..), внутренний add(-0.666..) -> печатаем c1(внутр.) c2(внешн.) c3(mul)
        lines.append('ridges_folded ' + ' '.join(repr(c) for c in reversed(consts)))
        out = f'{ROOT}/data/climate-{v}.txt'
        with open(out, 'w') as f:
            f.write('# generated by tools/extract_climate.py from ' + v + '\n')
            f.write('\n'.join(lines) + '\n')
        print(out, len(lines), 'lines, ridges_folded consts', consts)


if __name__ == '__main__':
    main()
