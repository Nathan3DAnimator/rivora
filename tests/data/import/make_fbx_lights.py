# Writes lights.fbx, the FBX lights and cameras import test (Blender 5.0:
#   blender --background --factory-startup --python make_fbx_lights.py): a cube, a spot light 3 m up pointing down
# (a 60° cone, 25% blend), a sun tilted 45°, a camera 5 m back looking at the cube (50 mm on 36 mm film: 39.6°). The
# expected counts are in CMakeLists.txt.
import bpy, math, os

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, 0.5))
bpy.context.object.name = "Box"

spot = bpy.data.lights.new("Spot", "SPOT")
spot.energy, spot.spot_size, spot.spot_blend = 500.0, math.radians(60.0), 0.25
o = bpy.data.objects.new("Spot", spot)
o.location = (0, 0, 3)
bpy.context.collection.objects.link(o)

sun = bpy.data.lights.new("Sun", "SUN")
o = bpy.data.objects.new("Sun", sun)
o.rotation_euler = (math.radians(45.0), 0, 0)
bpy.context.collection.objects.link(o)

cam = bpy.data.cameras.new("Shot")
cam.lens, cam.sensor_width, cam.sensor_fit = 50.0, 36.0, "HORIZONTAL"
o = bpy.data.objects.new("Shot", cam)
o.location, o.rotation_euler = (0, -5, 1), (math.radians(90.0), 0, 0)
bpy.context.collection.objects.link(o)

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lights.fbx")
bpy.ops.export_scene.fbx(filepath=out, object_types={"MESH", "LIGHT", "CAMERA"})
print("written", out)
