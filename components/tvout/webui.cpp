#define TVOUT_ENGINE
#include "TVout32.h"
#include "webui.h"
#include "imgstore.h"
#include "tv_state.h"
#include "sound.h"
#include "game.h"
#include <WebServer.h>
#include <LittleFS.h>
#include <mutex>
#include <lwip/sockets.h>

static WebServer server(80);
static uint8_t buf[IMG_MAX_BYTES];   // base64 decode target; handlers run one at a time

/**Tiny base64 decoder -- skips '=' and any whitespace the browser adds.**/
static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static size_t b64decode(const char *in, size_t inlen, uint8_t *out, size_t outmax) {
  size_t o = 0;
  int acc = 0, bits = 0;
  for (size_t i = 0; i < inlen; i++) {
    int v = b64val(in[i]);
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= outmax) return o;
      out[o++] = (acc >> bits) & 0xFF;
    }
  }
  return o;
}

static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TvOut FGF</title><style>
:root{--bg:#111;--fg:#eee;--dim:#888;--ac:#4ade80;--line:#333}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);
     font:14px/1.5 ui-monospace,Menlo,Consolas,monospace}
h1{font-size:18px;margin:0 0 16px;color:var(--ac);letter-spacing:.1em}
.card{border:1px solid var(--line);padding:14px;margin-bottom:14px}
#drop{border:1px dashed var(--line);padding:28px;text-align:center;color:var(--dim);cursor:pointer}
#drop.on{border-color:var(--ac);color:var(--ac)}
canvas{image-rendering:pixelated;background:#000;border:1px solid var(--line);
       max-width:100%;display:block;margin:12px 0}
label{display:inline-flex;align-items:center;gap:6px;margin-right:14px;color:var(--dim)}
input[type=text]{background:#000;color:var(--fg);border:1px solid var(--line);
                 padding:6px 8px;font:inherit;width:160px}
input[type=range]{vertical-align:middle;width:140px}
button{background:#000;color:var(--ac);border:1px solid var(--ac);padding:6px 12px;
       font:inherit;cursor:pointer}
