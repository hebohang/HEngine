diagnostic(off, derivative_uniformity);

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
  var output : VS_OUTPUT;
  let v_6 = (*(input)).g_position;
  output.g_position = vec4<f32>(v_6.x, v_6.y, v_6.z, 1.0f);
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
fn main(@location(0u) v_7 : vec3<f32>, @location(1u) v_8 : vec2<f32>, @location(2u) v_9 : vec4<f32>) -> tint_symbol {
  main_inner(v_7, v_8, v_9);
  return tint_symbol(v, v_1);
}
