// Look at me! PKJS: stores the settings on the phone and sends them to the watch.
var IRIS = {
  espresso: { c: 0x5C341A },
  hazel: { c: 0x5C7C3A, c2: 0x8A541C, mode: 1 },
  forest: { c: 0x327848 },
  steel: { c: 0x3A6AAC },
  ice: { c: 0x8494A0 },
  amber: { c: 0xC67E24 },
  violet: { c: 0x783CC8 },
  albino: { c: 0xCC2438 },
  ghost: { c: 0xD6E8F6, prad: 24 },
  cat: { c: 0xEAAE0A, slit: 1 },
  cyan: { c: 0x00D6FF },
  toxic: { c: 0x82FF14, slit: 1 },
  split: { c: 0x3A6AAC, c2: 0x92541C, mode: 2 },
  'void': { c: 0x1A1618, prad: 86 },
  demon: { c: 0xD61818, sclera: 0x221A1A, slit: 1 }
};
var DEFAULTS = { iris: 'steel', custom: '#3a6aac', pupil: 'auto', font: 0, dither: 0, wake: 5, stare: 1, veins: 1, quiet: 1, clock: 0, glint: 1, dark: 1 };
var STORE = 'lookatme.settings';

function log(m) { console.log('[LookAtMe] ' + m); }
function settings() {
  var s = {}, k;
  for (k in DEFAULTS) s[k] = DEFAULTS[k];
  try { var saved = JSON.parse(localStorage.getItem(STORE) || '{}'); for (k in saved) if (k in DEFAULTS) s[k] = saved[k]; } catch (e) {}
  return s;
}
function saveSettings(s) { localStorage.setItem(STORE, JSON.stringify(s)); }
function hexToInt(h) { var v = parseInt(String(h).replace('#', ''), 16); return isNaN(v) ? 0x3A6AAC : v; }
function settingsMessage(s) {
  var p = s.iris === 'custom' ? { c: hexToInt(s.custom) } : (IRIS[s.iris] || IRIS.steel);
  var slit = s.pupil === 'slit' ? 1 : s.pupil === 'round' ? 0 : (p.slit ? 1 : 0);
  return {
    S_IRIS: p.c, S_IRIS2: p.c2 || p.c, S_IMODE: p.mode || 0, S_PUPIL: slit, S_PRAD: p.prad || 0,
    S_SCLERA: p.sclera !== undefined ? p.sclera : -1,
    S_FONT: s.font, S_DITHER: s.dither, S_WAKE: s.wake, S_STARE: s.stare, S_VEINS: s.veins, S_QUIET: s.quiet, S_CLOCK: s.clock, S_GLINT: s.glint, S_DARK: s.dark
  };
}
// one AppMessage in flight; a newer message with the same label replaces a queued one
var queue = [], busy = false;
function pump() {
  if (busy || !queue.length) return;
  var item = queue.shift(), tries = item.tries || 0;
  busy = true;
  Pebble.sendAppMessage(item.msg, function () { busy = false; pump(); }, function () {
    busy = false;
    if (tries < 2) { item.tries = tries + 1; queue.unshift(item); setTimeout(pump, 1000); }
    else { log('send ' + item.label + ' failed'); pump(); }
  });
}
function send(msg, label) {
  for (var i = 0; i < queue.length; i++) if (queue[i].label === label) { queue[i] = { msg: msg, label: label }; return; }
  queue.push({ msg: msg, label: label });
  pump();
}

Pebble.addEventListener('configmessage', function (e) {
  var m = e.data || {};
  if (m.type === 'get') e.respond({ settings: settings() });
  else if (m.type === 'set') {
    var s = settings();
    for (var k in m.settings) if (k in DEFAULTS) s[k] = m.settings[k];
    saveSettings(s);
    send(settingsMessage(s), 'settings');
    e.respond({ ok: true });
  } else if (m.type === 'preview') { send({ DEMO: 1 }, 'preview'); e.respond({ ok: true }); }
  else e.respond({ error: 'unknown' });
});

Pebble.addEventListener('ready', function () { log('ready'); send(settingsMessage(settings()), 'settings'); });
Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (p.HELLO !== undefined) send(settingsMessage(settings()), 'settings');
});
