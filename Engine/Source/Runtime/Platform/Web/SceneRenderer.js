import {InterestRegion} from './InterestRegion.js';
// Web platform rendering adapter. Gameplay state is supplied by the C# runtime.
const identity = () => new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]);
export function multiply(a,b) {
    const o = new Float32Array(16);
    for(let c=0;c<4;c++) for(let r=0;r<4;r++) for(let k=0;k<4;k++) o[c*4+r]+=a[k*4+r]*b[c*4+k];
    return o;
}
export function quaternion(x,y,z) {
    const cx=Math.cos(x/2),sx=Math.sin(x/2),cy=Math.cos(y/2),sy=Math.sin(y/2),cz=Math.cos(z/2),sz=Math.sin(z/2);
    return [sx*cy*cz-cx*sy*sz,cx*sy*cz+sx*cy*sz,cx*cy*sz-sx*sy*cz,cx*cy*cz+sx*sy*sz];
}
export function trs(t,q,s) {
    const [x,y,z,w]=q,x2=x+x,y2=y+y,z2=z+z,xx=x*x2,xy=x*y2,xz=x*z2,yy=y*y2,yz=y*z2,zz=z*z2,wx=w*x2,wy=w*y2,wz=w*z2;
    return new Float32Array([(1-yy-zz)*s[0],(xy+wz)*s[0],(xz-wy)*s[0],0,(xy-wz)*s[1],(1-xx-zz)*s[1],(yz+wx)*s[1],0,(xz+wy)*s[2],(yz-wx)*s[2],(1-xx-yy)*s[2],0,...t,1]);
}
function transform(component={}) {return trs(component.Translation||[0,0,0],quaternion(...(component.Rotation||[0,0,0])),component.Scale||[1,1,1]);}
export function lookAt(eye,target) {
    const norm=v=>{const n=Math.hypot(...v);return v.map(x=>x/n);};
    const z=norm(eye.map((v,i)=>v-target[i])),x=norm([z[2],0,-z[0]]),y=[z[1]*x[2]-z[2]*x[1],z[2]*x[0]-z[0]*x[2],z[0]*x[1]-z[1]*x[0]];
    return new Float32Array([x[0],y[0],z[0],0,x[1],y[1],z[1],0,x[2],y[2],z[2],0,-x.reduce((v,n,i)=>v+n*eye[i],0),-y.reduce((v,n,i)=>v+n*eye[i],0),-z.reduce((v,n,i)=>v+n*eye[i],0),1]);
}
export function perspective(aspect) {const f=1/Math.tan(Math.PI/7),n=.1,z=220;return new Float32Array([f/aspect,0,0,0,0,f,0,0,0,0,(z+n)/(n-z),-1,0,0,2*z*n/(n-z),0]);}
const clean=p=>p.replaceAll('\\','/');
const vs=`#version 300 es
precision highp float;
layout(location=0) in vec3 aPosition;layout(location=1) in vec3 aNormal;layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aJoints;layout(location=4) in vec4 aWeights;
uniform mat4 uViewProjection,uModel;uniform bool uSkinned;
uniform highp sampler2D uBones;
mat4 bone(int i){return mat4(texelFetch(uBones,ivec2(0,i),0),texelFetch(uBones,ivec2(1,i),0),texelFetch(uBones,ivec2(2,i),0),texelFetch(uBones,ivec2(3,i),0));}
out vec2 vUV;out vec3 vNormal;out vec3 vPosition;
void main(){mat4 skin=mat4(1.0);if(uSkinned){skin=mat4(0.0);for(int i=0;i<4;i++)if(aWeights[i]>0.0&&aJoints[i]>=0.0)skin+=bone(int(aJoints[i]))*aWeights[i];}
vec4 p=uModel*skin*vec4(aPosition,1.0);vPosition=p.xyz;vNormal=normalize(mat3(uModel)*mat3(skin)*aNormal);vUV=aUV;gl_Position=uViewProjection*p;}`;
const fs=`#version 300 es
precision highp float;in vec2 vUV;in vec3 vNormal;in vec3 vPosition;
uniform sampler2D uTexture;uniform vec4 uColor;uniform vec3 uCamera;uniform float uGlow;
out vec4 color;void main(){vec4 c=texture(uTexture,vUV)*uColor;if(c.a<0.1)discard;
float light=0.58+0.42*max(dot(normalize(vNormal),normalize(vec3(-0.3,0.8,0.5))),0.0);
vec3 rgb=c.rgb*light+uGlow*c.rgb;float fog=smoothstep(45.0,130.0,distance(uCamera,vPosition));color=vec4(mix(rgb,vec3(0.10,0.16,0.24),fog),c.a);}`;

