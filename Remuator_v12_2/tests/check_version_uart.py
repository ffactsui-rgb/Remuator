"""Guard against stale HTML version and silent save on missing UART acknowledgements."""
from pathlib import Path
p=Path(__file__).resolve().parent.parent
ino=next(p.glob('*.ino')).read_text()
api=(p/'web_api.h').read_text()
for html in (p/'data').glob('*.html'):
    s=html.read_text()
    assert 'id="siteHeader"' in s, html.name
    assert 'id="siteFooter"' in s, html.name
    assert '/site-shell.js' in s, html.name
    assert 'data-firmware-version' not in s, html.name
    assert 'v11.4.' not in s, html.name
assert 'server.on("/api/version"' in ino
assert 'handleApiUartDiagnostics' in api
assert 'probe.IOIN()' in api
assert 'applyTmcSettingsVerified(axis, c)' in ino
assert 'axis.writesAccepted == sent' in ino
assert 'server.send(503' in ino
assert ino.index('saveTmcSettings(c, pov ? "tp" : "tn");') > ino.index('if (!applyTmcSettingsVerified(axis, c))')
assert 'TmcUartSettings uartNaklon{TmcModel::Tmc2208, 0};' in ino
assert 'TmcUartSettings uartPovorot{TmcModel::Tmc2209, 1};' in ino
print('PASS: single firmware version API, version UI, UART verification & nonpersistent errors')
