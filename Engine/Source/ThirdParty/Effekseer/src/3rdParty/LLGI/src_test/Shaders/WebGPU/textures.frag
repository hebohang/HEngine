diagnostic(off, derivative_uniformity);

@group(1) @binding(0) var g_texture1 : texture_2d<f32>;

@group(2) @binding(0) var g_sampler1 : sampler;

@group(1) @binding(1) var g_texture2 : texture_2d_array<f32>;

@group(2) @binding(1) var g_sampler2 : sampler;

@group(1) @binding(2) var g_texture3 : texture_3d<f32>;

@group(2) @binding(2) var g_sampler3 : sampler;

var<private> v : vec4<f32>;

struct PS_Input {
  Pos : vec4<f32>,
  UV : vec2<f32>,
  Color : vec4<f32>,
}

fn main_inner(v_1 : vec4<f32>, v_2 : vec2<f32>, v_3 : vec4<f32>) {
  var Input : PS_Input;
  Input.Pos = v_1;
  Input.UV = v_2;
  Input.Color = v_3;
  v = v_4(Input);
}

fn v_4(Input : PS_Input) -> vec4<f32> {
  if ((Input.UV.x < 0.30000001192092895508f)) {
    return textureSample(g_texture1, g_sampler1, Input.UV);
  } else if ((Input.UV.x < 0.60000002384185791016f)) {
    let v_5 = Input.UV;
    let v_6 = vec3<f32>(v_5.x, v_5.y, 1.0f);
    return textureSample(g_texture2, g_sampler2, v_6.xy, i32(v_6.z));
  }
  let v_7 = Input.UV;
  return textureSample(g_texture3, g_sampler3, vec3<f32>(v_7.x, v_7.y, 0.5f));
}

@fragment
fn main(@builtin(position) v_8 : vec4<f32>, @location(0u) v_9 : vec2<f32>, @location(1u) v_10 : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(v_8, v_9, v_10);
  return v;
}
