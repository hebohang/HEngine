import {CanvasOverlay} from '../Web/CanvasOverlay.js';

// WeChat needs a pixel upload for the private HUD canvas. Cache it until the
// visible HUD changes instead of reading back a full retina canvas every frame.
export class CachedOverlay extends CanvasOverlay {
    constructor(gl,canvas){
        super(gl,canvas,{pixelUpload:true});
        this.pendingKey='';this.paintedKey=null;this.uploadCount=0;this.uploadBytes=0;
    }
    requestUpdate(key){this.pendingKey=key;}
    draw(width,height,paint){
        const gl=this.gl,pw=gl.drawingBufferWidth,ph=gl.drawingBufferHeight,now=Date.now();
        const resized=this.canvas.width!==pw||this.canvas.height!==ph;
        if(resized){this.canvas.width=pw;this.canvas.height=ph;this.invalidate();}
        const update=this.lastUpdate===-Infinity||(this.pendingKey!==this.paintedKey&&now-this.lastUpdate>=50);
        const ctx=this.context;
        if(update){
            ctx.setTransform(1,0,0,1,0,0);ctx.clearRect(0,0,pw,ph);ctx.save();
            try{ctx.scale(pw/width,ph/height);paint(ctx,width,height);}finally{ctx.restore();}
        }
        gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.viewport(0,0,pw,ph);gl.useProgram(this.program);
        gl.disable(gl.DEPTH_TEST);gl.disable(gl.CULL_FACE);gl.disable(gl.SCISSOR_TEST);
        gl.colorMask(true,true,true,true);gl.enable(gl.BLEND);gl.blendEquation(gl.FUNC_ADD);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);
        gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,this.texture);
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,false);gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL,false);
        if(update){
            const pixels=ctx.getImageData(0,0,pw,ph).data;
            if(!this.allocated||resized)gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,pw,ph,0,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
            else gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,pw,ph,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
            this.allocated=true;this.lastUpdate=now;this.paintedKey=this.pendingKey;this.uploadCount++;this.uploadBytes+=pixels.byteLength;
        }
        gl.bindVertexArray(this.vao);gl.drawArrays(gl.TRIANGLES,0,3);
        const error=gl.getError();if(error!==gl.NO_ERROR)throw Error('WeChat HUD composition WebGL error '+error);
    }
}
