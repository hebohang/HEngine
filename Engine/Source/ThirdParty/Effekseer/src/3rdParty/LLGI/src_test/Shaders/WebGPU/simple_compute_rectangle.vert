diagnostic(off, derivative_uniformity);

struct compute {
  m : array<f32>,
}

@group(1) @binding(0) var<storage, read> compute_1 : compute;

var<private> v : vec4<f32>;

var<private> v_1 : vec2<f32>;

var<private> v_2 : vec4<f32>;

struct VS_INPUT {
  Position : vec3<f32>,
  UV : vec2<f32>,
  Color : vec4<f32>,
}

struct VS_OUTPUT {
  Position : vec4<f32>,
  UV : vec2<f32>,
  Color : vec4<f32>,
}

fn main_inner(v_3 : vec3<f32>, v_4 : vec2<f32>, v_5 : vec4<f32>) {
  var input : VS_INPUT;
  var flattenTemp : VS_OUTPUT;
  var param : VS_INPUT;
  input.Position = v_3;
  input.UV = v_4;
  input.Color = v_5;
  param = input;
  flattenTemp = v_6(&(param));
  v = flattenTemp.Position;
  v_1 = flattenTemp.UV;
  v_2 = flattenTemp.Color;
}

fn v_6(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var output : VS_OUTPUT;
  let v_7 = (*(input)).Position;
  let v_8 = compute_1.m[0i];
  output.Position = (vec4<f32>(v_7.x, v_7.y, v_7.z, 1.0f) + vec4<f32>(v_8, v_8, v_8, v_8));
  output.UV = (*(input)).UV;
  output.Color = (*(input)).Color;
  return output;
}

struct tint_symbol {
  @builtin(position)
  m_1 : vec4<f32>,
  @location(0u)
  m_2 : vec2<f32>,
  @location(1u)
  m_3 : vec4<f32>,
}

@vertex
fn main(@location(0u) v_9 : vec3<f32>, @location(1u) v_10 : vec2<f32>, @location(2u) v_11 : vec4<f32>) -> tint_symbol {
  main_inner(v_9, v_10, v_11);
  return tint_symbol(v, v_1, v_2);
}
