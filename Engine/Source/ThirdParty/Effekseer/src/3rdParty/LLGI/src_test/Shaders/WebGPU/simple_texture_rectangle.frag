diagnostic(off, derivative_uniformity);

@group(1) @binding(0) var txt : texture_2d<f32>;

@group(2) @binding(0) var smp : sampler;

var<private> v : vec4<f32>;

struct PS_INPUT {
  Position : vec4<f32>,
  UV : vec2<f32>,
  Color : vec4<f32>,
}

fn main_inner(v_1 : vec4<f32>, v_2 : vec2<f32>, v_3 : vec4<f32>) {
  var input : PS_INPUT;
  var param : PS_INPUT;
  input.Position = v_1;
  input.UV = v_2;
  input.Color = v_3;
  param = input;
  v = v_4(&(param));
}

fn v_4(input : ptr<function, PS_INPUT>) -> vec4<f32> {
  var c : vec4<f32>;
  c = textureSample(txt, smp, (*(input)).UV);
  c.w = 255.0f;
  return c;
}

@fragment
fn main(@builtin(position) v_5 : vec4<f32>, @location(0u) v_6 : vec2<f32>, @location(1u) v_7 : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(v_5, v_6, v_7);
  return v;
}
