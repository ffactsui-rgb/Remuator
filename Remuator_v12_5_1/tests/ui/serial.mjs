import {JSDOM} from 'jsdom';
import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
const root=new URL('../../data/',import.meta.url).pathname;
const dom=new JSDOM(readFileSync(root+'serial.html','utf8'),{url:'http://192.168.4.1/settings/serial',runScripts:'outside-only'});
Object.assign(globalThis,{window:dom.window,document:dom.window.document,location:dom.window.location,FormData:dom.window.FormData});
let serialOn=true;const posts=[];
globalThis.fetch=async(url,options)=>{
 if(options?.method==='POST') {posts.push(options.body.get('enabled'));serialOn=options.body.get('enabled')==='1';}
 return {ok:true,text:async()=>url==='/api/serial'?'Журнал запуска':JSON.stringify({ok:true,serialOn})};
};
const appCode=readFileSync(root+'app.js','utf8');
const appUrl='data:text/javascript;base64,'+Buffer.from(appCode).toString('base64');
const serialCode=readFileSync(root+'serial.js','utf8').replace('"./app.js"',JSON.stringify(appUrl));
await import('data:text/javascript;base64,'+Buffer.from(serialCode).toString('base64'));
const wait=()=>new Promise(r=>setTimeout(r,30));await wait();
const checkbox=document.getElementById('serialEnabled'),form=document.getElementById('serialSettings');
assert(checkbox.checked);assert(!form.querySelector('fieldset').disabled);
checkbox.checked=false;form.dispatchEvent(new window.Event('submit',{cancelable:true}));await wait();
assert(!serialOn&&!checkbox.checked);assert.equal(posts[0],'0');assert.match(document.getElementById('serialOutput').textContent,/Журнал запуска/);
checkbox.checked=true;form.dispatchEvent(new window.Event('submit',{cancelable:true}));await wait();assert(serialOn&&checkbox.checked);assert.equal(posts[1],'1');
// Основная форма также восстанавливает true/false из поля serialOn.
document.body.innerHTML=readFileSync(root+'settings.html','utf8');
const {populate}=await import(appUrl);populate(document.getElementById('settingsForm'),{serialOn:true});assert(document.querySelector('[name="serialOn"]').checked);
populate(document.getElementById('settingsForm'),{serialOn:false});assert(!document.querySelector('[name="serialOn"]').checked);
dom.window.close();console.log('PASS: Serial settings load, off/on posts, boot log independent, main settings checkbox round trip');
