import bpy
s = bpy.context.scene.mcgen
s.dimension = 'minecraft:overworld'
s.preset = 'normal'
s.seed_mode = 'SPLIT'
s.seed_climate = 'hello'
s.seed_terrain = '12345'
s.seed_structures = '777'
s.seed_features = 'world'
s.size_x = 8
s.size_z = 8
