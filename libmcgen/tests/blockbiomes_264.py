#!/usr/bin/env python3
"""26.4-snapshot-2 хранит биомы секций поблочно (4096 значений, CachedChunkBiomeResolver). Сверка с mcgen_biome_at
(`mcgen-cli biome --step 1`) по всем блокам чанков эталона W6.

    python3 libmcgen/tests/blockbiomes_264.py [--ref run/gt/26.4-snapshot-2/raw/overworld-s12345-c0_0-r2_t2] [--chunks 3]
"""
import argparse, glob, os, struct, subprocess, sys, zlib, json

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import anvil  # noqa: E402
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', default=os.path.join(ROOT, 'run/gt/26.4-snapshot-2/raw/overworld-s12345-c0_0-r2_t2'))
    ap.add_argument('--chunks', type=int, default=3, help='N×N чанков начиная с (0,0)')
    a = ap.parse_args()
    m = json.load(open(os.path.join(a.ref, 'manifest.json')))
    reg = glob.glob(os.path.join(a.ref, 'world', 'dimensions', 'minecraft', m['dim'] if m['dim'] != 'overworld' else 'overworld', 'region', 'r.0.0.mca'))[0]
    f = open(reg, 'rb').read()
    ref = {}
    for cx in range(a.chunks):
        for cz in range(a.chunks):
            idx = cx + cz * 32
            off = struct.unpack('>I', b'\0' + f[idx * 4:idx * 4 + 3])[0]
            L = struct.unpack('>I', f[off * 4096:off * 4096 + 4])[0]
            nbt = anvil.parse_nbt(zlib.decompress(f[off * 4096 + 5:off * 4096 + 4 + L]))
            for s in nbt['sections']:
                bi = s.get('biomes')
                if not bi:
                    continue
                Y, pal = int(s['Y']), bi['palette']
                vals = [0] * 4096
                if 'data' in bi:
                    bits = max(1, (len(pal) - 1).bit_length()); per = 64 // bits; vals = []
                    for l in bi['data']:
                        l = int(l) & ((1 << 64) - 1)
                        vals += [(l >> (k * bits)) & ((1 << bits) - 1) for k in range(per)]
                    vals = vals[:4096]
                for ly in range(16):
                    for z in range(16):
                        for x in range(16):
                            ref[(cx * 16 + x, Y * 16 + ly, cz * 16 + z)] = pal[vals[(ly * 16 + z) * 16 + x]]
    ys = sorted({k[1] for k in ref})
    n = a.chunks * 16; tot = bad = 0
    for y in ys:
        r = subprocess.run([CLI, 'biome', '--pack', os.path.join(ROOT, 'run', 'pack-26.4-snapshot-2'), '--version', '26.4-snapshot-2', '--dim',
                            'minecraft:' + m['dim'], '--seed', str(m['seed']), '--x0', '0', '--z0', '0', '--nx', str(n), '--nz', str(n), '--step', '1',
                            '--y', str(y)], capture_output=True, text=True)
        ours = r.stdout.split()
        for z in range(n):
            for x in range(n):
                k = (x, y, z)
                if k in ref:
                    tot += 1; bad += ref[k] != ours[z * n + x]
    print(f'26.4-snapshot-2 блочные биомы ({a.chunks}x{a.chunks} чанков, все y): сравнено {tot}, расхождений {bad}')
    return bad != 0


if __name__ == '__main__':
    sys.exit(main())
