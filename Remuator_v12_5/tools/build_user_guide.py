#!/usr/bin/env python3
"""Собрать страницу инструкции и встроенную копию из единого Markdown без внешних зависимостей."""
from pathlib import Path
import html
import re

root=Path(__file__).resolve().parents[1]
source=(root/'USER_GUIDE_RU.md').read_text()
def inline(text):
    text=html.escape(text)
    return re.sub(r'`([^`]+)`',r'<code>\1</code>',text)
blocks=[];toc=[];paragraph=[];listed=False;section=0
def flush():
    if paragraph:
        blocks.append('<p>'+inline(' '.join(paragraph))+'</p>');paragraph.clear()
for line in source.splitlines()+['']:
    if not line.strip():
        flush()
        if listed:blocks.append('</ul>');listed=False
    elif line.startswith('#'):
        flush();level=len(line)-len(line.lstrip('#'));title=line[level:].strip()
        section+=1;anchor='section-'+str(section)
        blocks.append(f'<h{level} id="{anchor}">{inline(title)}</h{level}>')
        if level==2:toc.append(f'<li><a href="#{anchor}">{html.escape(title)}</a></li>')
    elif line.startswith('- '):
        flush()
        if not listed:blocks.append('<ul>');listed=True
        blocks.append('<li>'+inline(line[2:])+'</li>')
    else:paragraph.append(line)
body='\n'.join(blocks)
page='''<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Инструкция — Remuage-VRN</title><link rel="stylesheet" href="/style.css"><script src="/site-shell.js" defer></script><style>html{scroll-behavior:smooth}body{font-family:system-ui,sans-serif;line-height:1.65;margin:0;background:#101827;color:#e6edf5}main{max-width:1000px;margin:auto;padding:24px}a{color:#7ec7ff}code{overflow-wrap:anywhere}h2{margin-top:2.5em;border-top:1px solid #536078;padding-top:1em}nav.guide-toc{padding:16px;border:1px solid #536078;border-radius:12px}nav.guide-toc ul{columns:2;column-width:260px}header,footer{padding:16px}li{break-inside:avoid}a:focus{outline:2px solid #7ec7ff}</style></head><body><header id="siteHeader"><a href="/">Remuage-VRN · Главная</a> · <a href="/settings">Настройки</a></header><main>'''
page+='<nav class="guide-toc" aria-label="Оглавление инструкции"><strong>Оглавление</strong><ul>'+''.join(toc)+'</ul></nav>'+body
page+='</main><footer id="siteFooter">Remuator 12.5 · <a href="/">На главную</a></footer></body></html>\n'
(root/'data/help.html').write_text(page)
(root/'user_guide_page.h').write_text('#pragma once\n#include <pgmspace.h>\n// Создано tools/build_user_guide.py из USER_GUIDE_RU.md; не редактировать вручную.\nstatic const char USER_GUIDE_HTML[] PROGMEM = R"GUIDE('+page+')GUIDE";\n')
print(f'Собрана инструкция: {len(toc)} разделов, {len(page.encode())} байт')
