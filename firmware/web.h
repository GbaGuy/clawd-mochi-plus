#pragma once
// Phone/PC controller served at / — commands go through /cmd, live state from /state.
const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Clawd Mochi</title>
<style>
:root{--bg:#121110;--card:#1d1b19;--line:#33302c;--text:#eee9e4;--mute:#8c857d;--accent:#da1100;--hi:#ff6a4d}
*{box-sizing:border-box}
body{margin:0;padding:16px;font:15px system-ui,sans-serif;background:var(--bg);color:var(--text);
 display:flex;flex-direction:column;align-items:center;gap:12px}
main{width:100%;max-width:360px;display:flex;flex-direction:column;gap:12px}
header{display:flex;justify-content:space-between;align-items:baseline}
h1{font-size:13px;letter-spacing:3px;color:var(--hi);margin:0}
#net{font-size:12px;color:var(--mute)}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px}
.preview{display:flex;gap:12px;align-items:center}
#pv{width:144px;height:144px;border-radius:12px;flex:none;background:#da1100}
#cap{font-size:13px;color:var(--mute);word-break:break-word}
#moodname{font-size:20px;font-weight:600;margin-bottom:4px}
.bars{display:grid;grid-template-columns:auto 1fr auto;gap:6px 10px;align-items:center;font-size:13px}
.bar{height:8px;background:var(--line);border-radius:4px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--hi);border-radius:4px;transition:width .4s}
.bar i.warn{background:#ffb02e}.bar i.crit{background:#ff3b30}
#reset{font-size:12px;color:var(--mute);margin-top:6px}
#pages button{flex:1;padding:11px 0}
.g4{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:8px}
#pageinfo{font-size:12px;color:var(--mute);margin-top:8px;text-align:center}
.g{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.row{display:flex;gap:8px}
button{background:#26221f;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:11px 10px;font:inherit;cursor:pointer}
button:active{transform:scale(.95)}
button.on{border-color:var(--hi);color:var(--hi)}
input[type=text]{flex:1;min-width:0;background:#26221f;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:10px;font:inherit}
input[type=color]{width:44px;height:42px;border:0;background:none;padding:0}
#cv{background:var(--accent);border-radius:12px;touch-action:none;width:240px;height:240px;display:block;margin:0 auto}
details summary{cursor:pointer;color:var(--mute);font-size:13px;letter-spacing:1px}
details[open] summary{margin-bottom:10px}
</style></head><body><main>
<header><h1>CLAWD &middot; MOCHI</h1><span id=net></span></header>

<div class="card preview"><canvas id=pv width=120 height=120></canvas>
 <div><div id=moodname>…</div><div id=cap></div></div></div>

<div class=card id=usage hidden><div class=bars id=bars></div><div id=reset></div></div>

<div class=card><div class=row id=pages>
 <button onclick="c('page prev').then(poll)">&lsaquo; prev</button>
 <button id=p-auto onclick="c('page auto').then(poll)">auto</button>
 <button onclick="c('page next').then(poll)">next &rsaquo;</button></div>
 <div class=g4 id=pagebtns></div>
 <div id=pageinfo></div>
 <button style="width:100%;margin-top:8px" onclick="c('notifs wipe').then(poll)">clear notification history</button></div>

<div class="card g" id=moods></div>

<div class="card row"><input id=msg type=text maxlength=60 placeholder="say something"><button onclick="say()">say</button></div>

<details class=card><summary>DRAW</summary>
 <div class=row style="margin-bottom:10px"><input type=color id=pen value="#000000">
  <button style="flex:1" onclick="clr()">clear</button><button id=blb onclick="c('bl '+(st.bl?0:1)).then(poll)">backlight</button></div>
 <canvas id=cv width=240 height=240></canvas></details>
</main>
<script>
const MOODS=['idle','think','work','wait','happy','sleep'];let st={mood:'idle',bl:1};
const c=s=>fetch('/cmd?c='+encodeURIComponent(s));
for(const p of ['clawd','clock','usage','timer','music','stats','history','weather','calendar','events','notifs']){const b=document.createElement('button');b.textContent=p;b.id='p-'+p;
 b.onclick=()=>c('page '+p).then(poll);pagebtns.append(b)}
for(const m of MOODS){const b=document.createElement('button');b.textContent=m;b.id='m-'+m;
 b.onclick=()=>c('mood '+m).then(poll);moods.append(b)}
function say(){if(msg.value)c('say '+msg.value).then(poll);msg.value=''}
msg.onkeydown=e=>{if(e.key==='Enter')say()};

async function poll(){try{st=await(await fetch('/state')).json()}catch(e){net.textContent='offline';return}
 net.textContent=st.ip&&st.ip!=='-'?'home wifi '+st.ip:'hotspot only';
 moodname.textContent=st.mood;cap.textContent=st.cap||'';
 for(const m of MOODS)document.getElementById('m-'+m).classList.toggle('on',st.mood===m);
 for(const p of ['auto','clawd','clock','usage','timer','music','stats','history','weather','calendar','events','notifs'])document.getElementById('p-'+p).classList.toggle('on',st.pin===p);
 pageinfo.textContent=st.page?'showing '+st.page+(st.pin==='auto'?' \u00b7 pages take turns':' \u00b7 pinned'):'';
 blb.classList.toggle('on',!!st.bl);
 const rows=[['5h',st.u5],['week',st.uw],['context',st.uc]].filter(r=>r[1]>=0);
 usage.hidden=!rows.length;
 bars.innerHTML=rows.map(([k,v])=>`<span>${k}</span><div class=bar><i class="${v>=90?'crit':v>=70?'warn':''}" style="width:${v}%"></i></div><span>${v}%</span>`).join('');
 reset.textContent=st.reset?'5h window resets at '+st.reset:''}
poll();setInterval(poll,2000);

// Live mirror of the Mochi's screen (120x120 RGB565 from /snap). The panel shows reds much warmer
// than a monitor (Clawd's 218,17,0 looks like 217,119,87), so red is bled into green/blue to match.
const g=pv.getContext('2d'),img=g.createImageData(120,120);
async function snap(){try{const b=new Uint16Array(await(await fetch('/snap')).arrayBuffer());
 if(b.length===14400){for(let i=0;i<b.length;i++){const p=b[i],o=i*4;
  const r=(p>>11)<<3,gg=((p>>5)&63)<<2,bb=(p&31)<<3;
  img.data[o]=r;img.data[o+1]=gg+.47*r*(1-gg/255);img.data[o+2]=bb+.4*r*(1-bb/255);img.data[o+3]=255}
  g.putImageData(img,0,0)}}catch(e){}
 setTimeout(snap,500)}
snap();

// Drawing pad mirrors onto the device.
const x=cv.getContext('2d');let pts=[],last=null;
function clr(){x.fillStyle='#da1100';x.fillRect(0,0,240,240);c('clear da1100')}
const pos=e=>{const r=cv.getBoundingClientRect();return[Math.round(e.clientX-r.left),Math.round(e.clientY-r.top)]};
function flush(){if(!pts.length)return;c('stroke '+pen.value.slice(1)+' '+pts.map(p=>p.join(',')).join(';'));pts=[pts[pts.length-1]]}
cv.onpointerdown=e=>{cv.setPointerCapture(e.pointerId);last=pos(e);pts=[last]};
cv.onpointermove=e=>{if(!last)return;const p=pos(e);
 x.strokeStyle=pen.value;x.lineWidth=3;x.lineCap='round';x.beginPath();x.moveTo(...last);x.lineTo(...p);x.stroke();
 last=p;pts.push(p);if(pts.length>=20)flush()};
cv.onpointerup=cv.onpointercancel=()=>{flush();last=null;pts=[]};
</script></body></html>)HTML";