button:hover{background:var(--ac);color:#000}
button.x{color:#f87171;border-color:#f87171}
button.x:hover{background:#f87171;color:#000}
table{width:100%;border-collapse:collapse}
td{border-bottom:1px solid var(--line);padding:6px 4px}
td.r{text-align:right;color:var(--dim)}
td.u{color:var(--dim);font-size:12px}
.tag{color:#000;background:var(--dim);font-size:11px;padding:0 4px}
select{background:#000;color:var(--fg);border:1px solid var(--line);font:inherit}
td button{margin:1px 0}
button:disabled{opacity:.35;cursor:default;background:#000;color:var(--ac)}
#msg{min-height:20px;color:var(--ac)}
.row{margin:6px 0}
.modes button.on{background:var(--ac);color:#000}
#mt{width:260px}
.modes button{margin:2px 4px 2px 0;min-width:34px}
</style></head><body>
<h1>TvOut FGF</h1>

<div class="card modes">
  <b>display</b>
  <div class="row">
    <label>image <select id="cur"></select></label>
  </div>
  <div class="row">
    <button data-p="a">intro</button><button data-p="i">show</button><button data-p="d">dvd</button>
    <label><input type="checkbox" id="trail" checked> trail enabled</label>
  </div>
  <div class="row">
    <input type="text" id="mt" placeholder="message" maxlength="63">
    <button data-p="m">message</button>
  </div>
  <div class="row">
    <button data-p="b">butterfly</button><button data-p="c">cube</button>
    <button data-p="g">game &rarr;</button>
  </div>
  <b>sound</b>
  <div class="row">
    <button onclick="go('/snd?name=birthday')">birthday</button>
    <button onclick="go('/snd?name=beep')">beep</button>
    <button onclick="go('/snd?name=off')">off</button>
  </div>
</div>

<div class="card">
  <div id="drop">drop an image or GIF here, or click to choose</div>
  <input type="file" id="f" accept="image/*" hidden>
  <canvas id="pv" width="320" height="240"></canvas>
  <div>
    <label>levels <select id="lv">
      <option value="16">16 greys</option><option value="4">4 greys</option>
      <option value="2">black/white</option></select></label>
    <label>dither <select id="dit">
      <option value="fs">Floyd-Steinberg</option>
      <option value="bayer">ordered (best for GIFs)</option>
      <option value="none">none</option></select></label>
    <label><input type="checkbox" id="inv"> invert</label>
    <label>brightness <input type="range" id="thr" min="16" max="240" value="128"></label>
    <label>size <input type="range" id="sz" min="5" max="100" value="100"> <span id="szv">100%</span></label>
    <span id="anim" style="display:none"><label>keep every <select id="every">
      <option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>6</option>
      </select> frame</label></span>
  </div>
  <div id="info" style="color:var(--dim);margin-top:6px"></div>
  <div style="margin-top:10px">
    <input type="text" id="nm" placeholder="name" maxlength="24">
    <button id="up">upload</button>
    <span id="msg"></span>
  </div>
</div>

<div class="card">
  <b>stored</b>
  <div style="color:var(--dim);font-size:12px">replace = overwrite that file with the image loaded above</div>
  <table id="ls"></table>
  <div id="use" style="color:var(--dim);margin-top:8px"></div>
</div>


<script>
const MAXW=320, MAXH=240, MAXF=120, MAXB=1024*1024, CHUNK=9600;
let src=null;      // {frames:[canvas], delays:[ms], gif:bool} -- what was loaded
let packed=null;   // still: .tvb bytes
let anim=null;     // animation: {w,h,frames:[rows], delays:[ms]}
let playT=null, busy=false;
const $=id=>document.getElementById(id);
const msg=t=>{$('msg').textContent=t; if(t) setTimeout(()=>{if($('msg').textContent===t)$('msg').textContent='';},4000);};
// which fixed mode draws each logo file
const USE={logo_princ:'boot intro'};

$('drop').onclick=()=>$('f').click();
$('f').onchange=e=>load(e.target.files[0]);
['dragenter','dragover'].forEach(v=>$('drop').addEventListener(v,e=>{
  e.preventDefault(); $('drop').classList.add('on');}));
['dragleave','drop'].forEach(v=>$('drop').addEventListener(v,e=>{
  e.preventDefault(); $('drop').classList.remove('on');}));
$('drop').addEventListener('drop',e=>load(e.dataTransfer.files[0]));
['lv','dit','inv','thr','sz','every'].forEach(id=>$(id).oninput=render);

/*---------------- GIF decoding (no browser API: the page is plain http) ----------------*/
function lzw(minSize,data,count){
  const clear=1<<minSize, eoi=clear+1, out=new Uint8Array(count);
  const prefix=new Uint16Array(4096), suffix=new Uint8Array(4096), stack=new Uint8Array(4097);
  for(let i=0;i<clear;i++) suffix[i]=i;
  let size=minSize+1, mask=(1<<size)-1, next=eoi+1, old=-1, first=0;
  let acc=0, bits=0, p=0, op=0;
  while(op<count){
    while(bits<size){ if(p>=data.length) return out; acc|=data[p++]<<bits; bits+=8; }
    let code=acc&mask; acc>>>=size; bits-=size;
    if(code===clear){ size=minSize+1; mask=(1<<size)-1; next=eoi+1; old=-1; continue; }
    if(code===eoi) break;
    if(old===-1){ out[op++]=suffix[code]; old=first=code; continue; }
    const cur=code; let sp=0;
    if(code>=next){ stack[sp++]=first; code=old; }
    while(code>=clear){ stack[sp++]=suffix[code]; code=prefix[code]; }
    first=code; stack[sp++]=first;
    if(next<4096){
      prefix[next]=old; suffix[next]=first; next++;
      if(next===mask+1 && size<12){ size++; mask=(1<<size)-1; }
    }
    old=cur;
    while(sp>0 && op<count) out[op++]=stack[--sp];
  }
  return out;
}

function decodeGif(ab){
  const d=new Uint8Array(ab); let p=6;
  const u16=()=>{const v=d[p]|d[p+1]<<8; p+=2; return v;};
  if(String.fromCharCode(d[0],d[1],d[2])!=='GIF') throw 'not a GIF';
  const W=u16(), H=u16(), pf=d[p++]; p+=2;
  let gct=null;
  if(pf&0x80){ const n=2<<(pf&7); gct=d.subarray(p,p+3*n); p+=3*n; }
  const cv=new Uint8ClampedArray(W*H*4), frames=[], delays=[];
  let gce={disp:0,delay:0,tr:-1}, prevDisp=0, prevRect=null, saved=null;
  while(p<d.length && frames.length<MAXF*6){
    const b=d[p++];
    if(b===0x3B) break;                                   // trailer
    if(b===0x21){                                         // extension
      const lbl=d[p++];
      if(lbl===0xF9 && d[p]===4){
        const f=d[p+1];
        gce={disp:(f>>2)&7, delay:(d[p+2]|d[p+3]<<8)*10, tr:(f&1)?d[p+4]:-1};
      }
      while(d[p]) p+=d[p]+1; p++;
      continue;
    }
    if(b!==0x2C) break;                                   // garbage: stop here
    const fx=u16(), fy=u16(), fw=u16(), fh=u16(), f2=d[p++];
    let ct=gct;
    if(f2&0x80){ const n=2<<(f2&7); ct=d.subarray(p,p+3*n); p+=3*n; }
    const minSize=d[p++];
    let len=0; for(let q=p; d[q]; q+=d[q]+1) len+=d[q];
    const data=new Uint8Array(len); let o=0;
    while(d[p]){ const n=d[p++]; data.set(d.subarray(p,p+n),o); o+=n; p+=n; } p++;

    if(prevDisp===2 && prevRect){                         // restore to background
      const [rx,ry,rw,rh]=prevRect;
      for(let y=ry;y<Math.min(H,ry+rh);y++) cv.fill(0,(y*W+rx)*4,(y*W+Math.min(W,rx+rw))*4);
    } else if(prevDisp===3 && saved) cv.set(saved);       // restore to previous
    if(gce.disp===3) saved=cv.slice();

    const idx=lzw(minSize,data,fw*fh);
    let rows=null;
    if(f2&0x40){ rows=[]; for(const [s,st] of [[0,8],[4,8],[2,4],[1,2]]) for(let y=s;y<fh;y+=st) rows.push(y); }
    for(let r=0;r<fh;r++){
      const y=fy+(rows?rows[r]:r); if(y>=H) continue;
      for(let c=0;c<fw;c++){
        const x=fx+c; if(x>=W) continue;
        const ci=idx[r*fw+c];
        if(ci===gce.tr || !ct || ci*3+2>=ct.length) continue;
        const k=(y*W+x)*4; cv[k]=ct[ci*3]; cv[k+1]=ct[ci*3+1]; cv[k+2]=ct[ci*3+2]; cv[k+3]=255;
      }
    }
    const c=document.createElement('canvas'); c.width=W; c.height=H;
    c.getContext('2d').putImageData(new ImageData(cv.slice(),W,H),0,0);
    frames.push(c); delays.push(gce.delay<20?100:gce.delay);   // browsers treat <20ms as 100
    prevDisp=gce.disp; prevRect=[fx,fy,fw,fh]; gce={disp:0,delay:0,tr:-1};
  }
  if(!frames.length) throw 'no frames';
  return {frames,delays};
}

/*---------------- loading ----------------*/
function load(file){
  if(!file) return;
  if(!$('nm').value) $('nm').value=file.name.replace(/\.[^.]+$/,'')
                                   .replace(/[^A-Za-z0-9_-]/g,'_').slice(0,24);
  const r=new FileReader();
  if(/gif$/i.test(file.type)||/\.gif$/i.test(file.name)){
    r.onload=()=>{
      try{
        const g=decodeGif(r.result);
        src={frames:g.frames, delays:g.delays, gif:g.frames.length>1};
        if(src.gif){
          $('dit').value='bayer';                           // stable between frames
          $('every').value=String(Math.min(6,Math.max(1,Math.ceil(src.frames.length/MAXF))));
        }
        $('anim').style.display=src.gif?'':'none';
        render();
      }catch(e){ msg('GIF: '+e); }
    };
    r.readAsArrayBuffer(file);
  } else {
    r.onload=()=>{
      const im=new Image();
      im.onload=()=>{
        const c=document.createElement('canvas'); c.width=im.width; c.height=im.height;
        c.getContext('2d').drawImage(im,0,0);
        src={frames:[c], delays:[0], gif:false};
        $('anim').style.display='none';
        render();
      };
      im.src=r.result;
    };
    r.readAsDataURL(file);
  }
}

/*---------------- conversion ----------------*/
const B8=[0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,
          3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21];
const BPP={2:1,4:2,16:4};

// canvas -> grey levels 0..L-1, dithered. "brightness" is the old threshold:
// at 2 levels it behaves exactly as before.
function toLevels(c,w,h,L){
  const t=document.createElement('canvas'); t.width=w; t.height=h;
  const x=t.getContext('2d',{willReadFrequently:true});
  x.fillStyle='#000'; x.fillRect(0,0,w,h);              // transparency -> black
  x.drawImage(c,0,0,w,h);
  const p=x.getImageData(0,0,w,h).data, g=new Float32Array(w*h), off=128-$('thr').value;
  for(let i=0;i<w*h;i++) g[i]=0.299*p[i*4]+0.587*p[i*4+1]+0.114*p[i*4+2]+off;
  const mode=$('dit').value, q=new Uint8Array(w*h), st=255/(L-1);
  const qz=v=>Math.max(0,Math.min(L-1,Math.round(v/st)));
  if(mode==='fs'){
    for(let y=0;y<h;y++) for(let xx=0;xx<w;xx++){
      const i=y*w+xx, old=g[i], k=qz(old), e=old-k*st;
      q[i]=k;
      if(xx+1<w)          g[i+1]   +=e*7/16;
      if(y+1<h){
        if(xx>0)          g[i+w-1] +=e*3/16;
                          g[i+w]   +=e*5/16;
        if(xx+1<w)        g[i+w+1] +=e*1/16;
      }
    }
  } else if(mode==='bayer'){
    for(let y=0;y<h;y++) for(let xx=0;xx<w;xx++){
      const i=y*w+xx;
      q[i]=qz(g[i]+((B8[(y&7)*8+(xx&7)]+0.5)/64-0.5)*st);
    }
  } else for(let i=0;i<w*h;i++) q[i]=qz(g[i]);
  if($('inv').checked) for(let i=0;i<w*h;i++) q[i]=L-1-q[i];
  return q;
}

// levels -> rows of `bpp` bits per pixel, MSB = leftmost (the TV's layout)
function packRows(q,w,h,bpp){
  const sb=Math.ceil(w*bpp/8), out=new Uint8Array(sb*h), ppb=8/bpp;
  for(let y=0;y<h;y++) for(let xx=0;xx<w;xx++){
    const v=q[y*w+xx]; if(!v) continue;
    out[y*sb+Math.floor(xx/ppb)] |= v<<(8-bpp*(xx%ppb+1));
  }
  return out;
}

function preview(rows,w,h,bpp){
  const q=$('pv').getContext('2d'), id=q.createImageData(MAXW,MAXH), d2=id.data;
  for(let i=0;i<MAXW*MAXH;i++) d2[i*4+3]=255;          // opaque black
  const sb=Math.ceil(w*bpp/8), ppb=8/bpp, mx=(1<<bpp)-1, ox=(MAXW-w)>>1, oy=(MAXH-h)>>1;
  for(let y=0;y<h;y++) for(let xx=0;xx<w;xx++){
    const v=(rows[y*sb+Math.floor(xx/ppb)]>>(8-bpp*(xx%ppb+1)))&mx;
    if(!v) continue;
    const i=((oy+y)*MAXW+ox+xx)*4; d2[i]=d2[i+1]=d2[i+2]=Math.round(v*255/mx);
  }
  q.putImageData(id,0,0);
}

function render(){
  $('szv').textContent=$('sz').value+'%';
  clearTimeout(playT);
  if(!src) return;
  const L=+$('lv').value, bpp=BPP[L];
  const f0=src.frames[0];
  const s=Math.min(MAXW/f0.width, MAXH/f0.height)*$('sz').value/100;
  const w=Math.max(1,Math.round(f0.width*s)), h=Math.max(1,Math.round(f0.height*s));
  const fb=Math.ceil(w*bpp/8)*h;

  if(!src.gif && L===2){                                  // classic .tvb
    const rows=packRows(toLevels(f0,w,h,2),w,h,1);
    const ext=(w>255||h>255), off=ext?5:2;
    packed=new Uint8Array(off+rows.length); anim=null;
    if(ext){ packed[0]=0; packed[1]=w&255; packed[2]=w>>8; packed[3]=h&255; packed[4]=h>>8; }
    else   { packed[0]=w; packed[1]=h; }
    packed.set(rows,off);
    preview(rows,w,h,1);
    $('info').textContent=w+'x'+h+'  black/white  '+packed.length+' B';
  } else {                                                // .tva: grey and/or animated
    const every=src.gif?+$('every').value:1, frames=[], delays=[];
    for(let i=0;i<src.frames.length && frames.length<MAXF;i+=every){
      frames.push(packRows(toLevels(src.frames[i],w,h,L),w,h,bpp));
      let d=0; for(let k=i;k<Math.min(i+every,src.frames.length);k++) d+=src.gif?src.delays[k]:0;
      delays.push(Math.min(65535,d));
    }
    anim={w,h,bpp,frames,delays}; packed=null;
    const total=12+2*frames.length+fb*frames.length;
    const dropped=src.gif && Math.ceil(src.frames.length/every)>MAXF;
    $('info').textContent=w+'x'+h+'  '+(L===2?'black/white':L+' greys')+
      (src.gif?'  '+frames.length+' frames':'')+'  '+(total/1024).toFixed(1)+' KB'+
      (total>MAXB?'  -- too big (max 1 MB): fewer levels, smaller size or skip frames':'')+
      (dropped?'  -- capped at '+MAXF+' frames':'');
    let k=0;
    const step=()=>{ preview(frames[k],w,h,bpp); if(frames.length>1) playT=setTimeout(step,delays[k]); k=(k+1)%frames.length; };
    step();
  }
  document.querySelectorAll('.rep').forEach(b=>b.disabled=false);
}

/*---------------- upload ----------------*/
function b64(u8){ let s=''; for(let i=0;i<u8.length;i++) s+=String.fromCharCode(u8[i]); return btoa(s); }

async function post(url,u8){
  const r=await fetch(url,{method:'POST',body:b64(u8)});
  if(!r.ok) throw await r.text();
}

async function save(n){
  if(busy) return;
  busy=true;
  try{
    if(anim){
      const N=anim.frames.length, h=new Uint8Array(12+2*N), v=new DataView(h.buffer);
      h.set([84,86,65,49]);                               // "TVA1"
      v.setUint16(4,anim.w,true); v.setUint16(6,anim.h,true); v.setUint16(8,N,true);
      h[10]=1; h[11]=anim.bpp;                            // loop, bits per pixel
      anim.delays.forEach((d,i)=>v.setUint16(12+2*i,d,true));
      const base='/anim?name='+encodeURIComponent(n);
      await post(base,h);
      const fb=anim.frames[0].length, data=new Uint8Array(fb*N);
      anim.frames.forEach((f,i)=>data.set(f,i*fb));
      for(let pos=0;pos<data.length;pos+=CHUNK){          // <= 9600 bytes per request
        msg('uploading '+Math.round(100*pos/data.length)+'%');
        await post(base+'&pos='+pos,data.subarray(pos,pos+CHUNK));
      }
    } else {
      await post('/upload?name='+encodeURIComponent(n),packed);
    }
    msg('saved '+n);
  }catch(e){ msg('failed: '+e); }
  busy=false;
  list();
}

$('up').onclick=()=>{
  const n=$('nm').value.trim();
  if(!packed&&!anim) return msg('no image');
  if(!/^[A-Za-z0-9_-]{1,24}$/.test(n)) return msg('bad name');
  save(n);
};

function rep(n){
  if(!packed&&!anim) return msg('load an image first');
  if(confirm('Replace "'+n+'" with the loaded '+(anim?'animation':'image')+'?')) save(n);
}

async function go(u){ await fetch(u); }

async function list(){
  const r=await fetch('/list'), j=await r.json();
  j.sort((a,b)=>a.n.localeCompare(b.n));
  $('ls').innerHTML=j.map(i=>
    '<tr><td>'+i.n+(i.a>1?' <span class=tag>anim '+i.a+'f</span>':'')+
      (i.b>1?' <span class=tag>'+(1<<i.b)+' grey</span>':'')+'</td>'+
    '<td class=u>'+(USE[i.n]||'')+'</td>'+
    '<td class=r>'+i.w+'x'+i.h+'</td><td class=r>'+(i.s>4096?(i.s/1024).toFixed(1)+' KB':i.s+' B')+'</td>'+
    '<td class=r><button onclick="mode(\'i\',\''+i.n+'\')">'+(i.a>1?'play':'show')+'</button> '+
    '<button onclick="mode(\'d\',\''+i.n+'\')">as DVD</button> '+
    '<button class=rep onclick="rep(\''+i.n+'\')"'+(packed||anim?'':' disabled')+'>replace</button> '+
    '<button class=x onclick="del(\''+i.n+'\')">del</button></td></tr>').join('')
    || '<tr><td style=color:#888>nothing uploaded yet</td></tr>';
  const sel=$('cur'), keep=sel.value||state.img;
  sel.innerHTML=j.map(i=>'<option>'+i.n+'</option>').join('');
  if(j.some(i=>i.n===keep)) sel.value=keep;
  const u=await (await fetch('/use')).json();
  $('use').textContent=(u.used/1024).toFixed(1)+' KB of '+(u.total/1024).toFixed(0)+' KB used';
}
async function del(n){
  if(!confirm('Delete "'+n+'"?'+(USE[n]?' It is the '+USE[n]+' image.':''))) return;
  await fetch('/del?name='+n); list();
}
list();
// ---- display control: a intro, i show, m message, d dvd, b butterfly, c cube
let state={p:'',img:'',msg:'',trail:1};
const IMG_MODES='aid';
function mark(){
  document.querySelectorAll('.modes button[data-p]').forEach(b=>
    b.classList.toggle('on',b.dataset.p===state.p));
}
async function mode(p,name){
  if(p==='g'){ location.href='/game'; return; }   // the controller page starts it
  let u;
  if(p==='m') u='/msg?t='+encodeURIComponent($('mt').value);
  else {
    if(name) $('cur').value=name;
    u='/mode?p='+p+(IMG_MODES.includes(p)&&$('cur').value?'&name='+encodeURIComponent($('cur').value):'');
  }
  await fetch(u);
  state.p=p; mark();
}
document.querySelectorAll('.modes button[data-p]').forEach(b=>b.onclick=()=>mode(b.dataset.p));
$('mt').onkeydown=e=>{ if(e.key==='Enter') mode('m'); };
// picking another image applies at once if the TV is showing an image
$('cur').onchange=()=>{ if(IMG_MODES.includes(state.p)) mode(state.p); };
$('trail').onchange=()=>go('/trail?on='+($('trail').checked?1:0));
// the board is the truth (MQTT can change it too): load it, then keep in step
async function sync(first){
  try{
    state=await (await fetch('/state')).json();
    $('trail').checked=!!state.trail;
    if(first){ $('mt').value=state.msg; $('cur').value=state.img; }
    mark();
  }catch(e){}
}
sync(true); setInterval(sync,5000);
</script></body></html>)HTML";

static const char GAME_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>TvOut FGF - game</title><style>
:root{--bg:#111;--fg:#eee;--dim:#888;--ac:#4ade80;--line:#333;--hot:#f87171}
*{box-sizing:border-box;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);
  font:14px/1.4 ui-monospace,Menlo,Consolas,monospace;overflow:hidden;touch-action:none}
#bar{display:flex;justify-content:space-between;align-items:center;padding:10px 14px;
  border-bottom:1px solid var(--line)}
#bar a{color:var(--ac);text-decoration:none;border:1px solid var(--ac);padding:4px 10px}
#st{color:var(--dim)}
#st b{color:var(--fg)}
#pad{display:flex;justify-content:space-between;align-items:center;
  height:calc(100% - 50px);padding:16px;gap:16px}
.grid{display:grid;gap:10px}
#dir{grid-template-columns:repeat(3,72px);grid-template-rows:repeat(3,72px)}
#act{grid-template-columns:repeat(2,84px);grid-template-rows:56px 112px 56px}
.k{border:1px solid var(--ac);color:var(--ac);background:#000;display:flex;
  align-items:center;justify-content:center;font-size:22px;border-radius:6px}
.k.on{background:var(--ac);color:#000}
.k.fire{grid-column:span 2;border-color:var(--hot);color:var(--hot);font-size:20px;border-radius:50%}
.k.fire.on{background:var(--hot);color:#000}
.k.sm{font-size:13px}
#help{position:fixed;bottom:6px;left:0;right:0;text-align:center;color:var(--dim);font-size:11px}
</style></head><body>
<div id="bar"><a href="/" id="exit">&larr; exit</a><span id="st">connecting...</span></div>
<div id="pad">
  <div class="grid" id="dir">
    <span></span><div class="k" data-k="1">&#9650;</div><span></span>
    <div class="k" data-k="4">&#8630;</div><span></span><div class="k" data-k="8">&#8631;</div>
    <span></span><div class="k" data-k="2">&#9660;</div><span></span>
  </div>
  <div class="grid" id="act">
    <div class="k sm" data-k="16">&#9664; strafe</div><div class="k sm" data-k="32">strafe &#9654;</div>
    <div class="k fire" data-k="64">FIRE</div>
    <div class="k sm" data-k="128" style="grid-column:span 2">use / continue</div>
  </div>
</div>
<div id="help">keys: W/S or &uarr;/&darr; move &middot; &larr;/&rarr; or Q/E turn &middot; A/D strafe &middot; space fire &middot; enter use</div>
<script>
const $=id=>document.getElementById(id);
let mask=0, inflight=false, dirty=false;
const STATES=['','DEAD','LEVEL DONE','YOU WIN'];
function show(t){
  const [hp,ammo,k,n,l,fps,g]=t.split(',');
  if(fps===undefined) return;
  $('st').innerHTML='HP <b>'+hp+'</b> &middot; AMMO <b>'+ammo+'</b> &middot; KILLS <b>'+k+'/'+n+
    '</b> &middot; L<b>'+l+'</b> &middot; '+fps+' fps'+(+g?' &middot; <b>'+STATES[+g]+'</b>':'');
}
// one request in flight at a time; the newest state always gets through
function send(){
  if(inflight){ dirty=true; return; }
  inflight=true;
  fetch('/key?k='+mask).then(r=>r.text()).then(show).catch(()=>{}).finally(()=>{
    inflight=false; if(dirty){ dirty=false; send(); }
  });
}
function set(bit,on){
  const m=on?(mask|bit):(mask&~bit);
  if(m===mask) return;
  mask=m;
  document.querySelectorAll('.k').forEach(e=>e.classList.toggle('on',!!(mask&+e.dataset.k)));
  send();
}
// held keys are refreshed; the board releases anything not refreshed in 0.8s
setInterval(()=>{ if(mask) send(); },250);
setInterval(()=>{ if(!mask) fetch('/gstat').then(r=>r.text()).then(show).catch(()=>{}); },1000);

// touch / mouse: pointer events, so several buttons can be held at once
document.querySelectorAll('.k').forEach(e=>{
  const b=+e.dataset.k;
  e.addEventListener('pointerdown',ev=>{ ev.preventDefault(); e.setPointerCapture(ev.pointerId); set(b,true); });
  ['pointerup','pointercancel','lostpointercapture'].forEach(t=>e.addEventListener(t,()=>set(b,false)));
});
document.addEventListener('contextmenu',e=>e.preventDefault());

// keyboard
const KEYS={ArrowUp:1,KeyW:1,ArrowDown:2,KeyS:2,ArrowLeft:4,KeyQ:4,ArrowRight:8,KeyE:8,
            KeyA:16,KeyD:32,Space:64,ControlLeft:64,ControlRight:64,Enter:128,KeyF:128};
addEventListener('keydown',e=>{ const b=KEYS[e.code]; if(b){ e.preventDefault(); set(b,true); } });
addEventListener('keyup',e=>{ const b=KEYS[e.code]; if(b){ e.preventDefault(); set(b,false); } });
addEventListener('blur',()=>{ mask=0; send(); });

// start the game on the TV; leaving goes back to the bouncing image
fetch('/mode?p=g');
$('exit').onclick=e=>{ e.preventDefault(); mask=0; fetch('/key?k=0').finally(()=>
  fetch('/mode?p=d').finally(()=>location.href='/')); };
</script></body></html>)HTML";

/**------------ handlers ------------**/

static void hRoot() { server.send_P(200, "text/html", PAGE); }

static void hList() { server.send(200, "application/json", imgstore_list_json()); }

static void hUse() {
  server.send(200, "application/json",
              "{\"used\":" + String(imgstore_used()) +
              ",\"total\":" + String(imgstore_total()) + "}");
}

static void hUpload() {
  if (!server.hasArg("name") || !server.hasArg("plain")) {
    server.send(400, "text/plain", "missing name or body");
    return;
  }
  String name = server.arg("name");
  String b64  = server.arg("plain");

  size_t n = b64decode(b64.c_str(), b64.length(), buf, sizeof(buf));
  if (n < 3) { server.send(400, "text/plain", "bad base64"); return; }
  if (!imgstore_save(name.c_str(), buf, n))   { server.send(400, "text/plain", "rejected"); return; }
  server.send(200, "text/plain", "ok");
}

/**One piece of a .tva: no pos = header, pos=P = frame data at P (see imgstore.h).**/
static void hAnim() {
  if (!server.hasArg("name") || !server.hasArg("plain")) {
    server.send(400, "text/plain", "missing name or body");
    return;
  }
  const String &b64 = server.arg("plain");
  size_t n = b64decode(b64.c_str(), b64.length(), buf, sizeof(buf));
  String err;
  const int32_t pos = server.hasArg("pos") ? server.arg("pos").toInt() : -1;
  int r = imgstore_anim_put(server.arg("name").c_str(), pos, buf, n, err);
  if (r < 0) server.send(400, "text/plain", err);
  else       server.send(200, "text/plain", r ? "done" : "ok");
}

static void hDel() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "no name"); return; }
  server.send(imgstore_delete(server.arg("name").c_str()) ? 200 : 404, "text/plain", "");
}

/**Select the image for intro/show/dvd. Shared by /mode, /show and /dvd.**/
static void setImage() {
  if (!server.hasArg("name") || !server.arg("name").length()) return;
  strlcpy(img_name, server.arg("name").c_str(), sizeof(img_name));
  img_dirty = true;
}

static void hShow() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "no name"); return; }
  setImage();
  prog = 'i';
  server.send(200, "text/plain", "ok");
}

