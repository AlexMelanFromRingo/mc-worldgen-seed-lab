#!/usr/bin/env python3
"""g6_scattered_starts.py — g6_starts для построек с ПОСТ-сдвигом по высоте (jungle_temple, swamp_hut, igloo, buried_treasure).

В эталоне сохранён bounding box части ПОСЛЕ рисования (ScatteredFeaturePiece.updateAverageGroundHeight / IglooPiece / BuriedTreasurePiece двигают его по y
при postProcess), а старт g6_starts строится до рисования. Поэтому здесь сверяются id, x/z-границы, высота (y1 − y0), ориентация и GD, а точная
высота bounding box проверяется блоками (g6_blocks.py: блоки не совпали бы при неверной высоте).
  g6_scattered_starts.py [--sets jungle_temples,swamp_huts,igloos,buried_treasures] [-v]   (те же переменные окружения, что у g6_starts.py)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g6_starts as m

_pk, _opk = m.piece_key, m.our_piece_key


def _norm(d):
    bb = list(d['bb'])
    d['bb'] = [bb[0], 0, bb[2], bb[3], bb[4] - bb[1], bb[5]]
    return d


m.piece_key = lambda p: _norm(_pk(p))
m.our_piece_key = lambda p, ref=None: _norm(_opk(p, ref))

if __name__ == '__main__':
    if '--sets' not in sys.argv:
        sys.argv += ['--sets', 'jungle_temples,swamp_huts,igloos,buried_treasures']
    m.main()
