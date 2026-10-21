// Host events report facts; the project decides whether to pause, save or resume.
export function bindBrowserLifecycle({resetInput=()=>{},onSuspend=()=>{},onHidden=()=>{},onPageHide=()=>{}}={}) {
    const controller=new AbortController(),options={signal:controller.signal};
    window.addEventListener('blur',()=>{resetInput();onSuspend();},options);
    window.addEventListener('resize',resetInput,options);
    document.addEventListener('visibilitychange',()=>{if(document.hidden){resetInput();onSuspend();onHidden();}},options);
    window.addEventListener('pagehide',()=>{resetInput();onPageHide();},options);
    return ()=>controller.abort();
}

export async function toggleFullscreen(element=document.documentElement) {
    if(document.fullscreenElement){await document.exitFullscreen();return false;}
    if(typeof element.requestFullscreen!=='function')throw Error('Fullscreen is unavailable');
    await element.requestFullscreen();return true;
}
