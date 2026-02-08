// --------- tiny helper ---------
async function jget(url){
  const r = await fetch(url, {cache:'no-store'});
  return r.json();
}
async function jpost(url, body){
  const r = await fetch(url, {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body
  });
  return r.json();
}

// --------- password eye ----------
let passVisible = false;
function togglePass(){
  const p = document.getElementById('pass');
  if(!p) return;
  passVisible = !passVisible;
  p.type = passVisible ? 'text' : 'password';
}

// --------- AP page: save wifi ----------
async function saveWifi(){
  const ssid = (document.getElementById('ssid')?.value || '').trim();
  const pass = (document.getElementById('pass')?.value || '');
  const msg  = document.getElementById('msg');
  if(msg) msg.textContent = 'Saving…';

  if(!ssid){
    if(msg) msg.textContent = 'SSID is required.';
    return;
  }

  const res = await jpost('/api/wifi',
    `ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`
  );

  if(res.ok){
    if(msg) msg.textContent = 'Saved. Rebooting…';
  }else{
    if(msg) msg.textContent = 'Error: ' + (res.err || 'unknown');
  }
}

// --------- index page ----------
async function refreshStatus(){
  const rEl = document.getElementById('r');
  const btn = document.getElementById('btn');
  const din = document.getElementById('din');
  const mq  = document.getElementById('mq');
  const ip  = document.getElementById('ip');

  // if not on a page that has status widgets, skip
  if(!rEl && !btn) return;

  const s = await jget('/api/status');
  if(!s.ok) return;

  if(rEl) rEl.textContent = s.relay ? 'ON' : 'OFF';
  if(btn){
    btn.textContent = s.relay ? 'Turn Off' : 'Turn On';
    btn.className = 'btn ' + (s.relay ? 'good' : 'dark');
  }
  if(din) din.textContent = s.input_pressed ? 'PRESSED' : 'OPEN';

  if(mq){
    mq.textContent = s.mqtt_connected ? 'Connected' : (s.mqtt_enabled ? 'Disconnected' : 'Off');
  }
  if(ip) ip.textContent = s.ip || '—';
}

async function toggleRelay(){
  const s = await jget('/api/status');
  if(!s.ok) return;
  const next = s.relay ? 'off' : 'on';
  await jpost('/api/relay', `state=${encodeURIComponent(next)}`);
  await refreshStatus();
}

function goSettings(){
  location.href = '/settings';
}

// --------- settings page: mqtt ----------
async function loadMqtt(){
  const enabled = document.getElementById('enabled');
  const host = document.getElementById('host');
  const port = document.getElementById('port');
  const user = document.getElementById('user');
  const pass = document.getElementById('pass');
  const cmd  = document.getElementById('cmdTopic');
  const st   = document.getElementById('stateTopic');
  const pill = document.getElementById('mqttPill');
  const hint = document.getElementById('passhint');

  if(!enabled) return; // not on settings page

  const c = await jget('/api/mqtt');
  if(!c.ok) return;

  enabled.value = c.enabled ? '1' : '0';
  host.value = c.host || '';
  port.value = c.port || '1883';
  user.value = c.user || '';
  cmd.value  = c.cmdTopic || '';
  st.value   = c.stateTopic || '';

  // never prefill password
  if(pass) pass.value = '';
  if(hint) hint.textContent = c.pass_set ? 'Password saved. Leave blank to keep.' : 'No password set.';

  if(pill){
    pill.className = 'pill dot ' + (c.enabled ? 'green' : '');
    pill.textContent = c.enabled ? 'Enabled' : 'Disabled';
  }
}

async function saveMqtt(){
  const msg  = document.getElementById('msg');
  const enabled = document.getElementById('enabled').value;
  const host = document.getElementById('host').value.trim();
  const port = document.getElementById('port').value.trim();
  const user = document.getElementById('user').value.trim();
  const pass = document.getElementById('pass').value; // blank => keep
  const cmd  = document.getElementById('cmdTopic').value.trim();
  const st   = document.getElementById('stateTopic').value.trim();

  if(msg) msg.textContent = 'Saving…';

  const res = await jpost('/api/mqtt',
    `enabled=${encodeURIComponent(enabled)}&host=${encodeURIComponent(host)}&port=${encodeURIComponent(port)}` +
    `&user=${encodeURIComponent(user)}&pass=${encodeURIComponent(pass)}` +
    `&cmdTopic=${encodeURIComponent(cmd)}&stateTopic=${encodeURIComponent(st)}`
  );

  if(res.ok){
    if(msg) msg.textContent = 'Saved. MQTT will reconnect automatically.';
  }else{
    if(msg) msg.textContent = 'Error: ' + (res.err || 'unknown');
  }

  // clear password field for safety
  const passEl = document.getElementById('pass');
  if(passEl) passEl.value = '';

  await loadMqtt();
}

// --------- boot ----------
setInterval(refreshStatus, 1500);
refreshStatus();
loadMqtt();
