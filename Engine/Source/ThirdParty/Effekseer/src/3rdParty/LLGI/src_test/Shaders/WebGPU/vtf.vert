diagnostic(off, derivative_uniformity);

@group(1) @binding(0) var txt : texture_2d<f32>;

@group(2) @binding(0) var smp : sampler;

var<private> v : vec4<f32>;

var<private> v_1 : vec4<f32>;

struct VS_INPUT {
  g_position : vec3<f32>,
  g_uv : vec2<f32>,
  g_color : vec4<f32>,
}

struct VS_OUTPUT {
  g_position : vec4<f32>,
  g_color : vec4<f32>,
}

fn main_inner(v_2 : vec3<f32>, v_3 : vec2<f32>, v_4 : vec4<f32>) {
  var input : VS_INPUT;
  var flattenTemp : VS_OUTPUT;
  var param : VS_INPUT;
  input.g_position = v_2;
  input.g_uv = v_3;
  input.g_color = v_4;
  param = input;
  flattenTemp = v_5(&(param));
  v = flattenTemp.g_position;
  v_1 = flattenTemp.g_color;
}

fn v_5(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var c : vec4<f32>;
  var output : VS_OUTPUT;
  c = textureSampleLevel(txt, smp, (*(input)).g_uv, 0.0f);
  let v_6 = (*(input)).g_position;
  output.g_position = vec4<f32>(v_6.x, v_6.y, v_6.z, 1.0f);
  let v_7 = c.xy;
  let v_8 = (output.g_position.xy + v_7);
  output.g_position.x = v_8.x;
  output.g_position.y = v_8.y;
  output.g_color = (*(input)).g_color;
  return output;
}

struct tint_symbol {
  @builtin(position)
  m : vec4<f32>,
  @location(0u)
  m_1 : vec4<f32>,
}

@vertex
fn main(@location(0u) v_9 : vec3<f32>, @location(1u) v_10 : vec2<f32>, @location(2u) v_11 : vec4<f32>) -> tint_symbol {
  main_inner(v_9, v_10, v_11);
  return tint_symbol(v, v_1);
}