static void hDvd() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "no name"); return; }
  setImage();
  prog = 'd';
  server.send(200, "text/plain", "ok");
}

/**GET /msg?t=TEXT -> show it centred (mode m).**/
static void hMsg() {
  strlcpy(msgbuf, server.arg("t").c_str(), sizeof(msgbuf));
  msg_dirty = true;
  prog = 'm';
  server.send(200, "text/plain", "ok");
}

/**GET /state -> {"p":mode,"img":name,"msg":text,"trail":0|1} for the page.**/
static String jsonStr(const char *t) {
  String o = "\"";
  for (const char *c = t; *c; c++) {
    if (*c == '"' || *c == '\\') { o += '\\'; o += *c; }
    else if ((uint8_t)*c >= 0x20) o += *c;
  }
  return o + "\"";
}

static void hState() {
  const char p[2] = { prog, 0 };
  server.send(200, "application/json",
              "{\"p\":" + jsonStr(p) + ",\"img\":" + jsonStr(img_name) +
              ",\"msg\":" + jsonStr(msgbuf) + ",\"trail\":" + (dvd_trail ? "1" : "0") +
              ",\"heap\":" + ESP.getFreeHeap() + ",\"block\":" + ESP.getMaxAllocHeap() + "}");
}

/**GET /trail -> "1"/"0"; /trail?on=0|1 sets it (asdvd clears per frame when 0).**/
static void hTrail() {
  if (server.hasArg("on")) dvd_trail = server.arg("on") != "0";
  server.send(200, "text/plain", dvd_trail ? "1" : "0");
}

