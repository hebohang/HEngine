diagnostic(off, derivative_uniformity);

@group(1) @binding(0) var txt : texture_2d<f32>;

@group(2) @binding(0) var smp : sampler;

var<private> v : vec4<f32>;

var<private> v_1 : vec4<f32>;

struct PS_INPUT {
  Position : vec4<f32>,
  UV : vec2<f32>,
  Color : vec4<f32>,
}

struct PS_OUTPUT {
  Color0 : vec4<f32>,
  Color1 : vec4<f32>,
}

fn main_inner(v_2 : vec4<f32>, v_3 : vec2<f32>, v_4 : vec4<f32>) {
  var input : PS_INPUT;
  var flattenTemp : PS_OUTPUT;
  var param : PS_INPUT;
  input.Position = v_2;
  input.UV = v_3;
  input.Color = v_4;
  param = input;
  flattenTemp = v_5(&(param));
  v = flattenTemp.Color0;
  v_1 = flattenTemp.Color1;
}

fn v_5(input : ptr<function, PS_INPUT>) -> PS_OUTPUT {
  var c : vec4<f32>;
  var output : PS_OUTPUT;
  c = textureSample(txt, smp, (*(input)).UV);
  c.w = 255.0f;
  output.Color0 = c;
  c.x = (1.0f - c.x);
  c.y = (1.0f - c.y);
  c.z = (1.0f - c.z);
  output.Color1 = c;
  return output;
}

struct tint_symbol {
  @location(0u)
  m : vec4<f32>,
  @location(1u)
  m_1 : vec4<f32>,
}

@fragment
fn main(@builtin(position) v_6 : vec4<f32>, @location(0u) v_7 : vec2<f32>, @location(1u) v_8 : vec4<f32>) -> tint_symbol {
  main_inner(v_6, v_7, v_8);
  return tint_symbol(v, v_1);
}
