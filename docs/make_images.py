# Draws docs/icon.png and docs/social-preview.png (the GitHub page's icon and
# link-preview image). Needs Pillow and macOS's SF fonts: python3 docs/make_images.py
from PIL import Image, ImageDraw, ImageFont
import math

BG=(10,14,28); PANEL=(16,22,42); BLUE=(40,72,220); BRIGHT=(0,135,255)
YELLOW=(255,221,64); CYAN=(80,220,235); GREEN=(110,235,110); WHITE=(235,240,250); DIM=(120,135,170)
MONO='/System/Library/Fonts/SFNSMono.ttf'; SANS='/System/Library/Fonts/SFNS.ttf'
def font(path,size,bold=False):
    f=ImageFont.truetype(path,size)
    try: f.set_variation_by_name('Bold' if bold else 'Regular')
    except Exception: pass
    return f

def draw_icon(size):
    S=4; W=size*S
    im=Image.new('RGBA',(W,W),(0,0,0,0)); d=ImageDraw.Draw(im)
    r=int(W*0.2)
    d.rounded_rectangle([0,0,W-1,W-1],r,fill=BG)
    # terminal window
    m=int(W*0.12); top=int(W*0.30); bot=int(W*0.88)
    d.rounded_rectangle([m,top,W-m,bot],int(W*0.05),fill=PANEL,outline=BRIGHT,width=int(W*0.012))
    bar=int(W*0.075)
    d.rounded_rectangle([m,top,W-m,top+bar],int(W*0.05),fill=BLUE)
    d.rectangle([m,top+bar//2,W-m,top+bar],fill=BLUE)
    f=font(MONO,int(W*0.058),True)
    d.text((m+int(W*0.035),top+bar//2),'QuickLogger',font=f,fill=YELLOW,anchor='lm')
    # log lines
    lf=font(MONO,int(W*0.062),True)
    rows=[('1','W4KWK',GREEN),('2','AA4FA',CYAN),('3','K4ZZ',WHITE)]
    y=top+bar+int(W*0.07)
    for n,call,col in rows:
        d.text((m+int(W*0.045),y),n,font=lf,fill=YELLOW,anchor='lm')
        d.text((m+int(W*0.13),y),call,font=lf,fill=col,anchor='lm')
        y+=int(W*0.1)
    # cursor
    cx=m+int(W*0.045); cy=y
    d.text((cx,cy),'>',font=lf,fill=YELLOW,anchor='lm')
    d.rectangle([cx+int(W*0.07),cy-int(W*0.03),cx+int(W*0.11),cy+int(W*0.03)],fill=YELLOW)
    # antenna mast + waves, top right
    ax=int(W*0.72); ay=int(W*0.18)
    lw=int(W*0.018)
    d.line([ax,ay,ax,top],fill=WHITE,width=lw)
    d.line([ax-int(W*0.05),top,ax,ay+int(W*0.05)],fill=WHITE,width=lw)
    d.line([ax+int(W*0.05),top,ax,ay+int(W*0.05)],fill=WHITE,width=lw)
    d.ellipse([ax-lw*1.3,ay-lw*1.3,ax+lw*1.3,ay+lw*1.3],fill=YELLOW)
    for i,col in enumerate([CYAN,BRIGHT,BLUE]):
        rr=int(W*(0.06+0.045*i))
        box=[ax-rr,ay-rr,ax+rr,ay+rr]
        d.arc(box,-60,-10,fill=col,width=lw)
        d.arc(box,190,240,fill=col,width=lw)
    return im.resize((size,size),Image.LANCZOS)

icon=draw_icon(512)
icon.save('/Users/weskeene/CLionProjects/QuickLogger/docs/icon.png')

# Social preview, 1280x640
S=2; W,H=1280*S,640*S
im=Image.new('RGB',(W,H),BG); d=ImageDraw.Draw(im)
big=draw_icon(360*S)
im.paste(big,(110*S,140*S),big)
tf=font(SANS,112*S,True)
d.text((540*S,255*S),'QuickLogger',font=tf,fill=WHITE,anchor='ls')
sf=font(SANS,40*S)
d.text((544*S,330*S),'Fast net logging for ham radio,',font=sf,fill=CYAN,anchor='ls')
d.text((544*S,385*S),'right in your terminal — or over SSH.',font=sf,fill=CYAN,anchor='ls')
mf=font(MONO,24*S,True)
tags=[('F2','New Station'),('F3','Log & Close')]
x=544*S; y=450*S
for k,l in tags:
    kw=d.textlength(' '+k+' ',font=mf); lwid=d.textlength(' '+l+'  ',font=mf)
    d.rectangle([x,y,x+kw,y+40*S],fill=YELLOW); d.text((x+kw/2,y+20*S),k,font=mf,fill=(0,0,0),anchor='mm')
    d.rectangle([x+kw,y,x+kw+lwid,y+40*S],fill=(0,160,170)); d.text((x+kw+14*S,y+20*S),l,font=mf,fill=(0,0,0),anchor='lm')
    x+=kw+lwid+8*S
d.rectangle([0,0,W,14*S],fill=BLUE)
im.resize((1280,640),Image.LANCZOS).save('/Users/weskeene/CLionProjects/QuickLogger/docs/social-preview.png')
