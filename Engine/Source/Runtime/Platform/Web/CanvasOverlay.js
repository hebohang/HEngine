// Paint host UI to an offscreen canvas, then composite over the 3D frame.
export class CanvasOverlay {
    constructor(gl,canvas,{pixelUpload=false,maxUpdateHz=0}={}) {
        this.gl=gl;this.canvas=canvas;this.context=canvas.getContext('2d',{alpha:true});this.pixelUpload=pixelUpload;
        this.updateInterval=maxUpdateHz>0?1000/maxUpdateHz:0;this.lastUpdate=-Infinity;
        const shader=(type,source)=>{const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(s));return s;};
        this.program=gl.createProgram();
        gl.attachShader(this.program,shader(gl.VERTEX_SHADER,'#version 300 es\nconst vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));out vec2 uv;void main(){vec2 v=p[gl_VertexID];uv=vec2((v.x+1.)*.5,1.-(v.y+1.)*.5);gl_Position=vec4(v,0,1);}'));
        gl.attachShader(this.program,shader(gl.FRAGMENT_SHADER,'#version 300 es\nprecision mediump float;in vec2 uv;uniform sampler2D image;out vec4 color;void main(){color=texture(image,uv);}'));
        gl.linkProgram(this.program);if(!gl.getProgramParameter(this.program,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(this.program));
        this.texture=gl.createTexture();this.vao=gl.createVertexArray();
        gl.bindTexture(gl.TEXTURE_2D,this.texture);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
    }
    invalidate(){this.lastUpdate=-Infinity;}
    // Restricted host canvases (e.g. WeChat open-data) must go directly to WebGL.
    drawCanvas(canvas,x,y,width,height,viewportWidth,viewportHeight){
        const gl=this.gl;
        if(!this.layerTextures)this.layerTextures=new WeakMap();
        let texture=this.layerTextures.get(canvas);
        gl.activeTexture(gl.TEXTURE0);
        if(!texture){texture=gl.createTexture();this.layerTextures.set(canvas,texture);gl.bindTexture(gl.TEXTURE_2D,texture);
            gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
            gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);}
        gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.useProgram(this.program);gl.disable(gl.DEPTH_TEST);gl.disable(gl.SCISSOR_TEST);gl.disable(gl.CULL_FACE);
        gl.enable(gl.BLEND);gl.blendEquation(gl.FUNC_ADD);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);gl.colorMask(true,true,true,true);
        gl.bindTexture(gl.TEXTURE_2D,texture);gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,false);gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL,false);
        gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,canvas);
        const sx=gl.drawingBufferWidth/viewportWidth,sy=gl.drawingBufferHeight/viewportHeight;
        gl.viewport(Math.round(x*sx),Math.round((viewportHeight-y-height)*sy),Math.round(width*sx),Math.round(height*sy));
        gl.bindVertexArray(this.vao);gl.drawArrays(gl.TRIANGLES,0,3);
        const error=gl.getError();if(error!==gl.NO_ERROR)throw Error('Canvas layer WebGL error '+error);
    }
    draw(width,height,paint) {
        // Rasterize at presentation resolution, while layout/input keep logical pixels.
        const pixelWidth=Math.max(1,this.gl.drawingBufferWidth),pixelHeight=Math.max(1,this.gl.drawingBufferHeight);
        if(this.canvas.width!==pixelWidth||this.canvas.height!==pixelHeight){this.canvas.width=pixelWidth;this.canvas.height=pixelHeight;this.invalidate();}
        const ctx=this.context,now=Date.now(),update=now-this.lastUpdate>=this.updateInterval;
        if(update){ctx.setTransform(1,0,0,1,0,0);ctx.clearRect(0,0,pixelWidth,pixelHeight);ctx.save();try{ctx.scale(pixelWidth/width,pixelHeight/height);paint(ctx,width,height);}finally{ctx.restore();}this.lastUpdate=now;}
        // Each pass owns its state. Mobile canvas uploads must not turn the
        // transparent HUD into an opaque full-screen rectangle.
        const gl=this.gl;gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.viewport(0,0,gl.drawingBufferWidth,gl.drawingBufferHeight);
        gl.useProgram(this.program);gl.disable(gl.DEPTH_TEST);gl.disable(gl.CULL_FACE);gl.disable(gl.SCISSOR_TEST);
        gl.colorMask(true,true,true,true);gl.enable(gl.BLEND);gl.blendEquation(gl.FUNC_ADD);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);
        gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,this.texture);
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,false);gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL,false);
        if(update){
            if(this.pixelUpload){const pixels=ctx.getImageData(0,0,pixelWidth,pixelHeight).data;gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,pixelWidth,pixelHeight,0,gl.RGBA,gl.UNSIGNED_BYTE,pixels);}
            else gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,this.canvas);
        }
        gl.bindVertexArray(this.vao);gl.drawArrays(gl.TRIANGLES,0,3);
        const error=gl.getError();if(error!==gl.NO_ERROR)throw Error('UI composition WebGL error '+error);
    }
}
