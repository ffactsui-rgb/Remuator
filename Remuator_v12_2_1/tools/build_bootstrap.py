"""Пересоздать самостоятельные страницы восстановления PROGMEM из редактируемых веб-исходников."""
from pathlib import Path
import re
r=Path(__file__).resolve().parents[1]
wifi=(r/'tools/bootstrap-wifi.html').read_text()
manager=(r/'data/filemanager.html').read_text()
manager=re.sub(r'<link[^>]*(?:rel="icon"|rel="shortcut icon")[^>]*>', '', manager)
manager=manager.replace('<link rel="stylesheet" href="/style.css" />','<style>'+(r/'data/style.css').read_text()+'</style>')
manager=manager.replace('<link rel="stylesheet" href="/filemanager.css" />','<style>'+(r/'data/filemanager.css').read_text()+'</style>')
manager=manager.replace('<script src="/site-shell.js" defer></script>','')
manager=manager.replace('<script src="/filemanager.js" defer></script>', '<script>'+(r/'data/site-shell.js').read_text()+'</script><script>'+(r/'data/filemanager.js').read_text()+'</script>')
manager=manager.replace('<h1>Файловый менеджер LittleFS</h1>', '''<h1>Встроенный файловый менеджер LittleFS</h1><section class="card"><p>Эта страница находится в памяти прошивки и работает даже без веб-файлов. Загрузите содержимое data в корень /, а CSV — в /prog. ZIP предварительно распакуйте на компьютере.</p><button id="bootstrapCheck" type="button">Проверить веб-файлы</button><p id="bootstrapState" role="status"></p><a id="bootstrapMain" class="btn" hidden>Перейти на главную</a></section>''')
manager=manager.replace('Его HTML, CSS и JavaScript также\n      находятся в корне LittleFS.', 'Обычная версия интерфейса хранится в LittleFS; текущая встроенная версия\n      работает независимо от этих файлов.')
manager=manager.replace('</body>', '''<script>
(()=>{
 const $=id=>document.getElementById(id);
 async function check(){
  try{const r=await fetch('/setup/status',{cache:'no-store'});if(!r.ok)throw Error(await r.text());const d=await r.json();
   $('bootstrapState').textContent=!d.fsMounted?'LittleFS не смонтирована: проверьте таблицу разделов и память. Существующие данные не стирались.':d.webReady?'Все необходимые веб-файлы загружены. Можно перейти на главную.':'Ещё нужны файлы: '+d.missing.join(', ');
   const u=new URL(location.href);u.port='';u.pathname='/';u.search='';u.hash='';$('bootstrapMain').href=u.href;$('bootstrapMain').hidden=!d.webReady;
  }catch(e){$('bootstrapState').textContent=String(e.message||e)+' Проверьте связь с контроллером и повторите проверку.';}
 }
 $('bootstrapCheck').onclick=check;
 $('button-upload').addEventListener('click',()=>setTimeout(check,1500));
 check();
})();
</script></body>''')
assert not re.search(r'<(?:script[^>]+src|link[^>]+href)=',manager)
assert not re.search(r'<(?:script[^>]+src|link[^>]+href)=',wifi)
for name,html in [('BOOTSTRAP_WIFI_HTML',wifi),('BOOTSTRAP_MANAGER_HTML',manager)]:
 assert ')BOOTPAGE"' not in html
text='#pragma once\n// Создано tools/build_bootstrap.py. Файлы LittleFS и ресурсы интернета не требуются.\n'
for name,html in [('BOOTSTRAP_WIFI_HTML',wifi),('BOOTSTRAP_MANAGER_HTML',manager)]:
 text+=f'static const char {name}[] PROGMEM = R"BOOTPAGE({html})BOOTPAGE";\n'
(r/'bootstrap_pages.h').write_text(text)
print('Generated embedded Wi-Fi and manager pages:',len(text.encode()),'bytes')
