diagnostic(off, derivative_uniformity);

struct CS_INPUT {
  value1 : f32,
  value2 : f32,
}

struct read_2 {
  m : array<CS_INPUT>,
}

@group(1) @binding(0) var<storage, read> read_1 : read_2;

var<private> v : vec4<f32>;

var<private> v_1 : vec4<f32>;

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

fn main_inner(v_2 : vec3<f32>, v_3 : vec2<f32>, v_4 : vec4<f32>, v_5 : u32) {
  var input : VS_INPUT;
  var flattenTemp : VS_OUTPUT;
  var param : VS_INPUT;
  input.g_position = v_2;
  input.g_uv = v_3;
  input.g_color = v_4;
  input.InstanceId = v_5;
  param = input;
  flattenTemp = v_6(&(param));
  v = flattenTemp.g_position;
  v_1 = flattenTemp.g_color;
}

fn v_6(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var output : VS_OUTPUT;
  let v_7 = (*(input)).g_position;
  output.g_position = vec4<f32>(v_7.x, v_7.y, v_7.z, 1.0f);
  let v_8 = read_1.m[(*(input)).InstanceId].value1;
  output.g_position.x = (output.g_position.x + v_8);
  let v_9 = read_1.m[(*(input)).InstanceId].value2;
  output.g_position.y = (output.g_position.y + v_9);
  output.g_color = (*(input)).g_color;
  return output;
}

struct tint_symbol {
  @builtin(position)
  m_1 : vec4<f32>,
  @location(0u)
  m_2 : vec4<f32>,
}

@vertex
fn main(@location(0u) v_10 : vec3<f32>, @location(1u) v_11 : vec2<f32>, @location(2u) v_12 : vec4<f32>, @builtin(instance_index) v_13 : u32) -> tint_symbol {
  main_inner(v_10, v_11, v_12, v_13);
  return tint_symbol(v, v_1);
}
