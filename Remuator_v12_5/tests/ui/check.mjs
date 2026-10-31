import {JSDOM} from 'jsdom';
import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
const root = new URL('../../data/', import.meta.url).pathname;
function setup(html,url='/settings/uart') {
 const dom=new JSDOM(readFileSync(root+html,'utf8'),{url:'http://192.168.4.1'+url,runScripts:'outside-only'});
 Object.assign(globalThis,{window:dom.window,document:dom.window.document,location:dom.window.location,FormData:dom.window.FormData});
 return dom;
}
const dom=setup('uart.html');
const calls=[];
const config={model:2209,address:1,rms:900,hold:0.5,holdPercent:50,holdEnabled:false,micro:16,toff:5,blank:24,hend:0,hstart:4,pwmo:36,pwmgrad:14,pwmfreq:1,pwmreg:8,pwmlim:12,freewheel:0,delay:2,stealth:false,autoscale:true,autograd:false,intpol:true,iscale:false,irsense:false,msfilter:false};
globalThis.fetch=async(url,opts)=>{
 calls.push([String(url),opts]);
 return {ok:true,text:async()=>JSON.stringify({ok:true,baud:115200,nak:{...config,model:2208,address:0},pov:config,na:1234,ns:5678,pa:2345,ps:6789,nakConnected:true,povConnected:true})};
};
const appCode=readFileSync(root+'app.js','utf8');
const appUrl='data:text/javascript;base64,'+Buffer.from(appCode).toString('base64');
const app=await import(appUrl);
let uart=readFileSync(root+'uart.js','utf8').replace('"./app.js"',JSON.stringify(appUrl));
await import('data:text/javascript;base64,'+Buffer.from(uart).toString('base64'));
await new Promise(r=>setTimeout(r,20));
const form=document.getElementById('nak'),e=form.elements;
assert.equal(e.na.value,'1234');assert.equal(e.ns.value,'5678');
assert(e.holdPercent.disabled);assert(e.pwmo.disabled);assert(!e.hend.disabled);
e.holdEnabled.checked=true;e.stealth.checked=true;form.dispatchEvent(new window.Event('change'));
assert(!e.holdPercent.disabled);assert(!e.pwmo.disabled);assert(e.hend.disabled);
e.autograd.checked=true;form.dispatchEvent(new window.Event('change'));assert(e.pwmgrad.disabled);
e.autoscale.checked=false;form.dispatchEvent(new window.Event('change'));assert(e.autograd.disabled);assert(e.pwmreg.disabled);assert(!e.pwmgrad.disabled);
// Значения неактивных полей должны сохраняться при выборе другого режима.
e.stealth.checked=false;form.dispatchEvent(new window.Event('change'));
form.dispatchEvent(new window.Event('submit',{cancelable:true}));
await new Promise(r=>setTimeout(r,20));
const save=calls.find(([url,opts])=>url==='/settings/uart/save'&&opts.method==='POST');assert(save);
assert.equal(save[1].body.get('pwmo'),'36');assert.equal(save[1].body.get('holdEnabled'),'on');
app.showError(new Error('Failed to fetch'));assert.match(document.getElementById('error').textContent,/Как исправить:.*Wi-Fi/);
// Кнопки подсказок доступны независимо от заблокированных полей.
dom.window.HTMLDialogElement.prototype.showModal=function(){this.open=true;};
dom.window.HTMLDialogElement.prototype.close=function(){this.open=false;};
dom.window.eval(readFileSync(root+'parameter-help.js','utf8'));
assert.equal(document.querySelectorAll('#nak label').length,document.querySelectorAll('#nak .parameter-help').length);
document.querySelector('#nak .parameter-help').click();assert(document.querySelector('dialog').open);
const programs=setup('programs.html','/programs');
// Повторная привязка общих ссылок в этом документе DOM.
programs.window.eval(appCode.replaceAll('export ',''));
assert.equal(document.querySelector('[data-manager-dir]').href,'http://192.168.4.1:8080/?dir=%2Fprog');
assert.match(app.programCards({programs:[{name:'A',path:'/prog/a.csv',count:1,seconds:30}],invalid:[],selectedPath:'/prog/a.csv'}),/selected-program/);
const settings=setup('settings.html','/settings');assert(!document.querySelector('[name="na"]'));assert(document.getElementById('reboot'));
const homeDom=setup('index.html','/');
const homeCode=readFileSync(root+'home.js','utf8').replace('"./app.js"',JSON.stringify(appUrl)).replace('setInterval(updateClock, 250);','').replace('\npoll();','\n').replaceAll('setInterval(', 'window.setInterval(')+'\nexport {render, pollTimer, freezeConnection, liveStatus, load, connectionLost, updateClock};';
const home=await import('data:text/javascript;base64,'+Buffer.from(homeCode).toString('base64'));
const state={running:false,programs:[],invalid:[],rows:[],selectedCount:0,poseY:25,targetY:30,actualY:24,poseRotation:0,epoch:1800000000,timeValid:true,calibrationReady:true};
home.render(state);
assert.equal(document.getElementById('posePanel').hidden,false);
assert.equal(document.getElementById('manualPanel').hidden,false);
assert.equal(document.getElementById('bottleRotation').getAttribute('transform'),'rotate(180 130 100)');
assert.match(document.getElementById('tiltValue').textContent,/30°.*реальный: 24,0°/);
home.render({...state,running:true,poseRotation:90,rows:[{number:1,incY:0,y:25,incRot:1,rot:90,orientation:90,pause:'00:00:30',start:1800000000}],currentStep:1});
assert.equal(document.getElementById('bottleRotation').getAttribute('transform'),'rotate(270 130 100)');
assert.equal(document.getElementById('manualPanel').hidden,true);
assert(document.querySelector('#schedule path[d="M20 35 V25"][transform="rotate(90 20 20)"]'));
home.render({...state,running:true,actualY:null,yBySteps:true,estimatedY:31});
assert.match(document.getElementById('tiltValue').textContent,/по карте: 31/);
assert.match(document.getElementById('poseNote').textContent,/Движение Y выполняется по сохранённой карте шагов/);
home.render({...state,actualY:24.7,targetY:30.4,yBySteps:true,estimatedY:50});assert.match(document.getElementById('tiltValue').textContent,/30,4°.*реальный: 24,7°/);
assert.equal(document.getElementById('bottleTilt').getAttribute('transform'),'rotate(-24.7 65 170)');
home.render({...state,actualY:null});assert.match(document.getElementById('tiltValue').textContent,/реальный: нет данных/);
// Разметка версии 12 и запуск из карточки с передачей черновика редактора.
assert(document.querySelector('.clock-panel').compareDocumentPosition(document.querySelector('h1')) & window.Node.DOCUMENT_POSITION_FOLLOWING);
assert(document.getElementById('manualPanel').compareDocumentPosition(document.getElementById('posePanel')) & window.Node.DOCUMENT_POSITION_FOLLOWING);
const cardState={...state,selectedPath:'/prog/a.csv',programs:[{name:'A',path:'/prog/a.csv',count:1,seconds:30},{name:'B',path:'/prog/b.csv',count:1,seconds:30}]};
home.render(cardState);
const startButton=document.querySelector('[data-start-program="/prog/b.csv"]');assert(startButton&&!startButton.disabled);
const frame=document.getElementById('editorFrame');
frame.contentWindow.postMessage=message=>setTimeout(()=>window.dispatchEvent(new window.MessageEvent('message',{origin:location.origin,source:frame.contentWindow,data:{type:'remuage-flush-response',token:message.token,ok:true}})),0);
const launchCalls=[];
globalThis.fetch=async(url,opts)=>{
 launchCalls.push([String(url),opts]);
 return {ok:true,text:async()=>JSON.stringify(String(url)==='/api/live'?{...cardState,bootId:1,cycle:launchCalls.length,currentStep:1,moving:false,running:true,actualY:26}:String(url)==='/api/home'?{...cardState,selectedPath:'/prog/b.csv',running:true}: {ok:true})};
};
startButton.click();startButton.click();
await new Promise(r=>setTimeout(r,50));
assert.equal(launchCalls.filter(([url])=>url.startsWith('/program/select?')).length,1);
assert.equal(launchCalls[0][0],'/program/select?path=%2Fprog%2Fb.csv');
const launched=launchCalls.find(([url])=>url==='/api/program');assert(launched);assert.equal(launched[1].body.get('action'),'start');assert.equal(launched[1].body.get('step'),'1');
// Старый ответ не должен снимать блокировку панели; новый проход цикла должен её снять.
await home.liveStatus();
home.freezeConnection();assert(home.connectionLost);const frozenClock=document.getElementById('clock').textContent;home.updateClock();assert.equal(document.getElementById('clock').textContent,frozenClock);
const sameCycle=launchCalls.length;
globalThis.fetch=async()=>({ok:true,text:async()=>JSON.stringify({...cardState,bootId:1,cycle:sameCycle,currentStep:1,moving:true,running:true,actualY:27})});
assert.equal(await home.liveStatus(),null);assert(home.connectionLost);
globalThis.fetch=async()=>({ok:true,text:async()=>JSON.stringify({...cardState,bootId:2,cycle:1,currentStep:1,moving:true,running:true,actualY:28})});
await home.load();assert(!home.connectionLost);assert.match(document.getElementById('tiltValue').textContent,/28/);
clearTimeout(home.pollTimer);
homeDom.window.close();
console.log('PASS: live DOM TMC modes, inactive value preservation, error remedies, help dialog, program directory and selected tile, settings relocation, home Z zero/direction and real Y');
dom.window.close();programs.window.close();settings.window.close();
