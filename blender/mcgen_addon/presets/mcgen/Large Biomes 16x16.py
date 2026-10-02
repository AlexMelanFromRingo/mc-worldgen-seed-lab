import bpy
s = bpy.context.scene.mcgen
s.dimension = 'minecraft:overworld'
s.preset = 'large_biomes'
s.seed_mode = 'UNIFIED'
s.size_x = 16
s.size_z = 16
s.use_terrain = True
s.use_surface = True
s.use_caves = False
s.use_features = False
s.use_structures = False
s.use_tweaks = False
s.chunks_per_object = '2'
