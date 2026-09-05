diagnostic(off, derivative_uniformity);

struct CB {
  offsets : array<vec4<f32>, 10u>,
}

@group(0) @binding(0) var<uniform> v : CB;

var<private> v_1 : vec4<f32>;

var<private> v_2 : vec4<f32>;

struct VS_INPUT {
  g_position : vec3<f32>,
  g_uv : vec2<f32>,
  g_color : vec4<f32>,
  InstanceId : u32,
}

struct VS_OUTPUT {
  g_position : vec4<f32>,
  g_color : vec4<f32>,
}

fn main_inner(v_3 : vec3<f32>, v_4 : vec2<f32>, v_5 : vec4<f32>, v_6 : u32) {
  var input : VS_INPUT;
  var flattenTemp : VS_OUTPUT;
  var param : VS_INPUT;
  input.g_position = v_3;
  input.g_uv = v_4;
  input.g_color = v_5;
  input.InstanceId = v_6;
  param = input;
  flattenTemp = v_7(&(param));
  v_1 = flattenTemp.g_position;
  v_2 = flattenTemp.g_color;
}

fn v_7(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var output : VS_OUTPUT;
  let v_8 = (*(input)).g_position;
  output.g_position = vec4<f32>(v_8.x, v_8.y, v_8.z, 1.0f);
  let v_9 = v.offsets[(*(input)).InstanceId].x;
  output.g_position.x = (output.g_position.x + v_9);
  let v_10 = v.offsets[(*(input)).InstanceId].y;
  output.g_position.y = (output.g_position.y + v_10);
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
fn main(@location(0u) v_11 : vec3<f32>, @location(1u) v_12 : vec2<f32>, @location(2u) v_13 : vec4<f32>, @builtin(instance_index) v_14 : u32) -> tint_symbol {
  main_inner(v_11, v_12, v_13, v_14);
  return tint_symbol(v_1, v_2);
}
