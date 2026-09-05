diagnostic(off, derivative_uniformity);

var<private> v : vec4<f32>;

struct PS_INPUT {
  g_position : vec4<f32>,
  g_color : vec4<f32>,
}

fn main_inner(v_1 : vec4<f32>, v_2 : vec4<f32>) {
  var input : PS_INPUT;
  var param : PS_INPUT;
  input.g_position = v_1;
  input.g_color = v_2;
  param = input;
  v = v_3(&(param));
}

fn v_3(input : ptr<function, PS_INPUT>) -> vec4<f32> {
  return (*(input)).g_color;
}

@fragment
fn main(@builtin(position) v_4 : vec4<f32>, @location(0u) v_5 : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(v_4, v_5);
  return v;
}
