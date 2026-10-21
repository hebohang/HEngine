import {createBrowserHost} from './BrowserHost.js';

// Try WebGPU on a fresh canvas: canvas context types cannot be switched after
// getContext succeeds. Failed probes therefore never poison the WebGL canvas.
export async function createBrowserRenderer(Renderer,canvas,{backend='auto',maxPixelRatio=3,...options}={}){
    if(!['auto','webgpu','webgl2'].includes(backend))throw Error('Unknown browser renderer: '+backend);
    let fallbackReason=null;
    if(backend!=='webgl2'){
        let graphics;
        try{
            const {WebGPUBackend}=await import('./WebGPUBackend.js');
            const candidate=canvas.cloneNode(false);
            graphics=await WebGPUBackend.create(candidate);
            const renderer=new Renderer(candidate,{...options,...createBrowserHost(candidate,{maxPixelRatio}),graphics});
            canvas.replaceWith(candidate);return renderer;
        }catch(error){graphics?.dispose();if(backend==='webgpu')throw error;fallbackReason=String(error.message||error);}
    }
    const renderer=new Renderer(canvas,{...options,...createBrowserHost(canvas,{maxPixelRatio})});renderer.fallbackReason=fallbackReason;return renderer;
}

export async function recoverBrowserRenderer(Renderer,renderer,initialize,reason){
    if(!renderer.graphics)throw reason;
    const canvas=renderer.canvas.cloneNode(false);renderer.canvas.replaceWith(canvas);renderer.graphics.dispose();
    const replacement=await createBrowserRenderer(Renderer,canvas,{backend:'webgl2',maxPixelRatio:renderer.maxPixelRatio});
    replacement.fallbackReason=String(reason.message||reason);await initialize(replacement);return replacement;
}