export class SceneRenderer {
    constructor(canvas, host = {}) {
        this.fetch=host.fetch||globalThis.fetch.bind(globalThis);
        this.loadImage=host.loadImage||(async path=>{const r=await this.fetch(path);if(!r.ok)throw Error("Texture missing: "+path);return createImageBitmap(await r.blob());});
        this.maxPixelRatio=host.maxPixelRatio||1.5;
        this.viewport=host.viewport||(()=>({width:canvas.clientWidth,height:canvas.clientHeight,ratio:globalThis.devicePixelRatio||1}));
        this.requests=new Map();this.interest=new InterestRegion(host.interest);this.platform=host.platform||'Web';this.pendingModels=new Map();this.streamError=null;
        this.meshes=new Map();this.textures=new Map();this.animations=new Map();this.scene=[];this.frames=0;this.drawCalls=0;
        this.canvas=canvas;this.graphics=host.graphics||null;
        if(this.graphics){this.white=this.graphics.white;this.primitive=this.makePrimitive();return;}
        this.canvas=canvas;this.gl=canvas.getContext('webgl2',{alpha:false,antialias:true,preserveDrawingBuffer:true});
        if(!this.gl)throw Error('此设备不支持 WebGL 2');
        const gl=this.gl;
        this.supportsReadback=host.supportsReadback!==false;
        this.compatibilityGeometry=!!host.compatibilityGeometry;
        this.capabilities={vertexUniformVectors:gl.getParameter(gl.MAX_VERTEX_UNIFORM_VECTORS),vertexTextureUnits:gl.getParameter(gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS),maxTextureSize:gl.getParameter(gl.MAX_TEXTURE_SIZE)};
        this.cpuSkinning=this.compatibilityGeometry||this.capabilities.vertexTextureUnits<1||this.capabilities.maxTextureSize<128;
        this.skinning=this.cpuSkinning?'cpu':'bone-texture';
        const vertexSource=this.cpuSkinning?vs.replace(/uniform highp sampler2D uBones;[\s\S]*?\n(?=out vec2)/,'').replace(/if\(uSkinned\)\{skin=mat4\(0.0\);[^\n]+?;\}/,''):vs;
        const shader=(type,source)=>{const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(s));return s;};
        this.program=gl.createProgram();gl.attachShader(this.program,shader(gl.VERTEX_SHADER,vertexSource));gl.attachShader(this.program,shader(gl.FRAGMENT_SHADER,fs));gl.linkProgram(this.program);
        if(!gl.getProgramParameter(this.program,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(this.program));
        this.uniforms=Object.fromEntries(['ViewProjection','Model','Bones','Skinned','Color','Camera','Glow','Texture'].map(n=>[n,gl.getUniformLocation(this.program,'u'+n)]));
        if(!this.cpuSkinning){
            this.boneTexture=gl.createTexture();gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,this.boneTexture);
            gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA32F,4,128,0,gl.RGBA,gl.FLOAT,null);
            gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);gl.activeTexture(gl.TEXTURE0);
        }
        this.white=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,this.white);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([255,255,255,255]));
        this.primitive=this.makePrimitive();
        gl.enable(gl.DEPTH_TEST);gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);
    }
    json(path) {path=clean(path);if(!this.requests.has(path))this.requests.set(path,this.fetch(path).then(r=>{if(!r.ok)throw Error("Asset load failed: "+path);return r.json();}));return this.requests.get(path);}
    async texture(path) {
        if(!path||!path.startsWith('Assets'))return this.white;
        path=clean(path);
        if(!this.textures.has(path))this.textures.set(path,(async()=>{
            const bitmap=await this.loadImage(path);
            if(this.graphics){try{return this.graphics.texture(bitmap);}finally{bitmap.close?.();}}
            const gl=this.gl,t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);
            gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,bitmap);if(bitmap.close)bitmap.close();
            gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR_MIPMAP_LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
            gl.generateMipmap(gl.TEXTURE_2D);return t;
        })());return this.textures.get(path);
    }
    async loadMesh(path) {
        path=clean(path);if(this.meshes.has(path))return this.meshes.get(path);
        const task=(async()=>{
            const mesh=await this.json(path),buffers=await Promise.all(mesh.bufferVec.map(async b=>{const r=await this.fetch(clean(b.uri));if(!r.ok)throw Error('Mesh buffer missing: '+b.uri);return r.arrayBuffer();}));
            const accessor=index=>{
                const a=mesh.accessorVec[index],v=mesh.bufferViewVec[a.bufferViewIndex],size={SCALAR:1,VEC2:2,VEC3:3,VEC4:4}[a.type];
                const C={5126:Float32Array,5125:Uint32Array,5123:Uint16Array,5121:Uint8Array}[a.componentType];
                if(!C||!size)throw Error('Unsupported mesh accessor');
                const offset=(v.byteOffset||0)+(a.byteOffset||0),stride=v.byteStride||size*C.BYTES_PER_ELEMENT;
                if(stride===size*C.BYTES_PER_ELEMENT)return new C(buffers[v.bufferIndex],offset,a.count*size);
                const result=new C(a.count*size);for(let i=0;i<a.count;i++)result.set(new C(buffers[v.bufferIndex],offset+i*stride,size),i*size);return result;
            };
            const parts=[];
            for(const p of mesh.primitives){
                const attributes=p.attributes,position=accessor(attributes.POSITION),normal=attributes.NORMAL===undefined?new Float32Array(position.length).fill(1):accessor(attributes.NORMAL);
                const uv=attributes.TEXCOORD_0===undefined?new Float32Array(position.length/3*2):accessor(attributes.TEXCOORD_0);
                const joints=attributes.JOINTS_0===undefined?null:accessor(attributes.JOINTS_0),weights=attributes.WEIGHTS_0===undefined?null:accessor(attributes.WEIGHTS_0);
                let color=[1,1,1,1],texture=this.white;
                if(p.materialResPath){const mat=await this.json(p.materialResPath),props=mat.Properties||{},pbr=props.pbrMetallicRoughness||{};
                    color=pbr.baseColorFactor||color;texture=await this.texture(props.ExposedProperties?.g_AlbedoMap||pbr.baseColorTexture?.textureRes?.textureSourcePath);}
                parts.push({...this.upload(position,normal,uv,accessor(p.indices),joints,weights),color,texture});
            }
            return {parts,path};
        })();this.meshes.set(path,task);return task;
    }
    upload(position,normal,uv,indices,joints=null,weights=null){
        if(this.graphics)return this.graphics.upload(position,normal,uv,indices,joints,weights);
        const gl=this.gl,vao=gl.createVertexArray(),buffers=[];gl.bindVertexArray(vao);
        for(const [location,data,size] of [[0,position,3],[1,normal,3],[2,uv,2],[3,joints,4],[4,weights,4]]){
            if(!data){gl.disableVertexAttribArray(location);continue;}
            const b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(data),gl.STATIC_DRAW);gl.enableVertexAttribArray(location);gl.vertexAttribPointer(location,size,gl.FLOAT,false,0,0);
            buffers.push({location,buffer:b,size});
        }
        const shortIndices=indices.every(index=>index<=65535),ib=gl.createBuffer();gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,ib);gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,shortIndices?new Uint16Array(indices):new Uint32Array(indices),gl.STATIC_DRAW);
        gl.bindVertexArray(null);return {vao,buffers,ib,indexType:shortIndices?gl.UNSIGNED_SHORT:gl.UNSIGNED_INT,count:indices.length,skinned:!!joints,position,normal,joints,weights};
    }
    makePrimitive(){
        const p=[],n=[],uv=[],indices=[];
        const vertices=[[0,1,0],[1,0,0],[0,0,1],[-1,0,0],[0,0,-1],[0,-1,0]];
        for(const face of [[0,1,2],[0,2,3],[0,3,4],[0,4,1],[5,2,1],[5,3,2],[5,4,3],[5,1,4]])for(const index of face){p.push(...vertices[index]);n.push(...vertices[index]);uv.push(0,0);indices.push(indices.length);}
        return {parts:[{...this.upload(p,n,uv,indices),color:[1,1,1,1],texture:this.white}]};
    }
    async initialize({scenePath, models, excludeTags=[]},progress=()=>{}){
        const manifest=await this.json('content-manifest.json');this.assetNames=Object.keys(manifest.assets);
        const scene=await this.json(scenePath);const matrixCache=new Map();
        const world=id=>{if(matrixCache.has(id))return matrixCache.get(id);const t=scene[id]?.TransformComponent||{};let m=transform(t);if(t.ParentUuid&&scene[t.ParentUuid])m=multiply(world(t.ParentUuid),m);matrixCache.set(id,m);return m;};
        const sources=Object.entries(scene).filter(([id,e])=>e.MeshComponent&&!excludeTags.includes(e.TagComponent?.Tag)&&e.EditorEntityStateComponent?.bVisible!==false);
        for(let i=0;i<sources.length;i++){
            const [id,e]=sources[i],path=e.MeshComponent.Path||e.MeshComponent.meshResPath;
            const matrix=world(id),near=this.interest.contains(matrix[12],matrix[14],0,0,this.interest.padding);
            this.scene.push({path,mesh:near?await this.loadMesh(path):null,matrix});if(i%20===0)progress('加载原场景 '+Math.round(i/sources.length*100)+'%');
        }
        this.models=new Map();this.modelDefinitions=new Map(models.map(definition=>[definition.id,definition]));
        for(const definition of (this.interest.enabled?models.slice(0,1):models)){
            progress('加载角色 '+(definition.name?.zh_cn||definition.id));await this.ensureModel(definition.id);
        }
    }
    ensureModel(id){
        if(this.models.has(id))return Promise.resolve(this.models.get(id));
        if(this.pendingModels.has(id))return this.pendingModels.get(id);
        const definition=this.modelDefinitions.get(id);if(!definition)return Promise.resolve(null);
        const task=(async()=>{
            const mesh=await this.loadMesh(definition.mesh),folder=clean(definition.mesh).slice(0,clean(definition.mesh).lastIndexOf('/')+1);
            const names=this.assetNames.filter(p=>p.startsWith(folder)&&p.endsWith('.hanim'));
            const idle=definition.idle||names.find(p=>/idle/i.test(p)),run=names.find(p=>/run|walk/i.test(p))||idle;
            const model={mesh,scale:definition.scale,idle:idle?await this.json(idle):null,run:run?await this.json(run):null,upright:definition.preview?.upright||0};
            this.models.set(id,model);return model;
        })();this.pendingModels.set(id,task);return task;
    }
    streamScene(x,z){
        if(this.streamError)throw this.streamError;
        let started=0;
        for(const item of this.scene){
            if(item.mesh||item.pending||!this.interest.contains(item.matrix[12],item.matrix[14],x,z,this.interest.padding))continue;
            item.pending=true;this.loadMesh(item.path).then(mesh=>{item.mesh=mesh;}).catch(error=>{this.streamError=error;});
            if(++started>=4)break;
        }
    }
    bones(animation,seconds){
        if(!animation)return null;const clip=animation.clips[0],time=seconds*(clip.ticksPerSecond||30)%Math.max(.001,clip.duration);
        const channels=new Map(clip.channels.map(c=>[c.name,c])),infos=new Map(animation.boneInfos.map(b=>[b.name,b]));
        const result=new Float32Array(128*16);for(let i=0;i<128;i++)result.set(identity(),i*16);
        const sample=(keys,fields,fallback,rotation=false)=>{
            if(!keys?.length)return fallback;let a=keys[0],b=a;for(let i=1;i<keys.length;i++){b=keys[i];if(time<=b.timeStamp)break;a=b;}
            const f=a===b?0:Math.max(0,Math.min(1,(time-a.timeStamp)/(b.timeStamp-a.timeStamp)));
            let av=fields.map(k=>a[k]),bv=fields.map(k=>b[k]);if(rotation&&av.reduce((s,v,i)=>s+v*bv[i],0)<0)bv=bv.map(v=>-v);
            let v=av.map((x,i)=>x+(bv[i]-x)*f);if(rotation){const length=Math.hypot(...v);v=v.map(x=>x/length);}return v;
        };
        const visit=(node,parent)=>{
            const c=channels.get(node.name);let local=node.localTransform;
            if(c)local=trs(sample(c.positionKeys,['px','py','pz'],[0,0,0]),sample(c.rotationKeys,['qx','qy','qz','qw'],[0,0,0,1],true),sample(c.scaleKeys,['sx','sy','sz'],[1,1,1]));
            const global=multiply(parent,local);const bone=infos.get(node.name);
            if(bone){if(bone.id>=128)throw Error('Skeleton exceeds supported bone count');result.set(multiply(global,bone.offsetMatrix),bone.id*16);}
            for(const child of node.children)visit(child,global);
        };visit(animation.rootNode,identity());return result;
    }
    draw(mesh,matrix,color=null,bones=null,glow=0){
        if(this.graphics){this.graphics.draw(mesh,matrix,color,bones,glow);this.drawCalls+=mesh.parts.length;return;}
        const gl=this.gl,u=this.uniforms;gl.uniformMatrix4fv(u.Model,false,matrix);gl.uniform1f(u.Glow,glow);
        for(const part of mesh.parts){
            if(!this.cpuSkinning){
                gl.uniform1i(u.Skinned,part.skinned&&bones?1:0);
                if(part.skinned&&bones){
                    if(bones.length%16||bones.length>128*16)throw Error('Invalid bone palette');
                    gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,this.boneTexture);
                    gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,4,bones.length/16,gl.RGBA,gl.FLOAT,bones);gl.activeTexture(gl.TEXTURE0);
                }
            }
            else if(part.skinned&&bones&&part.lastBones!==bones){
                const positions=new Float32Array(part.position.length),normals=new Float32Array(part.normal.length);
                for(let v=0;v<positions.length/3;v++)for(let influence=0;influence<4;influence++){
                    const weight=part.weights[v*4+influence],joint=part.joints[v*4+influence];if(weight<=0||joint<0)continue;
                    const offset=joint*16,p=v*3;
                    for(let row=0;row<3;row++){
                        positions[p+row]+=weight*(bones[offset+row]*part.position[p]+bones[offset+4+row]*part.position[p+1]+bones[offset+8+row]*part.position[p+2]+bones[offset+12+row]);
                        normals[p+row]+=weight*(bones[offset+row]*part.normal[p]+bones[offset+4+row]*part.normal[p+1]+bones[offset+8+row]*part.normal[p+2]);
                    }
                }
                for(const attribute of part.buffers)if(attribute.location<2){gl.bindBuffer(gl.ARRAY_BUFFER,attribute.buffer);gl.bufferData(gl.ARRAY_BUFFER,attribute.location===0?positions:normals,gl.DYNAMIC_DRAW);}
                part.lastBones=bones;
            }
            else if(this.cpuSkinning&&part.lastBones&&!bones){
                for(const attribute of part.buffers)if(attribute.location<2){gl.bindBuffer(gl.ARRAY_BUFFER,attribute.buffer);gl.bufferData(gl.ARRAY_BUFFER,attribute.location===0?part.position:part.normal,gl.DYNAMIC_DRAW);}
                part.lastBones=null;
            }
            gl.uniform4fv(u.Color,color||part.color);gl.bindTexture(gl.TEXTURE_2D,part.texture);
            gl.bindVertexArray(this.compatibilityGeometry?null:part.vao);
            if(this.compatibilityGeometry){for(let location=0;location<5;location++)gl.disableVertexAttribArray(location);for(const attribute of part.buffers){gl.bindBuffer(gl.ARRAY_BUFFER,attribute.buffer);gl.enableVertexAttribArray(attribute.location);gl.vertexAttribPointer(attribute.location,attribute.size,gl.FLOAT,false,0,0);}gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,part.ib);}
            gl.drawElements(gl.TRIANGLES,part.count,part.indexType,0);this.drawCalls++;}
    }
    diagnostics(){
        if(this.graphics)return {...this.graphics.diagnostics(),drawCalls:this.drawCalls,meshes:this.meshes.size,textures:this.textures.size};
        const gl=this.gl,w=gl.drawingBufferWidth,h=gl.drawingBufferHeight,pixels=[];
        if(this.supportsReadback)for(const [x,y]of [[.25,.25],[.5,.5],[.75,.75]]){const pixel=new Uint8Array(4);gl.readPixels(Math.floor(w*x),Math.floor(h*y),1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);pixels.push(Array.from(pixel));}
        return {backend:'webgl2',skinning:this.skinning,capabilities:this.capabilities,fallbackReason:this.fallbackReason||null,compatibilityGeometry:this.compatibilityGeometry,drawCalls:this.drawCalls,meshes:this.meshes.size,textures:this.textures.size,size:[w,h],supportsReadback:this.supportsReadback,pixels,glError:gl.getError()};
    }
    beginFrame(eye,target){
        if(this.graphics){this.drawCalls=0;this.graphics.beginFrame(eye,target,this.viewport(),this.maxPixelRatio);return;}
        const gl=this.gl,canvas=this.canvas,viewport=this.viewport(),ratio=Math.min(viewport.ratio||1,this.maxPixelRatio),w=Math.max(1,Math.round(viewport.width*ratio)),h=Math.max(1,Math.round(viewport.height*ratio));
        if(canvas.width!==w||canvas.height!==h){canvas.width=w;canvas.height=h;}
        gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.viewport(0,0,w,h);gl.disable(gl.SCISSOR_TEST);gl.disable(gl.CULL_FACE);
        gl.colorMask(true,true,true,true);gl.depthMask(true);gl.enable(gl.DEPTH_TEST);gl.depthFunc(gl.LESS);
        gl.enable(gl.BLEND);gl.blendEquation(gl.FUNC_ADD);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);gl.activeTexture(gl.TEXTURE0);
        gl.clearColor(.07,.11,.18,1);gl.clearDepth(1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);gl.useProgram(this.program);
        gl.uniformMatrix4fv(this.uniforms.ViewProjection,false,multiply(perspective(w/h),lookAt(eye,target)));gl.uniform3fv(this.uniforms.Camera,eye);gl.uniform1i(this.uniforms.Texture,0);this.drawCalls=0;
        if(!this.cpuSkinning)gl.uniform1i(this.uniforms.Bones,1);
    }
    endFrame(){
        if(this.graphics){this.graphics.endFrame();this.frames++;return;}
        this.frames++;const error=this.gl.getError();if(error!==this.gl.NO_ERROR)throw Error("WebGL error "+error);
    }
}
