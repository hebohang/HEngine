export function readViewport(wx) {
    const v=wx.getWindowInfo(),safe=v.safeArea||{left:0,top:0,right:v.windowWidth,bottom:v.windowHeight};
    const menu=typeof wx.getMenuButtonBoundingClientRect==='function'?wx.getMenuButtonBoundingClientRect():null;
    return {width:v.windowWidth,height:v.windowHeight,ratio:v.pixelRatio||1,left:safe.left||0,
        right:Math.max(0,v.windowWidth-safe.right),top:Math.max(safe.top||0,menu?menu.bottom+4:0),
        bottom:Math.max(0,v.windowHeight-safe.bottom)};
}
export async function applyOrientation(wx,orientation){
    if(!['portrait','landscape'].includes(orientation)||typeof wx.setDeviceOrientation!=='function')return;
    await new Promise(resolve=>{
        const timer=setTimeout(resolve,2500);
        wx.setDeviceOrientation({value:orientation,complete:()=>{clearTimeout(timer);resolve();},fail:error=>console.warn('[HE orientation]',error.errMsg)});
    });
}
