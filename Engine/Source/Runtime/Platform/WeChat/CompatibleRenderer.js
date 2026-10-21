import {multiply,lookAt} from '../Web/SceneRenderer.js';
import {frustumPlanes} from './Visibility.js';

// Keep WeChat's established CPU-skinning / explicit-attribute drawElements path.
// Shader compilation alone is not evidence that instancing renders on a phone.
export function withWeChatDepth(Base) {
    return class extends Base {
        constructor(canvas,host){
            super(canvas,host);
            this.sortOpaque=this.cpuSkinning&&this.compatibilityGeometry;
        }
        beginFrame(eye,target) {
            super.beginFrame(eye,target);
            const near=.75,far=180,aspect=this.canvas.width/this.canvas.height,f=1/Math.tan(Math.PI/7);
            const projection=new Float32Array([f/aspect,0,0,0,0,f,0,0,0,0,(far+near)/(near-far),-1,0,0,2*far*near/(near-far),0]);
            const viewProjection=multiply(projection,lookAt(eye,target));
            this.gl.uniformMatrix4fv(this.uniforms.ViewProjection,false,viewProjection);
            this.viewPlanes=frustumPlanes(viewProjection);
            this.opaqueGroups=new Map();this.materialBinds=0;this.skinUpdates=0;
        }
        draw(mesh,matrix,color=null,bones=null,glow=0) {
            if(this.sortOpaque&&(color?.[3]??1)>=1&&mesh.parts.every(p=>p.color[3]>=1)){
                let groups=this.opaqueGroups.get(mesh);
                if(!groups){groups=[];this.opaqueGroups.set(mesh,groups);}
                const key=(color?color.join(','):'material')+':'+glow;
                let group=groups.find(g=>g.bones===bones&&g.key===key);
                if(!group){group={mesh,bones,color,glow,key,matrices:[]};groups.push(group);}
                group.matrices.push(matrix);return;
            }
            // Flush before transparency to preserve the existing painter order.
            this.flushOpaque();
            const gl=this.gl,bias=this.depthBiasForMesh?.(mesh)||0,cullBack=this.cullBackFacesForMesh?.(mesh);
            if(bias){gl.enable(gl.POLYGON_OFFSET_FILL);gl.polygonOffset(-1,bias);}
            if(cullBack){gl.enable(gl.CULL_FACE);gl.cullFace(gl.BACK);}
            try{super.draw(mesh,matrix,color,bones,glow);this.materialBinds+=mesh.parts.length;}
            finally{if(bias)gl.disable(gl.POLYGON_OFFSET_FILL);if(cullBack)gl.disable(gl.CULL_FACE);}
        }
        prepareSkin(part,bones){
            if(!part.skinned||part.lastBones===bones)return;
            let positions=part.position,normals=part.normal;
            if(bones){
                positions=part.reusablePositions||(part.reusablePositions=new Float32Array(part.position.length));
                normals=part.reusableNormals||(part.reusableNormals=new Float32Array(part.normal.length));
                positions.fill(0);normals.fill(0);
                for(let v=0;v<positions.length/3;v++)for(let influence=0;influence<4;influence++){
                    const weight=part.weights[v*4+influence],joint=part.joints[v*4+influence];if(weight<=0||joint<0)continue;
                    const offset=joint*16,p=v*3;
                    for(let row=0;row<3;row++){
                        positions[p+row]+=weight*(bones[offset+row]*part.position[p]+bones[offset+4+row]*part.position[p+1]+bones[offset+8+row]*part.position[p+2]+bones[offset+12+row]);
                        normals[p+row]+=weight*(bones[offset+row]*part.normal[p]+bones[offset+4+row]*part.normal[p+1]+bones[offset+8+row]*part.normal[p+2]);
                    }
                }
            }
            const gl=this.gl;
            for(const attribute of part.buffers)if(attribute.location<2){
                gl.bindBuffer(gl.ARRAY_BUFFER,attribute.buffer);gl.bufferSubData(gl.ARRAY_BUFFER,0,attribute.location===0?positions:normals);
            }
            part.lastBones=bones;this.skinUpdates++;
        }
        flushOpaque(){
            if(!this.opaqueGroups?.size)return;
            const gl=this.gl,u=this.uniforms;gl.bindVertexArray(null);gl.activeTexture(gl.TEXTURE0);
            for(const groups of this.opaqueGroups.values())for(const group of groups){
                const bias=this.depthBiasForMesh?.(group.mesh)||0,cullBack=this.cullBackFacesForMesh?.(group.mesh);
                if(bias){gl.enable(gl.POLYGON_OFFSET_FILL);gl.polygonOffset(-1,bias);}
                if(cullBack){gl.enable(gl.CULL_FACE);gl.cullFace(gl.BACK);}
                gl.uniform1f(u.Glow,group.glow);
                for(const part of group.mesh.parts){
                    this.prepareSkin(part,group.bones);
                    for(let location=0;location<5;location++)gl.disableVertexAttribArray(location);
                    for(const attribute of part.buffers){
                        gl.bindBuffer(gl.ARRAY_BUFFER,attribute.buffer);gl.enableVertexAttribArray(attribute.location);
                        gl.vertexAttribPointer(attribute.location,attribute.size,gl.FLOAT,false,0,0);
                    }
                    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,part.ib);gl.bindTexture(gl.TEXTURE_2D,part.texture);
                    gl.uniform4fv(u.Color,group.color||part.color);this.materialBinds++;
                    for(const matrix of group.matrices){
                        gl.uniformMatrix4fv(u.Model,false,matrix);
                        gl.drawElements(gl.TRIANGLES,part.count,part.indexType,0);this.drawCalls++;
                    }
                }
                if(bias)gl.disable(gl.POLYGON_OFFSET_FILL);
                if(cullBack)gl.disable(gl.CULL_FACE);
            }
            this.opaqueGroups.clear();
        }
        endFrame(){this.flushOpaque();super.endFrame();}
        diagnostics(){
            return {...super.diagnostics(),instancing:false,materialBinds:this.materialBinds||0,skinUpdates:this.skinUpdates||0,depthBits:this.gl.getParameter(this.gl.DEPTH_BITS),nearPlane:.75};
        }
    };
}
