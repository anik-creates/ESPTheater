// Setup portal page served at http://192.168.4.1/

extern "C" const char portal_html[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP USB Wi-Fi Setup</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--text:#1d2129;--muted:#6b7280;--line:#e5e7eb;--accent:#2563eb;--ok:#16a34a;--warn:#d97706;--bad:#dc2626}
@media (prefers-color-scheme:dark){:root{--bg:#111318;--card:#1b1e25;--text:#e8eaed;--muted:#9aa0aa;--line:#2b2f38;--accent:#5b8cff}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:460px;margin:0 auto;padding:20px 16px 40px}
h1{font-size:20px;margin:4px 0 2px}
.sub{color:var(--muted);font-size:13px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;margin-bottom:14px}
.status{display:flex;gap:10px;align-items:flex-start}
.dot{width:10px;height:10px;border-radius:50%;margin-top:6px;flex:none;background:var(--muted)}
.dot.ok{background:var(--ok)}.dot.busy{background:var(--warn)}.dot.bad{background:var(--bad)}
.status small{display:block;color:var(--muted)}
.head{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.head h2{font-size:15px;margin:0}
button{font:inherit;border:0;border-radius:8px;padding:9px 14px;cursor:pointer;background:var(--accent);color:#fff}
button.ghost{background:transparent;color:var(--accent);padding:6px 8px}
button:disabled{opacity:.5;cursor:default}
ul{list-style:none;margin:0;padding:0}
li{border-top:1px solid var(--line)}
li:first-child{border-top:0}
.net{display:flex;align-items:center;gap:10px;width:100%;background:none;color:var(--text);padding:11px 2px;border-radius:0;text-align:left}
.net .name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net .meta{color:var(--muted);font-size:12px}
.bars{display:inline-flex;gap:2px;align-items:flex-end;height:14px}
.bars i{width:3px;background:var(--line);border-radius:1px}
.bars i.on{background:var(--text)}
form{padding:4px 2px 12px;display:grid;gap:8px}
input{font:inherit;width:100%;padding:9px 10px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--text)}
.row{display:flex;gap:8px;align-items:center}
.row label{color:var(--muted);font-size:13px;display:flex;gap:6px;align-items:center}
.empty{color:var(--muted);padding:10px 2px}
.foot{display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap}
details.card summary{cursor:pointer;font-weight:600;list-style:none}
details.card summary::-webkit-details-marker{display:none}
details.card summary::after{content:"\25B8";float:right;color:var(--muted)}
details.card[open] summary::after{content:"\25BE"}
.grid{display:grid;grid-template-columns:auto 1fr;gap:8px 12px;align-items:center;margin-top:12px}
.grid label{color:var(--muted);font-size:13px}
select{font:inherit;padding:8px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--text)}
.hint{color:var(--muted);font-size:12px;grid-column:1/-1;margin-top:-4px}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px;align-items:center}
.msg{font-size:13px;color:var(--muted)}
.ver{color:var(--muted);font-size:12px;text-align:center;margin-top:6px}
.credit{color:var(--muted);font-size:12px;text-align:center;margin-top:2px}
.credit a{color:var(--accent);text-decoration:none}
body::before{content:"Anik Sarker";position:fixed;top:50%;left:50%;transform:translate(-50%,-50%) rotate(-30deg);font:700 64px/1 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;color:var(--text);opacity:.045;white-space:nowrap;pointer-events:none;z-index:0}
main{position:relative;z-index:1}
</style>
</head>
<body>
<main>
  <h1>ESP USB Wi-Fi Setup</h1>
  <div class="sub">ESPTheater &middot; developed by Anik Sarker</div>
  <div class="sub" id="apname">ESP32-S3 Wi-Fi to USB adapter</div>

  <div class="card status">
    <span class="dot" id="dot"></span>
    <div><b id="st">Checking…</b><small id="st2"></small></div>
  </div>

  <div class="card">
    <div class="head"><h2>Networks</h2><button class="ghost" id="rescan">Scan again</button></div>
    <ul id="list"><li class="empty">Scanning…</li></ul>
  </div>

  <div class="card foot">
    <button class="ghost" id="other">Hidden network…</button>
    <button class="ghost" id="forget" hidden>Forget saved network</button>
  </div>

  <details class="card" id="settings">
    <summary>Settings</summary>
    <div class="grid">
      <label for="ledPin">LED GPIO</label>
      <input id="ledPin" type="number" min="-1" max="48">
      <div class="hint" id="ledHint">-1 turns the LED off.</div>
      <label for="ledType">LED type</label>
      <select id="ledType">
        <option value="0">RGB (WS2812 / NeoPixel)</option>
        <option value="1">Single colour (on = high)</option>
        <option value="2">Single colour (on = low)</option>
      </select>
      <label for="ledLevel">Brightness</label>
      <input id="ledLevel" type="range" min="1" max="255">
      <label for="apPass">Hotspot password</label>
      <input id="apPass" type="password" placeholder="unchanged" autocomplete="new-password" maxlength="63">
      <div class="hint" id="apHint">8-63 characters. Leave empty to keep the current one.</div>
      <label></label>
      <label style="display:flex;gap:6px;align-items:center"><input type="checkbox" id="apOpen" style="width:auto"> Open hotspot (no password)</label>
    </div>
    <div class="actions">
      <button id="saveSettings">Save</button>
      <button class="ghost" id="restart">Restart device</button>
      <span class="msg" id="setMsg"></span>
    </div>
  </details>
  <div class="ver" id="ver"></div>
  <div class="credit">&copy; 2026 Anik Sarker &middot; <a href="https://github.com/anik-creates/ESPTheater">github.com/anik-creates/ESPTheater</a></div>
</main>

<template id="formTpl">
  <form>
    <input name="ssid" placeholder="Network name" autocomplete="off" required maxlength="32">
    <input name="pass" type="password" placeholder="Password" autocomplete="off" maxlength="64">
    <div class="row">
      <label><input type="checkbox" style="width:auto"> Show</label>
      <span style="flex:1"></span>
      <button type="submit">Connect</button>
    </div>
  </form>
</template>

<script>
const $ = id => document.getElementById(id);
let openItem = null;

function bars(rssi) {
  const n = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  return '<span class="bars">' + [6, 9, 12, 14].map((h, i) =>
    `<i style="height:${h}px" class="${i < n ? 'on' : ''}"></i>`).join('') + '</span>';
}

function makeForm(ssid, secure) {
  const f = $('formTpl').content.firstElementChild.cloneNode(true);
  const [name, pass] = f.querySelectorAll('input[name]');
  const show = f.querySelector('input[type=checkbox]');
  if (ssid !== null) { name.value = ssid; name.hidden = true; }
  if (!secure) pass.hidden = true;
  show.parentElement.hidden = !secure;
  show.onchange = () => pass.type = show.checked ? 'text' : 'password';
  f.onsubmit = async e => {
    e.preventDefault();
    const btn = f.querySelector('button');
    btn.disabled = true; btn.textContent = 'Connecting…';
    try {
      await fetch('/api/connect', { method: 'POST', body: new URLSearchParams({ ssid: name.value, pass: secure ? pass.value : '' }) });
    } catch (_) {}
    setTimeout(() => { btn.disabled = false; btn.textContent = 'Connect'; }, 4000);
    status();
  };
  setTimeout(() => (secure ? pass : name).focus(), 0);
  return f;
}

function toggle(li, ssid, secure) {
  if (openItem) openItem.querySelector('form')?.remove();
  if (openItem === li) { openItem = null; return; }
  openItem = li;
  li.appendChild(makeForm(ssid, secure));
}

async function scan() {
  $('rescan').disabled = true;
  $('list').innerHTML = '<li class="empty">Scanning…</li>';
  openItem = null;
  try {
    let r = await (await fetch('/api/scan')).json();
    if (r.busy) {
      $('list').innerHTML = '<li class="empty">Wi-Fi is busy connecting — retrying…</li>';
      setTimeout(scan, 4000);
      return;
    }
    const list = $('list');
    list.innerHTML = r.aps.length ? '' : '<li class="empty">No networks found</li>';
    for (const ap of r.aps) {
      const li = document.createElement('li');
      const b = document.createElement('button');
      b.className = 'net';
      b.innerHTML = `<span class="name"></span><span class="meta">${ap.secure ? '🔒' : 'open'}</span>${bars(ap.rssi)}`;
      b.querySelector('.name').textContent = ap.ssid;
      b.onclick = () => toggle(li, ap.ssid, ap.secure);
      li.appendChild(b);
      list.appendChild(li);
    }
  } catch (_) {
    $('list').innerHTML = '<li class="empty">Scan failed</li>';
  } finally {
    $('rescan').disabled = false;
  }
}

async function loadSettings() {
  try {
    const c = await (await fetch('/api/settings')).json();
    $('ledPin').value = c.led_pin;
    $('ledType').value = c.led_type;
    $('ledLevel').value = c.led_level;
    $('ledHint').textContent = `-1 turns the LED off. Default for this chip: GPIO${c.default_led_pin}.`;
    $('apHint').textContent = (c.ap_secured ? 'Currently secured. ' : 'Currently open. ') +
      '8-63 characters; leave empty to keep it.';
    $('apOpen').checked = !c.ap_secured;
  } catch (_) {}
}

async function saveSettings() {
  const body = new URLSearchParams({ led_pin: $('ledPin').value, led_type: $('ledType').value, led_level: $('ledLevel').value });
  if ($('apOpen').checked) body.set('ap_pass', '');
  else if ($('apPass').value) body.set('ap_pass', $('apPass').value);
  $('setMsg').textContent = 'Saving…';
  try {
    const r = await fetch('/api/settings', { method: 'POST', body });
    $('setMsg').textContent = r.ok ? 'Saved.' : await r.text();
    if (r.ok) { $('apPass').value = ''; loadSettings(); }
  } catch (_) { $('setMsg').textContent = 'Could not reach the device.'; }
}

function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + 'd ' : '') + (h ? h + 'h ' : '') + m + 'm';
}

