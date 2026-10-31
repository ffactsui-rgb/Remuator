(async()=>{
  const v=document.getElementById('aboutVersion'),r=document.getElementById('readmeContents');
  try {const res=await fetch('/api/version',{cache:'no-store'});if(!res.ok)throw Error('HTTP '+res.status);v.textContent='Remuage-VRN · v'+(await res.json()).version;}
  catch(e){v.textContent='Версия недоступна: '+e.message;}
  try {const res=await fetch('/api/readme',{cache:'no-store'});if(!res.ok)throw Error('HTTP '+res.status);r.textContent=await res.text();}
  catch(e){r.textContent='Не удалось загрузить README_RU.md: '+e.message;}
})();
