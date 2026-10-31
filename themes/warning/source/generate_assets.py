#!/usr/bin/env python3
"""Rebuild Warning!'s original package art, PCM sounds and declarative layouts (Pillow)."""
from pathlib import Path
import json
import math
import random
import shutil
import struct
import wave
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets'
ASSETS.mkdir(exist_ok=True)
CYAN, RED, YELLOW, WHITE, BG = '#1fd8ff', '#ff2a3c', '#ffec1f', '#e8f4ff', '#09021d'
FONT = ASSETS / 'Audiowide-Regular.ttf'

def save(image, name):
    image.save(ASSETS / name, optimize=True)

def canvas(size, painter, name):
    scale = 3
    im = Image.new('RGBA', (size[0] * scale, size[1] * scale))
    painter(ImageDraw.Draw(im), scale)
    save(im.resize(size, Image.Resampling.LANCZOS), name)

# Transparent seams let this repeat vertically without a jump.
rng = random.Random(743)
stars = Image.new('RGBA', (650, 800))
d = ImageDraw.Draw(stars)
for i in range(175):
    x, y = rng.randrange(650), rng.randrange(800)
    c = rng.choice([(232, 244, 255, 180), (126, 208, 255, 140), (255, 125, 224, 110)])
    r = 1 if i % 5 else 2
    d.rectangle((x, y, x+r-1, y+r-1), fill=c)
save(stars, 'stars.png')

def ship(d, s):
    pts = [(42, 2), (54, 34), (80, 62), (56, 58), (50, 71), (34, 71), (28, 58), (4, 62), (30, 34)]
    p = [(x*s, y*s) for x,y in pts]
    d.polygon(p, fill=WHITE)
    d.line(p + [p[0]], fill=CYAN, width=2*s)
    d.polygon([(x*s,y*s) for x,y in [(42,12),(49,36),(42,44),(35,36)]], fill=RED)
    d.polygon([(34*s,72*s),(50*s,72*s),(42*s,80*s)], fill=YELLOW)
    d.line((16*s,57*s,32*s,48*s), fill='#6a81b0', width=2*s)
    d.line((68*s,57*s,52*s,48*s), fill='#6a81b0', width=2*s)
canvas((84,84), ship, 'ship.png')

def reticle(d,s):
    n=160
    for a in range(0,360,20):
        d.arc((12*s,12*s,148*s,148*s), a, a+9, fill=YELLOW, width=2*s)
    for points in [[(3,45),(3,3),(45,3)],[(115,3),(157,3),(157,45)],[(157,115),(157,157),(115,157)],[(45,157),(3,157),(3,115)]]:
        d.line([(x*s,y*s) for x,y in points], fill=YELLOW, width=4*s)
canvas((160,160),reticle,'reticle.png')

for name,c in [('frame.png',CYAN),('frame-unplayed.png',RED)]:
    def frame(d,s,c=c):
        p=[(14,2),(142,2),(154,14),(154,142),(142,154),(14,154),(2,142),(2,14)]
        d.line([(x*s,y*s) for x,y in p+[p[0]]], fill=c, width=4*s)
        for x,y in [(0,0),(156,0),(0,156),(156,156)]:
            dx=1 if x==0 else -1; dy=1 if y==0 else -1
            d.polygon([(x*s,y*s),((x+dx*12)*s,y*s),(x*s,(y+dy*12)*s)], fill='#110639')
    canvas((156,156),frame,name)

def warning(d,s):
    d.rectangle((0,0,650*s,150*s),fill=(5,1,16,245))
    for y in (0,132):
        d.rectangle((0,y*s,650*s,(y+18)*s),fill=YELLOW)
        for x in range(-40,690,44):
            d.polygon([(x*s,y*s),((x+22)*s,y*s),((x+4)*s,(y+18)*s),((x-18)*s,(y+18)*s)],fill='#151020')
canvas((650,150),warning,'warning-band.png')

placeholder=Image.new('RGBA',(256,256),'#160b39')
d=ImageDraw.Draw(placeholder)
d.line((36,128,220,128),fill='#274a73',width=1)
d.line((128,36,128,220),fill='#274a73',width=1)
d.polygon([(128,60),(172,98),(166,170),(128,190),(90,170),(84,98)],outline=CYAN,width=3)
d.polygon([(113,115),(143,115),(143,142),(113,142)],fill=CYAN)
save(placeholder,'placeholder.png')

