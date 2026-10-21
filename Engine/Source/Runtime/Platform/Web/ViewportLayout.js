// One coordinate transform for drawing and hit testing in either orientation.
export function viewportLayout(viewport,{landscapeHeight=540,portraitWidth=390}={}) {
    if(!(landscapeHeight>0)||!(portraitWidth>0))throw Error('Invalid UI design size');
    const left=viewport.left||0,top=viewport.top||0;
    const width=Math.max(1,viewport.width-left-(viewport.right||0));
    const height=Math.max(1,viewport.height-top-(viewport.bottom||0));
    const landscape=width>height,scale=landscape?height/landscapeHeight:width/portraitWidth;
    return {left,top,scale,landscape,width:width/scale,height:height/scale,
        // Same coordinate space: decorations fill the screen, controls use safe bounds.
        fullRect:{x:-left/scale,y:-top/scale,width:viewport.width/scale,height:viewport.height/scale},
        safeRect:{x:0,y:0,width:width/scale,height:height/scale},
        point:(x,y)=>({x:(x-left)/scale,y:(y-top)/scale}),
        paint:(ctx,paint)=>{ctx.save();try{ctx.translate(left,top);ctx.scale(scale,scale);return paint();}finally{ctx.restore();}},
        apply:ctx=>{ctx.translate(left,top);ctx.scale(scale,scale);}};
}
