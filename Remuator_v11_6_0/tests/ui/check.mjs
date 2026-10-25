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
// Inactive values must still survive saving a different mode.
e.stealth.checked=false;form.dispatchEvent(new window.Event('change'));
form.dispatchEvent(new window.Event('submit',{cancelable:true}));
await new Promise(r=>setTimeout(r,20));
const save=calls.find(([url,opts])=>url==='/settings/uart/save'&&opts.method==='POST');assert(save);
assert.equal(save[1].body.get('pwmo'),'36');assert.equal(save[1].body.get('holdEnabled'),'on');
app.showError(new Error('Failed to fetch'));assert.match(document.getElementById('error').textContent,/Как исправить:.*Wi-Fi/);
// Question buttons are usable independently of disabled inputs.
dom.window.HTMLDialogElement.prototype.showModal=function(){this.open=true;};
dom.window.HTMLDialogElement.prototype.close=function(){this.open=false;};
dom.window.eval(readFileSync(root+'parameter-help.js','utf8'));
assert.equal(document.querySelectorAll('#nak label').length,document.querySelectorAll('#nak .parameter-help').length);
document.querySelector('#nak .parameter-help').click();assert(document.querySelector('dialog').open);
const programs=setup('programs.html','/programs');
// run the shared links binding again in this DOM
programs.window.eval(appCode.replaceAll('export ',''));
assert.equal(document.querySelector('[data-manager-dir]').href,'http://192.168.4.1:8080/?dir=%2Fprog');
assert.match(app.programCards({programs:[{name:'A',path:'/prog/a.csv',count:1,seconds:30}],invalid:[],selectedPath:'/prog/a.csv'}),/selected-program/);
const settings=setup('settings.html','/settings');assert(!document.querySelector('[name="na"]'));assert(document.getElementById('reboot'));
const homeDom=setup('index.html','/');
const homeCode=readFileSync(root+'home.js','utf8').replace('"./app.js"',JSON.stringify(appUrl)).replace('setInterval(updateClock, 250);','').replace('\npoll();','\n')+'\nexport {render};';
const home=await import('data:text/javascript;base64,'+Buffer.from(homeCode).toString('base64'));
const state={running:false,programs:[],invalid:[],rows:[],selectedCount:0,poseY:25,targetY:30,actualY:24,poseRotation:0,epoch:1800000000,timeValid:true,calibrationReady:true};
home.render(state);
assert.equal(document.getElementById('posePanel').hidden,false);
assert.equal(document.getElementById('bottleRotation').getAttribute('transform'),'rotate(180 130 100)');
assert.match(document.getElementById('tiltValue').textContent,/30°.*реальный: 24°/);
home.render({...state,running:true,poseRotation:90,rows:[{number:1,incY:0,y:25,incRot:1,rot:90,orientation:90,pause:'00:00:30',start:1800000000}],currentStep:1});
assert.equal(document.getElementById('bottleRotation').getAttribute('transform'),'rotate(270 130 100)');
assert(document.querySelector('#schedule path[d="M20 35 V25"][transform="rotate(90 20 20)"]'));
home.render({...state,actualY:null});assert.match(document.getElementById('tiltValue').textContent,/нет данных/);
homeDom.window.close();
console.log('PASS: live DOM TMC modes, inactive value preservation, error remedies, help dialog, program directory and selected tile, settings relocation, home Z zero/direction and real Y');
dom.window.close();programs.window.close();settings.window.close();