# The title is package art so its shadow and line breaks match the reference.
title=Image.new('RGBA',(540,360))
d=ImageDraw.Draw(title)
f=ImageFont.truetype(str(FONT),92)
for i,line in enumerate(['BACK','LOG','BLAST']):
    y=i*110
    d.text((7,y+6),line,font=f,fill=YELLOW,stroke_width=1)
    d.text((1,y),line,font=f,fill=RED,stroke_width=1)
save(title,'title.png')
portrait_title=Image.new('RGBA',(780,140))
d=ImageDraw.Draw(portrait_title)
d.text((5,9),'WARNING!',font=ImageFont.truetype(str(FONT),106),fill=YELLOW)
d.text((0,4),'WARNING!',font=ImageFont.truetype(str(FONT),106),fill=RED)
save(portrait_title,'title-portrait.png')
shutil.copy2(ROOT.parent/'pocket-dimension/assets/hollycast-logo.png', ASSETS/'hollycast-logo.png')

# Quiet synthesized UI sounds; the app applies its normal audio volume.
for name,duration in [('move.wav',.09),('launch.wav',.43)]:
    with wave.open(str(ASSETS/name),'wb') as out:
        out.setparams((1,2,44100,0,'NONE','not compressed'))
        samples=[]
        phase=0.0
        for i in range(int(duration*44100)):
            t=i/44100; u=t/duration
            freq=(1200-700*u) if name=='move.wav' else (180+1800*u*u)
            phase+=2*math.pi*freq/44100
            env=min(1,t/.006)*(1-u)**2
            samples.append(struct.pack('<h',int(4200*env*math.sin(phase))))
        out.writeframes(b''.join(samples))

def e(kind,rect,**kw): return dict(type=kind,rect=rect,**kw)
def label(rect,text,**kw): return e('text',rect,text=text,color=CYAN,fontSize=.018,wrap=False,**kw)
def stat(rect,source,**kw): return e('stat',rect,source=source,color=WHITE,fontSize=.032,bold=True,wrap=False,**kw)
def button(rect,action,text,**kw):
    return e('button',rect,action=action,text=text,color=WHITE,background='120b30',borderColor=CYAN,rounding=0,fontSize=.018,**kw)

def common():
    return dict(mode='grid',activation='doubleClick',toolbar='compact',toolbarLogo='assets/hollycast-logo.png',toolbarLogoHeight=40,
                toolbarSelectedTextColor=BG,background=BG,artworkSource='cover',placeholder='assets/placeholder.png',showTitles=False,showItemActions=False,
                showFavouriteMarker=True,hoverDetails=False,showInfoTooltip=False,selectorStyle='outline',selectorColor='00000000',
                selectorBorderColor='00000000',textColor=YELLOW,selectedTextColor=YELLOW,minItemWidth=48,spacing=15,fontSize=.018,
                pagedGrid=dict(rows=4,padding=4,frame='assets/frame.png',unplayedFrame='assets/frame-unplayed.png'),
                sounds=dict(move='assets/move.wav',launch='assets/launch.wav'),launch=dict(seconds=.43))

