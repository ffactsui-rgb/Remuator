import {$,request,showError,clearError} from './app.js';
let state=null, held=null, hb=0, queue=Promise.resolve(), pending=false;
const requestAction=(action, extra={})=>request('/api/calibration/z',{action,...extra});
async function load(){
 try{
  const r=await request('/api/calibration/z'); state=r;
  if(document.activeElement!==$('micro'))$('micro').value=String(r.micro);
  $('calState').textContent=r.valid ? `Z откалиброван: ${r.steps360} STEP / 360° при микрошаге ${r.micro}` :
    `Нужна калибровка Z при микрошаге ${r.micro}. Запуск программы запрещён до сохранения.`;
  $('position').textContent=`${r.steps>0?'+':''}${r.steps} STEP`;
  $('estimated').textContent=r.started ? (r.jogging?'Двигатель движется':'Двигатель остановлен; подведите метку к 360°') : 'Нажмите «Запуск» для нового измерения';
  $('begin').disabled=r.programRunning || r.jogging || pending;
  $('microForm').querySelector('button').disabled=r.programRunning || r.jogging || pending;
  $('left').disabled=$('right').disabled=!r.started || r.programRunning || pending;
  $('save').disabled=!r.started || r.jogging || r.programRunning || pending || Math.abs(r.steps)<100;
  for(const id of ['minus10','minus1','plus1','plus10'])$(id).disabled=!r.started || r.jogging || r.programRunning || pending;
 }catch(e){showError(e);}
}
async function send(action,params={}) {clearError(); pending=true; try{await requestAction(action,params);await load();}catch(e){showError(e);}finally{pending=false;}}
$('microForm').addEventListener('submit', e=>{e.preventDefault();if(!state?.jogging)send('micro',{micro:$('micro').value});});
$('micro').addEventListener('keydown', e=>{if(e.key==='Enter'){e.preventDefault();$('microForm').requestSubmit();}});
$('begin').onclick=()=>{if(confirm('Обнулить счётчик Z в текущем физическом положении?'))send('start');};
$('save').onclick=()=>{
 if(!state?.started||!confirm(`Метка действительно сделала РОВНО 360°? Сохранить ${Math.abs(state.steps)} STEP?`))return;
 send('save');
};
for(const [id,steps] of [['minus10',-10],['minus1',-1],['plus1',1],['plus10',10]]){
 $(id).onclick=()=>{queue=queue.catch(()=>{}).then(()=>requestAction('nudge',{steps})).then(load).catch(showError);};
}
$('stop').onclick=()=>stop();
$('cancel').onclick=()=>{if(confirm('Отменить текущую калибровку Z, оставив прежние сохранённые значения?')){
 held=null;clearInterval(hb);hb=0;queue=queue.catch(()=>{}).then(()=>requestAction('cancel')).then(load).catch(showError);
}};
function stop(){held=null; clearInterval(hb); hb=0;
 queue=queue.catch(()=>{}).then(()=>requestAction('stop')).then(load).catch(showError);
}
function beginJog(dir,event){
 if(held||!state?.started||state.programRunning||pending||event.button!==0)return;
 event.preventDefault();held=dir;event.currentTarget.setPointerCapture(event.pointerId);
 queue=queue.catch(()=>{}).then(()=>requestAction(dir)).catch(err=>{held=null;showError(err);});
 clearInterval(hb);
 hb=setInterval(()=>{if(held)queue=queue.catch(()=>{}).then(()=>requestAction('heartbeat')).catch(()=>stop());},600);
}
for(const dir of ['left','right']){
 const b=$(dir);b.addEventListener('pointerdown',e=>beginJog(dir,e));
 for(const ev of ['pointerup','pointercancel','lostpointercapture'])b.addEventListener(ev,()=>{if(held===dir)stop();});
 b.addEventListener('contextmenu',e=>e.preventDefault());
}
window.addEventListener('blur',()=>{if(held)stop();});
document.addEventListener('visibilitychange',()=>{if(document.hidden&&held)stop();});
async function refresh(){await load();setTimeout(refresh,state?.jogging?400:1000);}refresh();
