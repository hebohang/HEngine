diagnostic(off, derivative_uniformity);

struct TransformData {
  m : array<u32>,
}

@group(1) @binding(0) var<storage, read> TransformData_1 : TransformData;

@group(1) @binding(1) var<storage, read> AttributeData : TransformData;

var<private> v : vec4<f32>;

var<private> v_1 : vec4<f32>;

struct VS_INPUT {
  g_position : vec3<f32>,
  g_uv : vec2<f32>,
  g_color : vec4<f32>,
  VertexId : u32,
  InstanceId : u32,
}

struct VS_OUTPUT {
  g_position : vec4<f32>,
  g_color : vec4<f32>,
}

fn main_inner(v_2 : vec3<f32>, v_3 : vec2<f32>, v_4 : vec4<f32>, v_5 : u32, v_6 : u32) {
  var input : VS_INPUT;
  var flattenTemp : VS_OUTPUT;
  var param : VS_INPUT;
  input.g_position = v_2;
  input.g_uv = v_3;
  input.g_color = v_4;
  input.VertexId = v_5;
  input.InstanceId = v_6;
  param = input;
  flattenTemp = v_7(&(param));
  v = flattenTemp.g_position;
  v_1 = flattenTemp.g_color;
}

fn v_8(vertexId : ptr<function, u32>) -> vec2<f32> {
  var corner : u32;
  corner = (*(vertexId) & 3u);
  if ((corner == 0u)) {
    return vec2<f32>(-0.20000000298023223877f, 0.20000000298023223877f);
  }
  if ((corner == 1u)) {
    return vec2<f32>(0.20000000298023223877f);
  }
  if ((corner == 2u)) {
    return vec2<f32>(0.20000000298023223877f, -0.20000000298023223877f);
  }
  return vec2<f32>(-0.20000000298023223877f);
}

fn v_9(index : ptr<function, u32>, localPosition : ptr<function, vec3<f32>>) -> vec3<f32> {
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

fn v_10(tag : ptr<function, u32>, vertexId : ptr<function, u32>) -> vec4<f32> {
  var cornerTint : f32;
  cornerTint = select(1.0f, 0.75f, (*(vertexId) == 0u));
  if ((*(tag) == 0u)) {
    return vec4<f32>(cornerTint, 0.0f, 0.0f, 1.0f);
  }
  if ((*(tag) == 1u)) {
    return vec4<f32>(0.0f, cornerTint, 0.0f, 1.0f);
  }
  return vec4<f32>(0.0f, 0.0f, cornerTint, 1.0f);
}

fn v_7(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var attrBase : u32;
  var offset : vec2<f32>;
  var byteAddrTemp : i32;
  var scale : f32;
  var colorTag : u32;
  var localPosition : vec3<f32>;
  var param : u32;
  var position : vec3<f32>;
  var param_1 : u32;
  var param_2 : vec3<f32>;
  var output : VS_OUTPUT;
  var param_3 : u32;
  var param_4 : u32;
  attrBase = ((*(input)).InstanceId * 16u);
  byteAddrTemp = bitcast<i32>(((attrBase + 0u) >> bitcast<u32>(2i)));
  offset = bitcast<vec2<f32>>(vec2<u32>(AttributeData.m[byteAddrTemp], AttributeData.m[(byteAddrTemp + 1i)]));
  scale = bitcast<f32>(AttributeData.m[bitcast<i32>(((attrBase + 8u) >> bitcast<u32>(2i)))]);
  colorTag = AttributeData.m[bitcast<i32>(((attrBase + 12u) >> bitcast<u32>(2i)))];
  localPosition = (*(input)).g_position;
  param = (*(input)).VertexId;
  let v_11 = (v_8(&(param)) * scale);
  localPosition.x = v_11.x;
  localPosition.y = v_11.y;
  param_1 = (*(input)).InstanceId;
  param_2 = localPosition;
  position = v_9(&(param_1), &(param_2));
  let v_12 = offset;
  let v_13 = (position.xy + v_12);
  position.x = v_13.x;
  position.y = v_13.y;
  let v_14 = position;
  output.g_position = vec4<f32>(v_14.x, v_14.y, v_14.z, 1.0f);
  param_3 = colorTag;
  param_4 = (*(input)).VertexId;
  output.g_color = v_10(&(param_3), &(param_4));
  return output;
}

struct tint_symbol {
  @builtin(position)
  m_1 : vec4<f32>,
  @location(0u)
  m_2 : vec4<f32>,
}

@vertex
fn main(@location(0u) v_15 : vec3<f32>, @location(1u) v_16 : vec2<f32>, @location(2u) v_17 : vec4<f32>, @builtin(vertex_index) v_18 : u32, @builtin(instance_index) v_19 : u32) -> tint_symbol {
  main_inner(v_15, v_16, v_17, v_18, v_19);
  return tint_symbol(v, v_1);
}
