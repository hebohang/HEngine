diagnostic(off, derivative_uniformity);

struct compute {
  m : array<f32>,
}

@group(1) @binding(0) var<storage, read> compute_1 : compute;

var<private> v : vec4<f32>;

struct CB {
  offset : vec4<f32>,
}

@group(0) @binding(1) var<uniform> v_1 : CB;

struct PS_INPUT {
  Position : vec4<f32>,
  UV : vec2<f32>,
  Color : vec4<f32>,
}

fn main_inner(v_2 : vec4<f32>, v_3 : vec2<f32>, v_4 : vec4<f32>) {
  var input : PS_INPUT;
  var param : PS_INPUT;
  input.Position = v_2;
  input.UV = v_3;
  input.Color = v_4;
  param = input;
  v = v_5(&(param));
}

fn v_5(input : ptr<function, PS_INPUT>) -> vec4<f32> {
  var c : vec4<f32>;
  let v_6 = (*(input)).Color;
  let v_7 = compute_1.m[0i];
  c = (v_6 + vec4<f32>(v_7, v_7, v_7, v_7));
  c.w = 1.0f;
  return c;
}

@fragment
fn main(@builtin(position) v_8 : vec4<f32>, @location(0u) v_9 : vec2<f32>, @location(1u) v_10 : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(v_8, v_9, v_10);
  return v;
}
