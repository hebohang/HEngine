diagnostic(off, derivative_uniformity);

struct CB {
  offset : vec4<f32>,
}

@group(0) @binding(0) var<uniform> v : CB;

var<private> v_1 : vec4<f32>;

var<private> v_2 : vec2<f32>;

var<private> v_3 : vec4<f32>;

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

fn main_inner(v_4 : vec3<f32>, v_5 : vec2<f32>, v_6 : vec4<f32>) {
  var input : VS_INPUT;
  var flattenTemp : VS_OUTPUT;
  var param : VS_INPUT;
  input.Position = v_4;
  input.UV = v_5;
  input.Color = v_6;
  param = input;
  flattenTemp = v_7(&(param));
  v_1 = flattenTemp.Position;
  v_2 = flattenTemp.UV;
  v_3 = flattenTemp.Color;
}

fn v_7(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var output : VS_OUTPUT;
  let v_8 = (*(input)).Position;
  output.Position = (vec4<f32>(v_8.x, v_8.y, v_8.z, 1.0f) + v.offset);
  output.UV = (*(input)).UV;
  output.Color = (*(input)).Color;
  return output;
}

struct tint_symbol {
  @builtin(position)
  m : vec4<f32>,
  @location(0u)
  m_1 : vec2<f32>,
  @location(1u)
  m_2 : vec4<f32>,
}

@vertex
fn main(@location(0u) v_9 : vec3<f32>, @location(1u) v_10 : vec2<f32>, @location(2u) v_11 : vec4<f32>) -> tint_symbol {
  main_inner(v_9, v_10, v_11);
  return tint_symbol(v_1, v_2, v_3);
}