/**GET /mode?p=X[&name=IMG] -- X: a intro, i show, m message, d dvd,
 * b butterfly, c cube. name sets the image for a/i/d.**/
/**The game: /game page, held-key mask in, status out.**/
static void hGame() { server.send_P(200, "text/html", GAME_PAGE); }

static void hKey() {
  game_keys(server.hasArg("k") ? server.arg("k").toInt() : 0);
  server.send(200, "text/plain", game_status());
}

/**Game levels in LittleFS: GET /level?n=N reads /lvl/N.txt, POST stores it
 * (body = the map text, format in game.cpp), &del=1 deletes it. A stored file
 * overrides built-in level N or adds a new one; it applies from the next
 * time that level starts.**/
static void hLevel() {
  const int n = server.arg("n").toInt();
  if (n < 1 || n > 99) { server.send(400, "text/plain", "n must be 1..99"); return; }
  const String path = "/lvl/" + String(n) + ".txt";
  if (server.hasArg("del")) {
    server.send(LittleFS.remove(path) ? 200 : 404, "text/plain", "");
  } else if (server.method() == HTTP_POST) {
    const String &body = server.arg("plain");
    if (!body.length() || body.length() > 40 * 34) { server.send(400, "text/plain", "empty or too big"); return; }
    if (!LittleFS.exists("/lvl")) LittleFS.mkdir("/lvl");
    File f = LittleFS.open(path, "w");
    bool ok = f && f.print(body) == body.length();
    if (f) f.close();
    server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "write failed");
  } else {
    File f = LittleFS.open(path, "r");
    if (!f) { server.send(404, "text/plain", "no such level file"); return; }
    server.streamFile(f, "text/plain");
    f.close();
  }
}

