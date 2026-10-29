"""Run in UE Python: run(optional_existing_lookup_texture_path).

Compiles the shipped HLSL, renders float UVs and checks numeric edge cases.
All materials and render targets are transient. No asset saves or UI edits.
"""
import json
import math
from pathlib import Path
import unreal


def run(lookup_texture_path=None):
    spec = json.loads((Path(__file__).resolve().parents[1] /
                       'Templates/MaterialClipboard/HighlightLookupUV.json').read_text(encoding='utf-8'))
    definition = spec['graph_spec']['custom_hlsl_nodes'][0]
    lib = unreal.MaterialEditingLibrary
    render = unreal.RenderingLibrary
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    material = unreal.new_object(unreal.Material)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    uv = lib.create_material_expression(material, unreal.MaterialExpressionCustom)
    uv.set_editor_property('code', definition['code'])
    uv.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    inputs = []
    for name in ('StrandDirectionWS', 'ViewDirectionWS', 'LightDirectionWS', 'MaskG'):
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        inputs.append(item)
    uv.set_editor_property('inputs', inputs)
    constants = [lib.create_material_expression(material, unreal.MaterialExpressionConstant3Vector) for _ in range(3)]
    constants.append(lib.create_material_expression(material, unreal.MaterialExpressionConstant))
    for node, item in zip(constants, inputs):
        assert lib.connect_material_expressions(node, '', uv, str(item.get_editor_property('input_name')))
    adapter = lib.create_material_expression(material, unreal.MaterialExpressionCustom)
    adapter.set_editor_property('code', 'return float3(UV, 0);')
    adapter.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    item = unreal.CustomInput()
    item.set_editor_property('input_name', 'UV')
    adapter.set_editor_property('inputs', [item])
    assert lib.connect_material_expressions(uv, '', adapter, 'UV')
    assert lib.connect_material_property(adapter, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    target = render.create_render_target2d(world, 4, 4, unreal.TextureRenderTargetFormat.RTF_RGBA32F)
    cases = [
        ('perpendicular', (0,0,1), (1,0,0), (1,0,0), 1, (1,0)),
        ('parallel', (0,0,1), (0,0,1), (0,0,1), 1, (0,1)),
        ('mask', (0,0,1), (1,0,0), (1,0,0), .25, (.25,0)),
        ('unclamped mask', (0,0,1), (1,0,0), (1,0,0), 2, (2,0)),
        ('normalized', (0,0,7), (4,0,0), (2,0,0), 1, (1,0)),
        ('oblique', (0,0,1), (1,0,1), (1,0,0), 1, (math.cos(math.pi/8),.5)),
        ('zero strand', (0,0,0), (1,0,0), (1,0,0), 1, (0,0)),
        ('zero view', (0,0,1), (0,0,0), (1,0,0), 1, (0,0)),
        ('zero light', (0,0,1), (1,0,0), (0,0,0), 1, (0,0)),
        ('opposite half vector', (1,0,0), (1,0,0), (-1,0,0), 1, (0,1)),
    ]
    results = []

    def draw(t, v, light, mask):
        for node, value in zip(constants[:3], (t, v, light)):
            node.set_editor_property('constant', unreal.LinearColor(*value, 1))
        constants[3].set_editor_property('r', mask)
        lib.recompile_material(material)
        lib.get_statistics(material)
        render.draw_material_to_render_target(world, target, material)
        color = render.read_render_target_raw_pixel(world, target, 1, 1, normalize=False)
        return (color.r, color.g, color.b)

    try:
        for name, t, v, light, mask, expected in cases:
            actual = draw(t, v, light, mask)
            assert all(math.isfinite(x) for x in actual), (name, actual)
            assert max(abs(a-b) for a,b in zip(actual, expected)) < 2e-4, (name, actual, expected)
            results.append({'case': name, 'uv': actual[:2]})
        if lookup_texture_path:
            texture = unreal.load_asset(lookup_texture_path)
            assert texture, lookup_texture_path
            sample = lib.create_material_expression(material, unreal.MaterialExpressionTextureSample)
            sample.set_editor_property('texture', texture)
            sample.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
            assert lib.connect_material_expressions(uv, '', sample, 'UVs')
            assert lib.connect_material_property(sample, 'RGB', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
            colors = [draw((0,0,1), (math.cos(a),0,math.sin(a)), (1,0,0), .8)
                      for a in (0, .4, .8, 1.2)]
            assert all(math.isfinite(x) for c in colors for x in c)
            assert any(max(abs(a-b) for a,b in zip(colors[0], c)) > .001 for c in colors[1:]), colors
            results.append({'lookup_view_colors': colors, 'texture': lookup_texture_path})
        return results
    finally:
        render.release_render_target2d(target)