wide=common()
wide.update(id='warning',name='Warning! / Arcade',canvasAspect=1.6,columns=4,browser=[.252,.025,.491,.79],narrowLayout='warning-portrait',narrowAspect=1.15)
a=[
 e('panel',[.24,0,.512,1],color='19084b',endColor='260b4e',zIndex=-5),
 e('image',[.244,0,.504,1],source='assets/stars.png',tileSeconds=9,zIndex=-4),
 e('panel',[.24,0,.002,1],color=CYAN,zIndex=-3),e('panel',[.75,0,.002,1],color=CYAN,zIndex=-3),
 e('image',[.02,.015,.21,.225],source='assets/title.png',fit='contain'),
 label([.03,.265,.18,.024],'WAVE'),stat([.03,.302,.07,.045],'page',digits=2),label([.10,.305,.025,.04],'/'),stat([.128,.302,.085,.045],'pageCount',digits=2),
 label([.03,.375,.18,.025],'SCORE'),stat([.03,.414,.195,.045],'totalPlayHours',digits=7,valueScale=1000),
 label([.03,.49,.18,.025],'HI-SCORE'),stat([.03,.529,.195,.045],'maxPlayHours',digits=7,valueScale=1000),
 label([.03,.61,.19,.025],'LIVES / FAVOURITES'),stat([.03,.649,.18,.045],'favouriteCount',digits=2),
 label([.03,.73,.18,.025],'BOSSES LEFT'),stat([.03,.769,.18,.045],'unplayedCount',digits=2),
 e('text',[.03,.849,.18,.055],text='1,000 PTS PER\nPLAYED HOUR',fontSize=.014,color='8793bf'),
 e('text',[.03,.933,.18,.045],text='CYAN  /  PLAYED\nRED  /  UNPLAYED',fontSize=.014,color='8793bf'),
 button([.265,.945,.107,.04],'previousPage','PREV WAVE'),button([.62,.945,.107,.04],'nextPage','NEXT WAVE'),
 e('image',[.4,.858,.066,.106],source='assets/ship.png',anchor='selectionX'),
 e('image',[.4,.2,.128,.205],source='assets/reticle.png',anchor='selection'),
 e('panel',[.4,.862,.008,.02],color='1fd8ffb0',anchor='selectionTrail',onSelection=.26),
 e('panel',[.4,.862,.002,.02],color='e8f4ffff',anchor='selectionTrail',onSelection=.26),
 e('artwork',[.768,.025,.212,.3392],source='preview',fallbackImage='assets/placeholder.png',fit='contain'),
 e('metadata',[.766,.394,.218,.086],source='title',fontSize=.033,color=YELLOW,wrap=True),
 e('details',[.765,.49,.22,.248],fields=['playTime','lastPlayed','developer','releaseDate','players','system'],fontSize=.018,color=WHITE),
 label([.768,.76,.21,.025],'THREAT LEVEL'),
 e('panel',[.768,.799,.212,.026],color='3a103f'),e('meter',[.77,.801,.208,.022],source='neglect',color=RED),
 button([.768,.837,.212,.043],'favourite','FAVOURITE',selectedText='FAVOURITED',icon='star',selectedIcon='star-filled',selectedColor=YELLOW,selectedBorderColor=YELLOW),
 e('button',[.768,.904,.212,.08],action='activate',text='FIRE!',fontSize=.047,color=WHITE,background=RED,rounding=0),
 e('image',[.242,.39,.508,.17],source='assets/warning-band.png',fit='stretch',when='unplayed',onSelection=1.35,zIndex=10),
 e('text',[.26,.425,.472,.063],text='WARNING!!',color=RED,fontSize=.055,align='center',when='unplayed',onSelection=1.35,zIndex=11),
 e('text',[.26,.494,.472,.028],text='UNPLAYED TARGET',color=YELLOW,fontSize=.021,align='center',when='unplayed',onSelection=1.35,zIndex=11),
 e('panel',[.4,.862,.012,.02],color='e8f4ff',anchor='selectionTrail',when='launching',zIndex=15),
 e('panel',[0,0,1,1],color='d9f7ff',when='launching',launchFadeFrom=.78,zIndex=20)
]
wide['elements']=a