static void hGstat() { server.send(200, "text/plain", game_status()); }

/**GET /shot.pgm -- what the TV shows right now, as a 320x240 16-grey PGM.**/
static void hShot() {
  static const char hdr[] = "P5\n320 240\n15\n";
  server.setContentLength(sizeof(hdr) - 1 + FB_W * FB_H);
  server.send(200, "image/x-portable-graymap", "");
  server.sendContent(hdr, sizeof(hdr) - 1);
  const uint8_t *fb = Composite.frame();
  static char rows[FB_W * 8];                       // 8 lines per send
  for (int y = 0; y < FB_H; y += 8) {
    for (int r = 0; r < 8; r++) {
      const uint8_t *s = fb + (y + r) * FB_STRIDE;
      char *d = rows + r * FB_W;
      for (int x = 0; x < FB_STRIDE; x++) { d[2 * x] = s[x] >> 4; d[2 * x + 1] = s[x] & 15; }
    }
    server.sendContent(rows, sizeof(rows));
  }
}

static void hMode() {
  setImage();
  if (server.hasArg("p") && server.arg("p").length()) prog = server.arg("p")[0];
  server.send(200, "text/plain", "ok");
}

static void hSnd() {
  bool ok = server.hasArg("name") && sound_by_name(server.arg("name").c_str());
  server.send(ok ? 200 : 400, "text/plain", ok ? "ok" : "unknown");
}


