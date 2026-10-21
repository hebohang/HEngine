// Logical viewport coordinates; matching the native mobile control proportions.
export function touchLayout(width,height) {
    const radius=Math.min(width,height)*.12;
    return {
        radius,stick:{x:width*.16,y:height*.78},
        jump:{x:width*.87,y:height*.78,radius:radius*.8},
        pause:{x:width*.94,y:height*.09,radius:radius*.6},
        isMovement:(x,y)=>x<width*.4&&y>height*.45,
        isLook:x=>x>=width*.4
    };
}
