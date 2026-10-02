import bpy
s = bpy.context.scene.mcgen
s.dimension = 'minecraft:overworld'
s.preset = 'normal'
s.use_terrain = True
s.use_surface = True
s.use_caves = True
s.use_tweaks = True
s.tweaks.cave_density = 2.0
s.tweaks.cave_size = 1.6
s.tweaks.lava_level_offset = 10
s.size_x = 6
s.size_z = 6
