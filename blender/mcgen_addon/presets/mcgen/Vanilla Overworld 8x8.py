import bpy
s = bpy.context.scene.mcgen
s.dimension = 'minecraft:overworld'
s.preset = 'normal'
s.seed_mode = 'UNIFIED'
s.unit = 'BLOCKS'
s.origin_x = 0
s.origin_z = 0
s.size_x = 8
s.size_z = 8
s.use_terrain = True
s.use_surface = True
s.use_caves = True
s.use_features = False
s.use_structures = False
s.use_tweaks = False
