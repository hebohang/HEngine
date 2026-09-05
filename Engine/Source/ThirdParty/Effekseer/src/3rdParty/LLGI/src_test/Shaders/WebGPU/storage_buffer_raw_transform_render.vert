diagnostic(off, derivative_uniformity);

struct TransformData {
  m : array<u32>,
}

@group(1) @binding(0) var<storage, read> TransformData_1 : TransformData;

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

fn v_7(index : ptr<function, u32>, localPosition : ptr<function, vec3<f32>>) -> vec3<f32> {
  var base : u32;
  var c0 : vec3<f32>;
  var c1 : vec3<f32>;
  var c2 : vec3<f32>;
  var c3 : vec3<f32>;
  base = (*(index) * 80u);
  c0 = bitcast<vec3<f32>>(vec3<u32>(TransformData_1.m[bitcast<i32>(((base + 32u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 48u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 64u) >> bitcast<u32>(2i)))]));
  c1 = bitcast<vec3<f32>>(vec3<u32>(TransformData_1.m[bitcast<i32>(((base + 36u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 52u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 68u) >> bitcast<u32>(2i)))]));
  c2 = bitcast<vec3<f32>>(vec3<u32>(TransformData_1.m[bitcast<i32>(((base + 40u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 56u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 72u) >> bitcast<u32>(2i)))]));
  c3 = bitcast<vec3<f32>>(vec3<u32>(TransformData_1.m[bitcast<i32>(((base + 44u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 60u) >> bitcast<u32>(2i)))], TransformData_1.m[bitcast<i32>(((base + 76u) >> bitcast<u32>(2i)))]));
  return ((((c0 * (*(localPosition)).x) + (c1 * (*(localPosition)).y)) + (c2 * (*(localPosition)).z)) + c3);
}

fn v_6(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var position : vec3<f32>;
  var param : u32;
  var param_1 : vec3<f32>;
  var output : VS_OUTPUT;
  param = (*(input)).InstanceId;
  param_1 = (*(input)).g_position;
  position = v_7(&(param), &(param_1));
  let v_8 = position;
  output.g_position = vec4<f32>(v_8.x, v_8.y, v_8.z, 1.0f);
  let v_9 = ((*(input)).InstanceId == 0u);
  output.g_color = select(vec4<f32>(0.0f, 1.0f, 0.0f, 1.0f), vec4<f32>(1.0f, 0.0f, 0.0f, 1.0f), vec4<bool>(v_9, v_9, v_9, v_9));
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
