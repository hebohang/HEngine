// Reusable canvas loading presentation; branding and images belong to projects.
export class LoadingScreen {
    constructor(title){this.title=title;this.message='准备加载';this.progress=0;this.background=null;}
    update(message,progress){this.message=message;this.progress=Math.max(this.progress,Math.min(1,progress));}
    paint(ctx,width,height){
        ctx.fillStyle='#10283b';ctx.fillRect(0,0,width,height);
        if(this.background){const image=this.background,scale=Math.max(width/image.width,height/image.height);ctx.drawImage(image,(width-image.width*scale)/2,(height-image.height*scale)/2,image.width*scale,image.height*scale);}
        ctx.fillStyle='rgba(8,20,38,0.65)';ctx.fillRect(0,height*.62,width,height*.38);
        ctx.fillStyle='#ffe5a0';ctx.textAlign='center';ctx.font='bold 26px sans-serif';ctx.fillText(this.title,width/2,height*.72);
        ctx.font='14px sans-serif';ctx.fillStyle='#ffffff';ctx.fillText(this.message,width/2,height*.78);
        const x=width*.12,y=height*.82,w=width*.76;ctx.fillStyle='#304b66';ctx.fillRect(x,y,w,10);ctx.fillStyle='#66d2c8';ctx.fillRect(x,y,w*this.progress,10);
        ctx.font='12px sans-serif';ctx.fillText(Math.round(this.progress*100)+'%',width/2,y+32);ctx.textAlign='start';
    }
}
