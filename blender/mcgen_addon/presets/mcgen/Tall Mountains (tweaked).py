import bpy
s = bpy.context.scene.mcgen
s.dimension = 'minecraft:overworld'
s.preset = 'normal'
s.use_terrain = True
s.use_surface = True
s.use_caves = False
s.use_tweaks = True
s.tweaks.terrain_amplitude = 1.8
s.tweaks.terrain_steepness = 1.6
s.tweaks.climate_scale_xz = 1.5
s.size_x = 12
s.size_z = 12
