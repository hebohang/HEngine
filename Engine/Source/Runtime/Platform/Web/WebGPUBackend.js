import {multiply,perspective,lookAt} from './SceneRenderer.js';

// GPU-only adapter: asset loading, animation sampling and game presentation stay
// in SceneRenderer. Per-draw slots prevent queue writes aliasing earlier draws.
const shader=`
struct Frame { viewProjection: mat4x4f, camera: vec4f };
struct Draw { model: mat4x4f, color: vec4f, params: vec4f };
@group(0) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<storage,read> bones: array<mat4x4f>;
@group(1) @binding(2) var albedo: texture_2d<f32>;
@group(1) @binding(3) var albedoSampler: sampler;
struct Vertex {
 @location(0) position: vec3f, @location(1) normal: vec3f,
 @location(2) uv: vec2f, @location(3) joints: vec4f, @location(4) weights: vec4f
};
struct Varying {
 @builtin(position) clip: vec4f, @location(0) uv: vec2f,
 @location(1) normal: vec3f, @location(2) position: vec3f
};
@vertex fn vs(v: Vertex) -> Varying {
 var skin=mat4x4f(vec4f(1,0,0,0),vec4f(0,1,0,0),vec4f(0,0,1,0),vec4f(0,0,0,1));
 if(draw.params.x>0.5){
  skin=mat4x4f(vec4f(0),vec4f(0),vec4f(0),vec4f(0));
  for(var i=0u;i<4u;i++){if(v.weights[i]>0 && v.joints[i]>=0){skin+=bones[u32(v.joints[i])]*v.weights[i];}}
 }
 let world=draw.model*skin*vec4f(v.position,1);
 var output: Varying;output.clip=frame.viewProjection*world;
 // Shared camera uses OpenGL's -w..w clip depth; WebGPU requires 0..w.
 output.clip.z=(output.clip.z+output.clip.w)*0.5;
 output.uv=v.uv;output.position=world.xyz;
 output.normal=normalize((draw.model*vec4f((skin*vec4f(v.normal,0)).xyz,0)).xyz);
 return output;
}
@fragment fn fs(v: Varying) -> @location(0) vec4f {
 let c=textureSample(albedo,albedoSampler,v.uv)*draw.color;
 if(c.a<0.1){discard;}
 let light=0.58+0.42*max(dot(normalize(v.normal),normalize(vec3f(-0.3,0.8,0.5))),0);
 let rgb=c.rgb*(light+draw.params.y);
 let fog=smoothstep(45.0,130.0,distance(frame.camera.xyz,v.position));
 return vec4f(mix(rgb,vec3f(0.10,0.16,0.24),fog),c.a);
}`;

