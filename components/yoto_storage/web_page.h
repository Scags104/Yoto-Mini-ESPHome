#pragma once

namespace esphome {
namespace yoto_storage {

// Admin page served at http://<device>:<port>/ (login via HTTP Basic auth)
static const char WEB_PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Yoto</title>
<style>
:root{--bg:#18191c;--side:#1e1f23;--card:#26272c;--line:#33343a;--fg:#ececef;--mut:#9b9ca3;--acc:#4f9d98;--acc2:#6cc3bc;--danger:#e5675c;--r:18px}
*{box-sizing:border-box}html,body{margin:0;height:100%}
body{font:15px/1.45 -apple-system,system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg);display:flex}
a{color:var(--acc2)}
aside{width:300px;flex:none;background:var(--side);border-right:1px solid var(--line);display:flex;flex-direction:column;height:100vh;position:sticky;top:0}
.brand{display:flex;gap:12px;align-items:center;padding:20px 18px 14px}
.logo{width:42px;height:42px;border-radius:12px;background:linear-gradient(135deg,#f08a4b,#4f9d98);display:grid;place-items:center;font-size:20px}
.brand b{display:block;font-size:17px}.brand small{color:var(--mut)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#5cc06d;margin-left:6px;vertical-align:middle}
nav{overflow:auto;flex:1;padding:4px 10px 16px}
.grp{color:var(--mut);font-size:12px;letter-spacing:.08em;text-transform:uppercase;margin:16px 10px 6px}
.nav{display:flex;gap:12px;align-items:center;padding:9px 10px;border-radius:14px;cursor:pointer;color:var(--fg);text-decoration:none}
.nav:hover{background:#25262b}.nav.on{background:#2c2d33}
.ic{width:36px;height:36px;border-radius:50%;display:grid;place-items:center;font-size:17px;flex:none}
.nav b{display:block;font-weight:600}.nav small{color:var(--mut);font-size:12.5px}
.foot{padding:12px 18px;color:var(--mut);font-size:12.5px;border-top:1px solid var(--line)}
main{flex:1;min-width:0;padding:18px 28px 60px;max-width:980px}
h1{font-size:22px;font-weight:600;margin:6px 0 4px;display:flex;gap:10px;align-items:center}
.sec{color:var(--acc2);font-size:13.5px;margin:22px 4px 8px}
.card{background:var(--card);border-radius:var(--r);padding:4px 20px;margin-bottom:12px}
.row{display:flex;gap:16px;align-items:center;justify-content:space-between;padding:15px 0;border-bottom:1px solid var(--line)}
.row:last-child{border-bottom:0}.row.col{flex-direction:column;align-items:stretch}
.lbl b{display:block;font-weight:600}.lbl small{color:var(--mut)}
.val{color:var(--fg);font-weight:600;white-space:nowrap}
.top{display:flex;justify-content:space-between;align-items:baseline;gap:12px}
input[type=range]{width:100%;accent-color:var(--acc);margin-top:8px}
select,.btn,input[type=text]{background:#2f3036;color:var(--fg);border:1px solid #3c3d44;border-radius:12px;padding:9px 12px;font:inherit}
.btn{cursor:pointer}.btn.pri{background:var(--acc);border-color:var(--acc);color:#fff}.btn.dng{color:var(--danger)}
.sw{position:relative;width:52px;height:30px;flex:none}.sw input{display:none}
.sw span{position:absolute;inset:0;background:#4a4b52;border-radius:30px;transition:.2s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:24px;height:24px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
.sw input:checked+span{background:var(--acc)}.sw input:checked+span:before{transform:translateX(22px)}
.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));gap:12px}
.tile{background:var(--card);border-radius:var(--r);padding:16px}.tile small{color:var(--mut)}.tile b{display:block;font-size:20px;margin-top:4px}
table{width:100%;border-collapse:collapse}td{padding:10px 4px;border-bottom:1px solid var(--line)}
td.s{color:var(--mut);text-align:right;white-space:nowrap}td.a{text-align:right;white-space:nowrap}
td a{color:var(--fg);text-decoration:none}
.bar{display:flex;gap:8px;flex-wrap:wrap;margin:6px 0 12px}
textarea{width:100%;min-height:55vh;background:#1d1e22;color:var(--fg);border:1px solid var(--line);border-radius:14px;padding:14px;font:13.5px/1.5 ui-monospace,Menlo,Consolas,monospace}
#status{color:var(--mut);min-height:1.4em;margin:6px 4px}progress{width:100%;accent-color:var(--acc)}
.hide{display:none}
@media(max-width:820px){body{display:block}aside{width:auto;height:auto;position:static}nav{display:flex;overflow-x:auto;padding:0 8px 8px}.grp,.nav small,.foot{display:none}.nav{flex:none}main{padding:12px 14px 40px}}
</style></head><body>
<aside>
 <div class="brand"><div class="logo">&#9835;</div><div><b id="dev">Yoto</b><small>Yoto Mini<span class="dot"></span></small></div></div>
 <nav id="nav"></nav>
 <div class="foot" id="foot">ESPHome</div>
</aside>
<main>
 <h1 id="title"></h1>
 <div id="status"></div>
 <div id="view"></div>
</main>
<script>
const $=s=>document.querySelector(s);
const enc=p=>p.split('/').map(encodeURIComponent).join('/');
const join=(a,b)=>(a.endsWith('/')?a:a+'/')+b;
const fmt=n=>n>1048576?(n/1048576).toFixed(1)+' MB':n>1024?(n/1024).toFixed(0)+' KB':n+' B';
const st=t=>$('#status').textContent=t||'';
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const q=s=>esc(JSON.stringify(String(s)));  // safe JS string literal inside a "..." attribute

// Pages, and which settings (by name) live on each. Anything not listed shows under "Other".
const PAGES=[
 {id:'overview',grp:'Device',name:'Overview',sub:'Status at a glance',ic:'&#9776;',col:'#e2804f'},
 {id:'display',grp:'Settings',name:'Display',sub:'Theme, clock, screen, night mode',ic:'&#9728;',col:'#e2804f',
  secs:[['Screen',['Screen','Screen timeout']],['Look',['Theme','Clock style']],['Night mode',['Night mode','Night mode start','Night mode end','Night mode brightness']]]},
 {id:'sound',grp:'Settings',name:'Sound',sub:'Volume',ic:'&#9834;',col:'#d9a23c',secs:[['Volume',['Volume']]]},
 {id:'power',grp:'Settings',name:'Power & Battery',sub:'Charge limit, auto power-off',ic:'&#9889;',col:'#5aa26a',
  secs:[['Battery',['Charge limit']],['Power',['Auto power-off','Restart']]]},
 {id:'conn',grp:'Settings',name:'Bluetooth',sub:'Phone audio, away mode',ic:'&#8635;',col:'#5b8fd6',secs:[['Bluetooth',['Bluetooth','Bluetooth when away']]]},
 {id:'other',grp:'Settings',name:'Other',sub:'Everything else',ic:'&#8943;',col:'#8a7fd1'},
 {id:'files',grp:'Storage',name:'Files',sub:'Browse, upload, play',ic:'&#128193;',col:'#4f9d98'},
 {id:'radio',grp:'Storage',name:'Radio stations',sub:'Edit stations.txt',ic:'&#128251;',col:'#c76a9a'},
];
const DESC={
 'Screen':'Backlight on/off and brightness.',
 'Screen timeout':'Turn the screen off after this many minutes without a touch (0 = never).',
 'Theme':'Colours used on the screen.','Clock style':'How the idle clock looks.',
 'Night mode':'Switch to the dim red Night theme between the hours below.',
 'Night mode start':'Hour night mode begins (24 h).','Night mode end':'Hour night mode ends (24 h).',
 'Night mode brightness':'Screen brightness while night mode is active.',
 'Volume':'Playback volume for the speaker and headphones.',
 'Charge limit':'Stop charging at this level to extend battery life (100 = off).',
 'Auto power-off':'Power off after this many idle minutes on battery (0 = never).',
 'Restart':'Restart the Yoto.',
 'Bluetooth':'Let a phone stream audio to the Yoto.',
 'Bluetooth when away':'Turn Bluetooth on automatically when home Wi-Fi is gone.',
};
const OVERVIEW=['Battery','Charging state','Wi-Fi signal','IP address','Storage free','Uptime','Headphones','Bluetooth connected','Last tag','Battery voltage'];
let data=null,page='overview',cur='/',editPath=null,timer=null;

function navInit(){let h='',g='';for(const p of PAGES){if(p.grp!==g){g=p.grp;h+=`<div class="grp">${g}</div>`}
 h+=`<a class="nav" data-id="${p.id}"><span class="ic" style="background:${p.col}33;color:${p.col}">${p.ic}</span><span><b>${p.name}</b><small>${p.sub}</small></span></a>`}
 $('#nav').innerHTML=h;document.querySelectorAll('.nav').forEach(a=>a.onclick=()=>go(a.dataset.id));}
function go(id){page=id;editPath=null;document.querySelectorAll('.nav').forEach(a=>a.classList.toggle('on',a.dataset.id===id));
 const p=PAGES.find(x=>x.id===id);$('#title').innerHTML=`<span class="ic" style="background:${p.col}33;color:${p.col}">${p.ic}</span>${p.name}`;st('');
 if(id==='files')loadDir(cur);else if(id==='radio')openEditor('/stations.txt');else render();}
async function load(){try{const r=await fetch('/api/settings',{cache:'no-store'});if(!r.ok)throw r.status;data=await r.json();
 $('#dev').textContent=data.device||'Yoto';$('#foot').textContent='Connected · '+(data.device||'');
 const a=document.activeElement;if(['overview','display','sound','power','conn','other'].includes(page)&&!(a&&['INPUT','SELECT'].includes(a.tagName)))render();}catch(e){st('Could not load settings ('+e+')')}}
const S=n=>data&&data.settings.find(s=>s.n===n);
function ctl(s){const id='c'+Math.random().toString(36).slice(2,8),d=DESC[s.n]||'';
 if(s.t==='switch')return `<div class="row"><div class="lbl"><b>${esc(s.n)}</b><small>${d}</small></div><label class="sw"><input type="checkbox" ${s.v?'checked':''} onchange="setv('switch',${q(s.n)},this.checked?1:0)"><span></span></label></div>`;
 if(s.t==='select')return `<div class="row"><div class="lbl"><b>${esc(s.n)}</b><small>${d}</small></div><select onchange="setv('select',${q(s.n)},this.value)">${s.o.map(o=>`<option ${o===s.v?'selected':''}>${esc(o)}</option>`).join('')}</select></div>`;
 if(s.t==='button')return `<div class="row"><div class="lbl"><b>${esc(s.n)}</b><small>${d}</small></div><button class="btn" onclick="if(confirm(${q(s.n+'?')}))setv('button',${q(s.n)},1)">${esc(s.n)}</button></div>`;
 if(s.t==='light')return `<div class="row col"><div class="top"><div class="lbl"><b>${esc(s.n)}</b><small>${d}</small></div><label class="sw"><input type="checkbox" ${s.on?'checked':''} onchange="setv('light',${q(s.n)},this.checked?'on':'off')"><span></span></label></div>
  <div class="top"><small style="color:var(--mut)">Brightness</small><span class="val" id="${id}">${Math.round(s.v)}%</span></div><input type="range" min="1" max="100" value="${Math.round(s.v)}" oninput="$('#${id}').textContent=this.value+'%'" onchange="setv('light',${q(s.n)},this.value)"></div>`;
 const min=s.t==='volume'?0:s.min,max=s.t==='volume'?100:s.max,step=s.t==='volume'?5:s.step,u=s.t==='volume'?'%':(s.u||'');
 return `<div class="row col"><div class="top"><div class="lbl"><b>${esc(s.n)}</b><small>${d}</small></div><span class="val" id="${id}">${+(+s.v).toFixed(2)} ${esc(u)}</span></div>
  <input type="range" min="${min}" max="${max}" step="${step}" value="${s.v}" oninput="$('#${id}').textContent=this.value+' ${esc(u)}'" onchange="setv('${s.t}',${q(s.n)},this.value)"></div>`;}
async function setv(t,n,v){st('Saving…');try{const r=await fetch(`/api/set?t=${t}&n=${encodeURIComponent(n)}&v=${encodeURIComponent(v)}`,{method:'POST'});st(r.ok?'Saved':'Failed');setTimeout(load,600)}catch(e){st('Failed')}}
function render(){if(!data){$('#view').innerHTML='';return}const v=$('#view');
 if(page==='overview'){const tiles=OVERVIEW.map(n=>data.status.find(x=>x.n===n)).filter(Boolean);
  const vol=S('Volume');let h=`<div class="tiles">`;
  if(vol)h+=`<div class="tile"><small>Player</small><b>${esc(vol.state||'')}</b></div><div class="tile"><small>Volume</small><b>${Math.round(vol.v)}%</b></div>`;
  for(const t of tiles)h+=`<div class="tile"><small>${esc(t.n)}</small><b>${esc(t.v)}${t.u?' '+esc(t.u):''}</b></div>`;
  h+=`</div><div class="sec">All sensors</div><div class="card">${data.status.map(t=>`<div class="row"><div class="lbl"><b>${esc(t.n)}</b></div><span class="val">${esc(t.v)} ${esc(t.u)}</span></div>`).join('')}</div>`;v.innerHTML=h;return}
 const p=PAGES.find(x=>x.id===page);let h='';
 if(page==='other'){const used=new Set(PAGES.flatMap(x=>(x.secs||[]).flatMap(s=>s[1])));const rest=data.settings.filter(s=>!used.has(s.n));
  h=rest.length?`<div class="card">${rest.map(ctl).join('')}</div>`:'<div class="card"><div class="row">Nothing else to show.</div></div>';}
 else for(const [title,names] of p.secs){const items=names.map(S).filter(Boolean);if(!items.length)continue;h+=`<div class="sec">${title}</div><div class="card">${items.map(ctl).join('')}</div>`}
 v.innerHTML=h;}

// ---- Files ----
async function loadDir(p){cur=p;st('Loading…');
 try{const r=await fetch('/api/list?path='+encodeURIComponent(p));const j=await r.json();if(j.error)throw j.error;
 let h=`<div class="bar"><label class="btn pri">Upload files<input id="up" type="file" multiple hidden></label><button class="btn" onclick="mkdir()">New folder</button><button class="btn" onclick="play(cur)">&#9654; Play this folder</button><button class="btn dng" onclick="clearAll()">Delete all here</button></div>
  <div class="sec">${esc(p)}</div><progress id="prog" value="0" max="100" class="hide"></progress><div class="card"><table><tbody>`;
 if(p!=='/'){const up=p.substring(0,p.lastIndexOf('/'))||'/';h+=`<tr><td><a href="#" onclick="loadDir(${q(up)});return false">&#11025; ..</a></td><td></td><td></td></tr>`}
 for(const e of j.entries){const path=join(p,e.name),txt=/\.(txt|json|yaml|yml|m3u|pls|csv)$/i.test(e.name);
  h+=`<tr><td>${e.dir?`<a href="#" onclick="loadDir(${q(path)});return false">&#128193; ${esc(e.name)}</a>`:`<a href="/file${enc(path)}" download>${e.audio?'&#127925;':'&#128196;'} ${esc(e.name)}</a>`}</td>
  <td class="s">${e.dir?'':fmt(e.size)}</td><td class="a">${(e.dir||e.audio)?`<button class="btn" onclick="play(${q(path)})">&#9654;</button> `:''}${txt?`<button class="btn" onclick="openEditor(${q(path)})">Edit</button> `:''}<button class="btn dng" onclick="del(${q(path)},${e.dir})">Delete</button></td></tr>`}
 h+=`</tbody></table></div>`;$('#view').innerHTML=h;$('#up').onchange=upload;
 st(j.entries.length+' item(s)'+(j.truncated?' – showing the first '+j.entries.length+' only':''));}catch(e){st('Error: '+e)}}
async function play(p){const r=await fetch('/api/play?path='+encodeURIComponent(p),{method:'POST'});st(r.ok?'Playing '+p:'Play failed')}
async function del(p,dir){if(!confirm(dir?'Delete folder '+p+' and everything in it?':'Delete '+p+'?'))return;st('Deleting…');
 const r=await fetch('/file'+enc(p)+(dir?'?recursive=1':''),{method:'DELETE'});const j=await r.json();st(j.error?'Error: '+j.error:'Deleted');loadDir(cur)}
async function clearAll(){if(!confirm('Delete EVERYTHING in '+cur+'?'))return;st('Deleting…');
 const r=await fetch('/api/clear?path='+encodeURIComponent(cur),{method:'POST'});const j=await r.json();st(j.error?'Error: '+j.error:'Deleted '+j.deleted+' item(s)');loadDir(cur)}
async function mkdir(){const n=prompt('Folder name');if(!n)return;await fetch('/api/mkdir?path='+encodeURIComponent(join(cur,n)),{method:'POST'});loadDir(cur)}
function putFile(body,path){return new Promise((res,rej)=>{const x=new XMLHttpRequest();x.open('PUT','/file'+enc(path));const pr=$('#prog');if(pr)pr.classList.remove('hide');
 x.upload.onprogress=e=>{if(pr&&e.lengthComputable)pr.value=e.loaded/e.total*100};x.onload=()=>{if(pr)pr.classList.add('hide');x.status<300?res():rej(x.responseText)};
 x.onerror=()=>{if(pr)pr.classList.add('hide');rej('network error')};x.send(body)})}
async function upload(e){const f=[...e.target.files];for(let i=0;i<f.length;i++){st(`Uploading ${i+1}/${f.length}: ${f[i].name}`);try{await putFile(f[i],join(cur,f[i].name))}catch(err){st('Upload failed: '+err);return}}loadDir(cur)}

// ---- Text editor ----
async function openEditor(path){editPath=path;$('#title').innerHTML='&#9998; Edit '+esc(path);
 let txt='';try{const r=await fetch('/file'+enc(path),{cache:'no-store'});if(r.ok)txt=await r.text();else if(path==='/stations.txt')
  txt='# Genre | Name | stream URL   (or: Name | stream URL)\n# MP3 streams work best; .m3u and .pls links also work.\n\n';}catch(e){}
 const back=page==='radio'?'':`<button class="btn" onclick="loadDir(cur)">Back to files</button>`;
 $('#view').innerHTML=`<div class="bar"><button class="btn pri" onclick="saveEditor()">Save</button>${back}</div><textarea id="ed" spellcheck="false"></textarea>`;
 $('#ed').value=txt;st(path==='/stations.txt'?'One station per line: Genre | Name | URL':'');}
async function saveEditor(){const t=$('#ed').value;if(t.length>65536){st('Too large (64 KB max)');return}
 st('Saving…');try{await putFile(new Blob([t],{type:'text/plain'}),editPath);st('Saved '+editPath)}catch(e){st('Save failed: '+e)}}

navInit();go('overview');load();timer=setInterval(()=>{if(!document.hidden)load()},5000);
</script></body></html>)HTML";

}  // namespace yoto_storage
}  // namespace esphome