async function status() {
  try {
    const s = await (await fetch('/api/status')).json();
    $('ver').textContent = `ESP USB Wi-Fi Adapter v${s.version} · ${s.chip} · up ${fmtUptime(s.uptime)}` +
      (s.connected && s.ip !== '0.0.0.0' ? ` · ${s.ip}` : '');
    $('apname').textContent = 'Setup network: ' + s.ap;
    $('forget').hidden = !s.saved;
    let cls = '', t, t2 = '';
    const pc = s.pc ? 'PC connected over USB.' : 'No PC detected — use the board\'s port labelled USB.';
    if (s.connected && s.internet === 'ok') {
      cls = 'ok'; t = 'Online via ' + s.ssid;
      t2 = 'Internet OK. ' + pc;
    } else if (s.connected && s.internet === 'down') {
      cls = 'bad'; t = 'Connected to ' + s.ssid + ', but no internet';
      t2 = 'The Wi-Fi network has no internet access (or needs a login page). ' + pc;
    } else if (s.connected) {
      cls = 'busy'; t = 'Connected to ' + s.ssid + ' — checking internet…';
      t2 = pc;
    } else if (s.connecting) {
      cls = 'busy'; t = 'Connecting to ' + s.ssid + '…';
    } else if (s.saved) {
      cls = s.fails ? 'bad' : 'busy';
      t = s.reason ? `${s.ssid}: ${s.reason}` : 'Saved: ' + s.ssid;
      t2 = s.reason === 'Wrong password' ? 'Pick the network again and re-enter the password.'
         : 'Retrying automatically. Or choose another network below.';
    } else {
      t = 'Not connected'; t2 = 'Choose a Wi-Fi network below.';
    }
    $('dot').className = 'dot ' + cls;
    $('st').textContent = t;
    $('st2').textContent = t2;
  } catch (_) {}
}

$('rescan').onclick = scan;
$('other').onclick = () => {
  const li = document.createElement('li');
  $('list').prepend(li);
  toggle(li, null, true);
};
$('saveSettings').onclick = saveSettings;
$('apPass').oninput = () => { if ($('apPass').value) $('apOpen').checked = false; };
$('restart').onclick = async () => {
  $('setMsg').textContent = 'Restarting… reconnect in a few seconds.';
  await fetch('/api/restart', { method: 'POST' }).catch(() => {});
};
$('settings').ontoggle = () => { if ($('settings').open) loadSettings(); };
$('forget').onclick = async () => {
  await fetch('/api/forget', { method: 'POST' }).catch(() => {});
  status();
};

status();
scan();
setInterval(status, 2000);
</script>
</body>
</html>
)html";