portrait=common()
portrait.update(id='warning-portrait',name='Warning! / Vertical',columns=2,browser=[.04,.195,.92,.335],spacing=12,minItemWidth=48,toolbarLogoHeight=24)
portrait['pagedGrid']['rows']=2
portrait['elements']=[
 e('image',[.03,.008,.53,.073],source='assets/title-portrait.png'),
 label([.76,.018,.2,.02],'GAMES'),stat([.76,.048,.2,.035],'gameCount',digits=2),
 e('panel',[.03,.099,.94,.071],color='120b30',zIndex=-3),
 label([.05,.105,.25,.02],'WAVE'),stat([.05,.135,.105,.029],'page',digits=2),label([.165,.135,.035,.025],'/'),stat([.22,.135,.105,.029],'pageCount',digits=2),
 label([.405,.105,.24,.02],'LIVES'),stat([.405,.135,.16,.029],'favouriteCount',digits=2),
 label([.72,.105,.24,.02],'BOSSES'),stat([.72,.135,.16,.029],'unplayedCount',digits=2),
 e('panel',[.02,.184,.96,.481],color='19084b',endColor='260b4e',zIndex=-5),
 e('image',[.02,.184,.96,.481],source='assets/stars.png',tileSeconds=9,zIndex=-4),
 e('panel',[.02,.184,.002,.481],color=CYAN,zIndex=-3),e('panel',[.978,.184,.002,.481],color=CYAN,zIndex=-3),
 e('image',[.4,.546,.10,.072],source='assets/ship.png',anchor='selectionX'),
 e('image',[.4,.2,.28,.205],source='assets/reticle.png',anchor='selection'),
 e('panel',[.4,.551,.012,.02],color='1fd8ffb0',anchor='selectionTrail',onSelection=.26),
 button([.04,.625,.26,.033],'previousPage','PREV WAVE'),button([.70,.625,.26,.033],'nextPage','NEXT WAVE'),
 e('panel',[.03,.687,.94,.239],color='120b30',zIndex=-3),
 e('artwork',[.045,.704,.25,.205],source='preview',fallbackImage='assets/placeholder.png'),
 e('metadata',[.33,.702,.62,.064],source='title',fontSize=.027,color=YELLOW),
 e('metadata',[.33,.777,.62,.025],fields=['developer','system'],separator=' / ',fontSize=.016,color=CYAN,wrap=False),
 e('details',[.33,.812,.62,.079],fields=['playTime','lastPlayed'],fontSize=.016,color=WHITE),
 label([.33,.900,.20,.02],'THREAT'),e('panel',[.55,.902,.4,.016],color='3a103f'),e('meter',[.55,.902,.4,.016],source='neglect',color=RED),
 button([.035,.946,.38,.046],'favourite','FAVOURITE',selectedText='FAVOURITED',icon='star',selectedIcon='star-filled',selectedColor=YELLOW,selectedBorderColor=YELLOW),
 e('button',[.44,.94,.525,.052],action='activate',text='FIRE!',fontSize=.034,color=WHITE,background=RED,rounding=0),
 e('image',[.023,.342,.954,.12],source='assets/warning-band.png',fit='stretch',when='unplayed',onSelection=1.35,zIndex=10),
 e('text',[.045,.371,.91,.045],text='WARNING!!',color=RED,fontSize=.041,align='center',when='unplayed',onSelection=1.35,zIndex=11),
 e('text',[.045,.421,.91,.023],text='UNPLAYED TARGET',color=YELLOW,fontSize=.018,align='center',when='unplayed',onSelection=1.35,zIndex=11),
 e('panel',[.4,.551,.015,.02],color='e8f4ff',anchor='selectionTrail',when='launching',zIndex=15),
 e('panel',[0,0,1,1],color='d9f7ff',when='launching',launchFadeFrom=.78,zIndex=20)
]
colors=dict(Text='e8f4ff',TextDisabled='8793bf',WindowBg='09021d',ChildBg='09021d',PopupBg='140a31',Border='274470',Separator='274470',
            FrameBg='160b39',FrameBgHovered='25375d',FrameBgActive='173e67',Button='120b30',ButtonHovered='17305e',ButtonActive='1fd8ff',
            Header='213a68',HeaderHovered='244b76',HeaderActive='285a8b',CheckMark='ffec1f',SliderGrab='1fd8ff',SliderGrabActive='ffec1f',
            NavCursor='ffec1f',ScrollbarBg='00000000',ScrollbarGrab='31507c',ScrollbarGrabHovered='1fd8ff',ScrollbarGrabActive='ffec1f',
            TitleBg='09021d',TitleBgActive='140a31',TableHeaderBg='160b39',TextLink='1fd8ff')
theme=dict(formatVersion=1,id='warning',name='Warning!',author='Hollycast contributors',description='Your library is the enemy formation. Lock on, fire, and take on your unplayed backlog, one sixteen-game wave at a time.',baseTheme=0,
           fonts=dict(body='assets/Audiowide-Regular.ttf',bold='assets/PressStart2P-Regular.ttf'),colors=colors,layouts=[wide,portrait])
def clean(value):
    if isinstance(value, dict): return {k: clean(v) for k, v in value.items()}
    if isinstance(value, list): return [clean(v) for v in value]
    return value[1:] if isinstance(value, str) and value.startswith('#') else value
(ROOT/'theme.json').write_text(json.dumps(clean(theme),indent=2)+'\n')
