"""Inspect available UE material parameter queries without saving assets."""

import unreal


material = unreal.EditorAssetLibrary.load_asset(
    "/Game/SICKA_DYNASTY/Materials/MI_StairSets")
library = unreal.MaterialEditingLibrary
print("HE_UE_MATERIAL_QUERY_NAMES " + str([name for name in dir(library)
      if "texture" in name.lower() and "parameter" in name.lower()]))
for parameter in ("BaseColor", "Normal", "ORM"):
    for method_name in ("get_material_instance_texture_parameter_value",
                        "get_material_default_texture_parameter_value"):
        method = getattr(library, method_name, None)
        if method is None:
            continue
        try:
            value = method(material, parameter)
            print("HE_UE_MATERIAL_QUERY " + method_name + " " + parameter + " " + str(value))
        except Exception as exc:
            print("HE_UE_MATERIAL_QUERY_ERROR " + method_name + " " + str(exc))