export class WebGPUBackend {
    static async create(canvas,gpu=globalThis.navigator?.gpu){
        if(!gpu)throw Error('WebGPU is unavailable in this browser/context');
        const adapter=await gpu.requestAdapter();
        if(!adapter)throw Error('No WebGPU adapter');
        const device=await adapter.requestDevice();
        const backend=new WebGPUBackend(canvas,device,gpu.getPreferredCanvasFormat());
        try{await backend.initialize();return backend;}catch(error){backend.dispose();throw error;}
    }
    constructor(canvas,device,format){
        this.canvas=canvas;this.device=device;this.format=format;this.resources=new Set();this.slots=[];this.frames=0;this.failure=null;
        this.onError=event=>{this.failure=event.error;};device.addEventListener('uncapturederror',this.onError);
        device.lost.then(info=>{if(!this.disposed)this.failure=Error('WebGPU device lost: '+info.message);});
    }
    track(resource){this.resources.add(resource);return resource;}
    buffer(data,usage){
        const buffer=this.track(this.device.createBuffer({size:Math.ceil(data.byteLength/4)*4,usage,mappedAtCreation:true}));
        new Uint8Array(buffer.getMappedRange()).set(new Uint8Array(data.buffer,data.byteOffset,data.byteLength));buffer.unmap();return buffer;
    }
    async initialize(){
        const device=this.device;
        if(device.limits.maxStorageBufferBindingSize<128*64||device.limits.maxStorageBuffersPerShaderStage<1)throw Error('Insufficient WebGPU bone buffer limits');
        const module=device.createShaderModule({code:shader,label:'HEngine scene'});
        const info=await module.getCompilationInfo(),errors=info.messages.filter(m=>m.type==='error');
        if(errors.length)throw Error(errors.map(m=>m.message).join('\n'));
        this.pipeline=await device.createRenderPipelineAsync({layout:'auto',
            vertex:{module,entryPoint:'vs',buffers:[{arrayStride:64,attributes:[
                {shaderLocation:0,offset:0,format:'float32x3'},{shaderLocation:1,offset:12,format:'float32x3'},
                {shaderLocation:2,offset:24,format:'float32x2'},{shaderLocation:3,offset:32,format:'float32x4'},
                {shaderLocation:4,offset:48,format:'float32x4'}]}]},
            fragment:{module,entryPoint:'fs',targets:[{format:this.format,blend:{
                color:{srcFactor:'src-alpha',dstFactor:'one-minus-src-alpha'},alpha:{srcFactor:'src-alpha',dstFactor:'one-minus-src-alpha'}}}]},
            primitive:{topology:'triangle-list',cullMode:'none'},depthStencil:{format:'depth24plus',depthWriteEnabled:true,depthCompare:'less'},multisample:{count:4}});
        this.frameBuffer=this.buffer(new Float32Array(20),GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST);
        this.frameGroup=device.createBindGroup({layout:this.pipeline.getBindGroupLayout(0),entries:[{binding:0,resource:{buffer:this.frameBuffer}}]});
        this.sampler=device.createSampler({minFilter:'linear',magFilter:'linear',mipmapFilter:'linear',addressModeU:'repeat',addressModeV:'repeat'});
        this.white=this.track(device.createTexture({size:[1,1],format:'rgba8unorm',usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST}));
        device.queue.writeTexture({texture:this.white},new Uint8Array([255,255,255,255]),{bytesPerRow:4},[1,1]);
        this.context=this.canvas.getContext('webgpu');if(!this.context)throw Error('WebGPU canvas unavailable');
        this.context.configure({device,format:this.format,alphaMode:'opaque'});
        await device.queue.onSubmittedWorkDone();this.check();
    }
    check(){if(this.failure){const error=Error(this.failure.message);error.renderBackendFailure=true;throw error;}}
    texture(bitmap){
        const {width,height}=bitmap;
        if(Math.max(width,height)>this.device.limits.maxTextureDimension2D)throw Error('Texture exceeds WebGPU device limit');
        const levels=1+Math.floor(Math.log2(Math.max(width,height)));
        const texture=this.track(this.device.createTexture({size:[width,height],mipLevelCount:levels,format:'rgba8unorm',usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST|GPUTextureUsage.RENDER_ATTACHMENT}));
        this.device.queue.copyExternalImageToTexture({source:bitmap},{texture},[width,height]);
        if(levels>1){
            const canvas=new OffscreenCanvas(width,height),context=canvas.getContext('2d');
            for(let level=1;level<levels;level++){
                canvas.width=Math.max(1,width>>level);canvas.height=Math.max(1,height>>level);
                context.drawImage(bitmap,0,0,canvas.width,canvas.height);
                this.device.queue.copyExternalImageToTexture({source:canvas},{texture,mipLevel:level},[canvas.width,canvas.height]);
            }
        }
        return texture;
    }
    upload(position,normal,uv,indices,joints,weights){
        const count=position.length/3,data=new Float32Array(count*16);
        for(let i=0;i<count;i++){
            data.set(position.slice(i*3,i*3+3),i*16);data.set(normal.slice(i*3,i*3+3),i*16+3);data.set(uv.slice(i*2,i*2+2),i*16+6);
            if(joints&&weights){data.set(joints.slice(i*4,i*4+4),i*16+8);data.set(weights.slice(i*4,i*4+4),i*16+12);}
        }
        return {vertex:this.buffer(data,GPUBufferUsage.VERTEX),index:this.buffer(new Uint32Array(indices),GPUBufferUsage.INDEX),count:indices.length,skinned:!!(joints&&weights)};
    }
    beginFrame(eye,target,viewport,maxPixelRatio){
        this.check();const device=this.device,ratio=Math.min(viewport.ratio||1,maxPixelRatio),limit=device.limits.maxTextureDimension2D;
        const w=Math.min(limit,Math.max(1,Math.round(viewport.width*ratio))),h=Math.min(limit,Math.max(1,Math.round(viewport.height*ratio)));
        if(!this.depth||this.canvas.width!==w||this.canvas.height!==h){
            this.depth?.destroy();this.color?.destroy();this.canvas.width=w;this.canvas.height=h;
            this.depth=device.createTexture({size:[w,h],format:'depth24plus',sampleCount:4,usage:GPUTextureUsage.RENDER_ATTACHMENT});
            this.color=device.createTexture({size:[w,h],format:this.format,sampleCount:4,usage:GPUTextureUsage.RENDER_ATTACHMENT});
        }
        const frame=new Float32Array(20);frame.set(multiply(perspective(w/h),lookAt(eye,target)));frame.set(eye,16);device.queue.writeBuffer(this.frameBuffer,0,frame);
        this.encoder=device.createCommandEncoder();
        this.pass=this.encoder.beginRenderPass({colorAttachments:[{view:this.color.createView(),resolveTarget:this.context.getCurrentTexture().createView(),clearValue:{r:.07,g:.11,b:.18,a:1},loadOp:'clear',storeOp:'discard'}],depthStencilAttachment:{view:this.depth.createView(),depthClearValue:1,depthLoadOp:'clear',depthStoreOp:'discard'}});
        this.pass.setPipeline(this.pipeline);this.pass.setBindGroup(0,this.frameGroup);this.drawIndex=0;
    }
    draw(mesh,matrix,color,bones,glow){
        for(const part of mesh.parts){
            let slot=this.slots[this.drawIndex++];
            if(!slot){
                slot={uniform:this.buffer(new Float32Array(24),GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST),bones:this.buffer(new Float32Array(128*16),GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_DST)};
                this.slots.push(slot);
            }
            const values=new Float32Array(24);values.set(matrix);values.set(color||part.color,16);values[20]=part.skinned&&bones?1:0;values[21]=glow;
            this.device.queue.writeBuffer(slot.uniform,0,values);
            if(part.skinned&&bones){if(bones.length%16||bones.length>128*16)throw Error('Invalid bone palette');this.device.queue.writeBuffer(slot.bones,0,bones);}
            if(slot.texture!==part.texture){
                slot.texture=part.texture;slot.group=this.device.createBindGroup({layout:this.pipeline.getBindGroupLayout(1),entries:[
                    {binding:0,resource:{buffer:slot.uniform}},{binding:1,resource:{buffer:slot.bones}},
                    {binding:2,resource:part.texture.createView()},{binding:3,resource:this.sampler}]});
            }
            this.pass.setBindGroup(1,slot.group);this.pass.setVertexBuffer(0,part.vertex);this.pass.setIndexBuffer(part.index,'uint32');this.pass.drawIndexed(part.count);
        }
    }
    endFrame(){this.pass.end();this.device.queue.submit([this.encoder.finish()]);this.frames++;}
    diagnostics(){return {backend:'webgpu',skinning:'storage-buffer',size:[this.canvas.width,this.canvas.height],frames:this.frames,supportsReadback:false,pixels:[],error:this.failure?.message||null};}
    dispose(){
        if(this.disposed)return;this.disposed=true;this.device.removeEventListener('uncapturederror',this.onError);
        this.context?.unconfigure();this.depth?.destroy();this.color?.destroy();for(const resource of this.resources)resource.destroy();this.resources.clear();this.device.destroy();
    }
}
