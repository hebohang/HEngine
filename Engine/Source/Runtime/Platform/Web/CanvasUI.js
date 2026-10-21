// Canvas backend primitives. Projects provide styles and opaque action/value data.
export function hitTest(targets,x,y) {
    // Last drawn target is on top. Return one target, never dispatch overlapping actions.
    for(let i=targets.length-1;i>=0;i--){const b=targets[i];if(x>=b.x&&y>=b.y&&x<=b.x+b.w&&y<=b.y+b.h)return b;}
    return undefined;
}

export class CanvasUI {
    constructor(context){this.context=context;this.targets=[];}
    panel(x,y,width,height,color,radius=0){
        const ctx=this.context,r=Math.max(0,Math.min(radius,width/2,height/2));
        ctx.fillStyle=color;ctx.beginPath();ctx.moveTo(x+r,y);
        ctx.arcTo(x+width,y,x+width,y+height,r);ctx.arcTo(x+width,y+height,x,y+height,r);
        ctx.arcTo(x,y+height,x,y,r);ctx.arcTo(x,y,x+width,y,r);ctx.fill();
    }
    text(value,x,y,{size=16,color='#fff',font='sans-serif',maxWidth}={}){
        const ctx=this.context;ctx.fillStyle=color;ctx.font=`${size}px ${font}`;
        if(maxWidth===undefined)ctx.fillText(String(value),x,y);
        else ctx.fillText(String(value),x,y,maxWidth);
    }
    imageCover(image,{x,y,width,height}){
        if(!image||!(image.width>0&&image.height>0))return;
        const scale=Math.max(width/image.width,height/image.height),ctx=this.context;
        ctx.save();try{ctx.beginPath();ctx.rect(x,y,width,height);ctx.clip();
            ctx.drawImage(image,x+(width-image.width*scale)/2,y+(height-image.height*scale)/2,image.width*scale,image.height*scale);
        }finally{ctx.restore();}
    }
    button(label,bounds,{action,value='',hitBounds=bounds,background='#fff',radius=0,textColor='#000',fontSize=14,padding=12}={}){
        this.panel(bounds.x,bounds.y,bounds.w,bounds.h,background,radius);
        this.text(label,bounds.x+padding,bounds.y+bounds.h/2+fontSize*.36,{size:fontSize,color:textColor,maxWidth:Math.max(1,bounds.w-padding*2)});
        this.targets.push({...hitBounds,action,value});
    }
}
