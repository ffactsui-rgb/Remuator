// Все страницы используют общую навигацию, подвал и API версии прошивки.
(() => {
  const filePort = location.port === '8080';
  const base = new URL(location.href);
  base.port = location.protocol === 'https:' ? '' : '80';
  base.pathname = '/'; base.search = ''; base.hash = '';
  const href = (path) => filePort ? new URL(path, base).href : path;
  const manager = new URL(base);
  manager.port='8080';
  const nav = [
    ['/', 'Главная'], ['/programs', 'Программы'],
    ['/settings', 'Настройки'], ['/network', 'Сеть'], ['/help', 'Инструкция']
  ];
  const header=document.getElementById('siteHeader');
  if (header) {
    header.className='site-header';
    const title=document.createElement('a');
    title.className='site-brand'; title.href=href('/');
    title.textContent='Remuage-VRN';
    const links=document.createElement('nav'); links.className='site-menu';
    links.setAttribute('aria-label','Главное меню');
    const path=location.pathname;
    for (const [url,name] of nav) {
      const a=document.createElement('a'); a.href=href(url); a.textContent=name;
      if(!filePort && (path===url || (url==='/settings' && path.startsWith('/settings/')))) a.setAttribute('aria-current','page');
      links.appendChild(a);
    }
    header.replaceChildren(title,links);
  }
  const footer=document.getElementById('siteFooter');
  if(footer){
    footer.className='site-footer';
    const left=document.createElement('span');
    left.textContent='Remuage-VRN · '; 
    const docs=document.createElement('a');docs.href=href('/settings/about');docs.textContent='О программе';
    left.appendChild(docs);
    const guide=document.createElement('a');guide.href=href('/help');guide.textContent=' · Инструкция';left.appendChild(guide);
    const f=document.createElement('a');f.href=manager.href;f.textContent='Файловый менеджер';
    const right=document.createElement('span');right.textContent='Прошивка: ';
    const ver=document.createElement('strong');ver.id='siteFooterVersion';ver.textContent='…';right.appendChild(ver);
    footer.replaceChildren(left,document.createTextNode(' · '),f,right);
  }
  const versionUrl='/api/version';
  fetch(versionUrl,{cache:'no-store'}).then(r=>{if(!r.ok) throw new Error('version');return r.json()})
    .then(x=>{const el=document.getElementById('siteFooterVersion');if(el)el.textContent=x.version?'v'+x.version:'недоступна'})
    .catch(()=>{const el=document.getElementById('siteFooterVersion');if(el)el.textContent='недоступна';});
})();
