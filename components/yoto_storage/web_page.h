#pragma once

namespace esphome {
namespace yoto_storage {

// Single-page file manager served at http://<device>:<port>/
static const char WEB_PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Yoto storage</title>
<style>
:root{--bg:#fff;--fg:#1c1c1e;--muted:#6e6e73;--line:#e5e5ea;--accent:#0a84ff;--danger:#ff3b30;--card:#f5f5f7}
@media (prefers-color-scheme:dark){:root{--bg:#000;--fg:#f2f2f7;--muted:#98989d;--line:#2c2c2e;--card:#1c1c1e}}
*{box-sizing:border-box}body{margin:0;font:15px/1.4 -apple-system,system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:16px;border-bottom:1px solid var(--line);display:flex;gap:8px;flex-wrap:wrap;align-items:center}
h1{font-size:18px;margin:0 12px 0 0}#crumbs a{color:var(--accent);text-decoration:none}
main{padding:8px 16px 32px;max-width:900px;margin:0 auto}
.bar{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}
button,label.btn{border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:8px;padding:6px 12px;font:inherit;cursor:pointer}
button.primary,label.btn{background:var(--accent);border-color:var(--accent);color:#fff}
button.danger{color:var(--danger)}
table{width:100%;border-collapse:collapse}td{padding:8px 6px;border-bottom:1px solid var(--line);vertical-align:middle}
td.n{word-break:break-word}td.s{color:var(--muted);white-space:nowrap;text-align:right}td.a{white-space:nowrap;text-align:right}
td.n a{color:var(--fg);text-decoration:none}td.n a:hover{text-decoration:underline}
#status{color:var(--muted);min-height:1.4em}progress{width:100%}
</style></head><body>
<header><h1>Yoto storage</h1><div id="crumbs"></div></header>
<main>
<div class="bar">
<label class="btn">Upload files<input id="up" type="file" multiple hidden></label>
<button onclick="mkdir()">New folder</button>
<button onclick="play(cur)">Play this folder</button>
<button onclick="load(cur)">Refresh</button>
<button class="danger" onclick="clearAll()">Delete all here</button>
</div>
<div id="status"></div><progress id="prog" value="0" max="100" hidden></progress>
<table><tbody id="list"></tbody></table>
</main>
<script>
let cur='/';
const enc=p=>p.split('/').map(encodeURIComponent).join('/');
const join=(a,b)=>(a.endsWith('/')?a:a+'/')+b;
const fmt=n=>n>1048576?(n/1048576).toFixed(1)+' MB':n>1024?(n/1024).toFixed(0)+' KB':n+' B';
const st=t=>document.getElementById('status').textContent=t;
function crumbs(p){const c=document.getElementById('crumbs');c.innerHTML='';
 const parts=p.split('/').filter(Boolean);let acc='';
 const add=(name,path)=>{const a=document.createElement('a');a.href='#';a.textContent=name;a.onclick=e=>{e.preventDefault();load(path)};c.appendChild(a)};
 add('/','/');parts.forEach((x,i)=>{acc+='/'+x;if(i)c.append(' / ');add(x,acc)});}
async function load(p){cur=p;crumbs(p);st('Loading…');
 try{const r=await fetch('/api/list?path='+encodeURIComponent(p));const j=await r.json();if(j.error)throw j.error;
  const tb=document.getElementById('list');tb.innerHTML='';
  if(p!=='/'){const up=p.substring(0,p.lastIndexOf('/'))||'/';row('..',true,0,false,up,true)}
  j.entries.forEach(e=>row(e.name,e.dir,e.size,e.audio,join(p,e.name)));
  st(j.entries.length+' item(s)'+(j.truncated?' - showing the first '+j.entries.length+' only (use "Delete all here" to clear a huge folder)':''));}catch(e){st('Error: '+e)}}
function row(name,dir,size,audio,path,isUp){const tr=document.createElement('tr');
 const n=document.createElement('td');n.className='n';const a=document.createElement('a');
 a.textContent=(dir?'📁 ':audio?'🎵 ':'📄 ')+name;
 if(dir){a.href='#';a.onclick=e=>{e.preventDefault();load(path)}}else{a.href='/file'+enc(path);a.download=name}
 n.appendChild(a);const s=document.createElement('td');s.className='s';s.textContent=dir?'':fmt(size);
 const ac=document.createElement('td');ac.className='a';
 if(!isUp){if(dir||audio){const b=document.createElement('button');b.textContent='▶ Play';b.onclick=()=>play(path);ac.appendChild(b)}
  const d=document.createElement('button');d.className='danger';d.textContent='Delete';d.onclick=()=>del(path,dir);ac.append(' ',d)}
 tr.append(n,s,ac);document.getElementById('list').appendChild(tr)}
async function play(p){const r=await fetch('/api/play?path='+encodeURIComponent(p),{method:'POST'});st(r.ok?'Playing '+p:'Play failed')}
async function del(p,dir){if(!confirm(dir?'Delete folder '+p+' and everything in it?':'Delete '+p+'?'))return;
 st('Deleting…');const r=await fetch('/file'+enc(p)+(dir?'?recursive=1':''),{method:'DELETE'});const j=await r.json();
 st(j.error?'Error: '+j.error:'Deleted');load(cur)}
async function clearAll(){if(!confirm('Delete EVERYTHING in '+cur+' (files and folders)?'))return;
 st('Deleting…');const r=await fetch('/api/clear?path='+encodeURIComponent(cur),{method:'POST'});const j=await r.json();
 st(j.error?'Error: '+j.error:'Deleted '+j.deleted+' item(s)'+(j.ok?'':' (some items could not be deleted)'));load(cur)}
async function mkdir(){const n=prompt('Folder name');if(!n)return;
 const r=await fetch('/api/mkdir?path='+encodeURIComponent(join(cur,n)),{method:'POST'});st(r.ok?'Created':'Failed');load(cur)}
function putFile(f,path){return new Promise((res,rej)=>{const x=new XMLHttpRequest();x.open('PUT','/file'+enc(path));
 const pr=document.getElementById('prog');pr.hidden=false;
 x.upload.onprogress=e=>{if(e.lengthComputable)pr.value=e.loaded/e.total*100};
 x.onload=()=>{pr.hidden=true;x.status<300?res():rej(x.responseText)};x.onerror=()=>{pr.hidden=true;rej('network error')};x.send(f)})}
document.getElementById('up').onchange=async e=>{const files=[...e.target.files];
 for(let i=0;i<files.length;i++){st('Uploading '+(i+1)+'/'+files.length+': '+files[i].name);
  try{await putFile(files[i],join(cur,files[i].name))}catch(err){st('Upload failed: '+err);break}}
 e.target.value='';load(cur)};
load('/');
</script></body></html>)HTML";

}  // namespace yoto_storage
}  // namespace esphome
