diagnostic(off, derivative_uniformity);

struct RibbonRecord {
  BaseIndex : u32,
  Count : u32,
  Head : u32,
  Width : f32,
  ColorTag : u32,
  Reserved0 : u32,
  Reserved1 : u32,
  Reserved2 : u32,
}

struct RibbonData {
  m : array<RibbonRecord>,
}

@group(1) @binding(1) var<storage, read> RibbonData_1 : RibbonData;

struct HistoryRecord {
  Position : vec3<f32>,
  ColorTag : u32,
  Tangent : vec3<f32>,
  Sequence : u32,
}

struct HistoryData {
  m_1 : array<HistoryRecord>,
}

@group(1) @binding(0) var<storage, read> HistoryData_1 : HistoryData;

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

fn v_8(tag : ptr<function, u32>, pointIndex : ptr<function, u32>) -> vec4<f32> {
  var tint : f32;
  tint = select(1.0f, 0.75f, (*(pointIndex) == 0u));
  if ((*(tag) == 0u)) {
    return vec4<f32>(tint, 0.0f, 0.0f, 1.0f);
  }
  return vec4<f32>(0.0f, tint, 0.0f, 1.0f);
}

struct RibbonRecord_1 {
  BaseIndex : u32,
  Count : u32,
  Head : u32,
  Width : f32,
  ColorTag : u32,
  Reserved0 : u32,
  Reserved1 : u32,
  Reserved2 : u32,
}

struct HistoryRecord_1 {
  Position : vec3<f32>,
  ColorTag : u32,
  Tangent : vec3<f32>,
  Sequence : u32,
}

fn v_7(input : ptr<function, VS_INPUT>) -> VS_OUTPUT {
  var ribbon : RibbonRecord_1;
  var pointIndex : u32;
  var historyIndex : u32;
  var history : HistoryRecord_1;
  var side : f32;
  var position : vec3<f32>;
  var output : VS_OUTPUT;
  var param : u32;
  var param_1 : u32;
  let v_9 = RibbonData_1.m[(*(input)).InstanceId];
  ribbon.BaseIndex = v_9.BaseIndex;
  ribbon.Count = v_9.Count;
  ribbon.Head = v_9.Head;
  ribbon.Width = v_9.Width;
  ribbon.ColorTag = v_9.ColorTag;
  ribbon.Reserved0 = v_9.Reserved0;
  ribbon.Reserved1 = v_9.Reserved1;
  ribbon.Reserved2 = v_9.Reserved2;
  pointIndex = min(((*(input)).VertexId / 2u), (ribbon.Count - 1u));
  historyIndex = (ribbon.BaseIndex + ((ribbon.Head + pointIndex) % ribbon.Count));
  let v_10 = HistoryData_1.m_1[historyIndex];
  history.Position = v_10.Position;
  history.ColorTag = v_10.ColorTag;
  history.Tangent = v_10.Tangent;
  history.Sequence = v_10.Sequence;
  side = select(-1.0f, 1.0f, (((*(input)).VertexId & 1u) == 0u));
  position = history.Position;
  let v_11 = ((history.Tangent.xy * ribbon.Width) * side);
  let v_12 = (position.xy + v_11);
  position.x = v_12.x;
  position.y = v_12.y;
  let v_13 = position;
  output.g_position = vec4<f32>(v_13.x, v_13.y, v_13.z, 1.0f);
  param = ribbon.ColorTag;
  param_1 = pointIndex;
  output.g_color = v_8(&(param), &(param_1));
  return output;
}

struct tint_symbol {
  @builtin(position)
  m_2 : vec4<f32>,
  @location(0u)
  m_3 : vec4<f32>,
}

@vertex
fn main(@location(0u) v_14 : vec3<f32>, @location(1u) v_15 : vec2<f32>, @location(2u) v_16 : vec4<f32>, @builtin(vertex_index) v_17 : u32, @builtin(instance_index) v_18 : u32) -> tint_symbol {
  main_inner(v_14, v_15, v_16, v_17, v_18);
  return tint_symbol(v, v_1);
}