/*********************
* WiFi setup (fallback hotspot captive portal)
*
* ESPHome's captive_portal can't be used: it wants port 80, which this server
* already has. So the same job is done here: a form, a scan list, and a tiny
* DNS server that answers every name with the hotspot IP while it is up.
* The page only queues the SSID/password; ESPHome's loop saves them.
**********************/
static const char WIFI_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"><title>TvOut WiFi</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:auto;padding:16px}
h2{margin-top:0}input,button{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;font-size:16px;border-radius:6px;border:1px solid #555;background:#222;color:#eee}
button{background:#2a6;border:0;font-weight:bold}#nets div{padding:9px;border-bottom:1px solid #333;cursor:pointer;display:flex;justify-content:space-between}
#nets div:hover{background:#222}small{color:#999}a{color:#6cf}</style></head><body>
<h2>TvOut WiFi setup</h2>
<small>Pick a network or type its name.</small>
<div id="nets"><small>Scanning...</small></div>
<form method="POST" action="/wifisave">
<input id="ssid" name="ssid" placeholder="Network name (SSID)" maxlength="32" required autocapitalize="none" autocorrect="off">
<input id="psk" name="psk" type="password" placeholder="Password" maxlength="64">
<label><input type="checkbox" style="width:auto" onclick="psk.type=this.checked?'text':'password'"> show password</label>
<button type="submit">Save &amp; connect</button></form>
<p><small><a href="/?full=1">Image upload page</a></small></p>
<script>
function load(){fetch('/wifiscan').then(r=>r.json()).then(a=>{
 const seen={};a=a.filter(n=>n.ssid&&!seen[n.ssid]&&(seen[n.ssid]=1)).sort((x,y)=>y.rssi-x.rssi);
 const d=document.getElementById('nets');d.innerHTML=a.length?'':'<small>No networks found yet - retrying...</small>';
 a.forEach(n=>{const e=document.createElement('div');
  e.innerHTML='<span></span><small>'+(n.lock?'&#128274; ':'')+n.rssi+' dBm</small>';
  e.firstChild.textContent=n.ssid;e.onclick=()=>{ssid.value=n.ssid;psk.focus()};d.appendChild(e)});
}).catch(()=>{});}
load();setInterval(load,5000);
</script></body></html>)HTML";

static const char WIFI_SAVED[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"><title>TvOut WiFi</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:auto;padding:16px}</style></head><body>
<h2>Saved</h2><p>TvOut is connecting to the new network. If it works, this hotspot
closes and the board joins that network (address from its DHCP).</p>
<p>If the password was wrong, the hotspot comes back in about a minute -
reconnect to it and try again.</p></body></html>)HTML";

static std::mutex wifi_mx;
static std::string wifi_nets = "[]";
static volatile bool ap_active = false;
static uint32_t ap_ip = 0;                // network byte order
static char pend_ssid[33], pend_psk[65];
static bool pend = false;

void webui_set_ap(bool active, const char *ip) {
  std::lock_guard<std::mutex> l(wifi_mx);
  ap_ip = inet_addr(ip);
  ap_active = active;
}

void webui_set_networks(const std::string &json) {
  std::lock_guard<std::mutex> l(wifi_mx);
  wifi_nets = json;
}

bool webui_take_wifi(char ssid[33], char psk[65]) {
  std::lock_guard<std::mutex> l(wifi_mx);
  if (!pend) return false;
  memcpy(ssid, pend_ssid, sizeof(pend_ssid));
  memcpy(psk, pend_psk, sizeof(pend_psk));
  memset(pend_psk, 0, sizeof(pend_psk));
  pend = false;
  return true;
}

static void hWifi() { server.send_P(200, "text/html", WIFI_PAGE); }

static void hWifiScan() {
  std::string j;
  { std::lock_guard<std::mutex> l(wifi_mx); j = wifi_nets; }
  server.send(200, "application/json", j.c_str());
}

static void hWifiSave() {
  String ssid = server.arg("ssid"), psk = server.arg("psk");
  if (ssid.length() == 0 || ssid.length() > 32 || psk.length() > 64 ||
      (psk.length() > 0 && psk.length() < 8)) {
    server.send(400, "text/plain", "SSID 1-32 chars; password empty (open network) or 8-64 chars");
    return;
  }
  {
    std::lock_guard<std::mutex> l(wifi_mx);
    strlcpy(pend_ssid, ssid.c_str(), sizeof(pend_ssid));
    strlcpy(pend_psk, psk.c_str(), sizeof(pend_psk));
    pend = true;
  }
  server.send_P(200, "text/html", WIFI_SAVED);
}

/**Hotspot up: phones probe /generate_204, /hotspot-detect.html, ... -- send
 * them all to the setup page so the OS shows it as a sign-in portal.**/
static void hNotFound() {
  if (ap_active) {
    server.sendHeader("Location", "http://" + server.client().localIP().toString() + "/wifi", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

/**Minimal DNS: every A query gets the hotspot IP; anything else gets an empty
 * answer. Polled from the web task, socket only open while the AP is up.**/
static int dns_sock = -1;

static void dnsPoll() {
  if (!ap_active) {
    if (dns_sock >= 0) { close(dns_sock); dns_sock = -1; }
    return;
  }
  if (dns_sock < 0) {
    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(dns_sock, (sockaddr *) &a, sizeof(a)) < 0) { close(dns_sock); dns_sock = -1; return; }
    fcntl(dns_sock, F_SETFL, O_NONBLOCK);
  }
  uint8_t pkt[512];
  sockaddr_in from{};
  socklen_t fl = sizeof(from);
  int n = recvfrom(dns_sock, pkt, sizeof(pkt) - 16, 0, (sockaddr *) &from, &fl);
  if (n < 12) return;
  if ((pkt[2] & 0x80) || pkt[4] != 0 || pkt[5] != 1) return;  // queries with one question only
  int i = 12;
  while (i < n && pkt[i] != 0) {
    if (pkt[i] & 0xC0) return;                                 // no compression in a question
    i += pkt[i] + 1;
  }
  i++;                                                         // the root label
  if (i + 4 > n) return;
  const bool is_a = pkt[i] == 0 && pkt[i + 1] == 1;
  i += 4;                                                      // qtype + qclass
  pkt[2] = 0x84 | (pkt[2] & 0x01);                             // response, authoritative, keep RD
  pkt[3] = 0x80;                                               // RA, NOERROR
  pkt[6] = 0; pkt[7] = is_a ? 1 : 0;                           // answers
  memset(pkt + 8, 0, 4);                                       // no authority / additional
  if (is_a) {
    uint32_t ip;
    { std::lock_guard<std::mutex> l(wifi_mx); ip = ap_ip; }
    const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
    memcpy(pkt + i, ans, sizeof(ans));
    memcpy(pkt + i + sizeof(ans), &ip, 4);
    i += sizeof(ans) + 4;
  }
  sendto(dns_sock, pkt, i, 0, (sockaddr *) &from, fl);
}

static void hRootOrPortal() {
  if (ap_active && !server.hasArg("full")) hWifi();
  else hRoot();
}

static void webTask(void *) {
  for (;;) {
    server.handleClient();
    dnsPoll();
    vTaskDelay(1);
  }
}

void webui_begin(uint16_t port) {
  server.on("/",       HTTP_GET,  hRootOrPortal);
  server.on("/list",   HTTP_GET,  hList);
  server.on("/use",    HTTP_GET,  hUse);
  server.on("/upload", HTTP_POST, hUpload);
  server.on("/anim",   HTTP_POST, hAnim);
  server.on("/del",    HTTP_GET,  hDel);
  server.on("/show",   HTTP_GET,  hShow);
  server.on("/dvd",    HTTP_GET,  hDvd);
  server.on("/trail",  HTTP_GET,  hTrail);
  server.on("/msg",    HTTP_GET,  hMsg);
  server.on("/state",  HTTP_GET,  hState);
  server.on("/game",   HTTP_GET,  hGame);
  server.on("/key",    HTTP_GET,  hKey);
  server.on("/gstat",  HTTP_GET,  hGstat);
  server.on("/level",  HTTP_ANY,  hLevel);
  server.on("/shot.pgm", HTTP_GET, hShot);
  server.on("/mode",   HTTP_GET,  hMode);
  server.on("/snd",    HTTP_GET,  hSnd);
  server.on("/wifi",     HTTP_GET,  hWifi);
  server.on("/wifiscan", HTTP_GET,  hWifiScan);
  server.on("/wifisave", HTTP_POST, hWifiSave);
  server.onNotFound(hNotFound);
  server.begin(port);
  xTaskCreatePinnedToCore(webTask, "web", 6144, nullptr, 1, nullptr, 0);
}
