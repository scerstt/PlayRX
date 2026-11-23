// AnalogRX app: shell (race bar, navigation) and views. Talks to the engine over the JSON API.
import {$, $$, esc, get, post, toast, ask, copy, fit, cssVar, every, video, multiSelect, events} from './arx.js';

// ---------------------------------------------------------------- shared state
const S = {state: null, race: null, channels: [], settings: {bands: ['R']}};
const BANDS = [
  {value: 'R', label: 'Raceband', hint: 'R1–R8'},
  {value: 'F', label: 'Fatshark / ImmersionRC', hint: 'F1–F8'},
  {value: 'A', label: 'Boscam A', hint: 'A1–A8'},
  {value: 'B', label: 'Boscam B', hint: 'B1–B8'},
  {value: 'E', label: 'Boscam E', hint: 'E1–E8'},
  {value: 'L', label: 'Lowband', hint: 'L1–L8 · 5362–5621 MHz'},
];
const listeners = new Set();  // views re-render from S on every poll
const leagueListeners = new Set();  // views showing the pilot list or events: reload on a league change
const onLeague = fn => (leagueListeners.add(fn), () => leagueListeners.delete(fn));
const onState = fn => (listeners.add(fn), () => listeners.delete(fn));
// someone is typing or choosing inside `box` (an input, list or text box has focus): don't redraw it under them.
// A clicked button keeps focus too, but must not stop a redraw (a removed pilot stayed listed, a struck lap unstruck)
const editing = box => !!box && box.contains(document.activeElement) && document.activeElement.matches('input, select, textarea');

async function pollState() {
  const [state, race] = await Promise.all([get('/api/state'), get('/api/race')]);
  S.state = state;
  S.race = race;
  S.flow = {phase: 'idle', pilots: []};
  renderBar();
  for (const fn of listeners) fn();
}

const STATE_LABEL = {
  locked: ['Locked', 'pill-good'], no_signal: ['No signal', 'pill-warn'], opening: ['Opening adapter', 'pill-accent'],
  no_adapter: ['No adapter', 'pill-bad'], error: ['Adapter error', 'pill-bad'], closed: ['Closed', ''],
  timing: ['Timing only', 'pill-good'], rotorhazard: ['RotorHazard', 'pill-good'],
  usb_live: ['USB video', 'pill-good'], usb_wait: ['USB: no picture', 'pill-warn'], usb_error: ['USB video error', 'pill-bad'],
  net_live: ['Feed node', 'pill-good'], net_wait: ['Feed node: no picture', 'pill-warn'], net_error: ['Feed node not reached', 'pill-bad'],
};
// Slots: where a slot's pictures come from (the adapter, or a capture device such as a USB video receiver)
let CAPTURES = [];
const loadCaptures = () => get('/api/captures').then(l => { CAPTURES = Array.isArray(l) ? l : []; }).catch(() => {});
function videoOpts(p) {
  const cur = p.video || '';
  let o = `<option value="" ${cur ? '' : 'selected'}>Adapter</option>`;
  const kind = id => (String(id).startsWith('net:') ? 'Network' : 'USB');  // a feed node on the network, or a USB device
  for (const c of CAPTURES) o += `<option value="${esc(c.id)}" ${c.id === cur ? 'selected' : ''}>${kind(c.id)} · ${esc(c.name)}</option>`;
  if (cur && !CAPTURES.some(c => c.id === cur)) o += `<option value="${esc(cur)}" selected>${kind(cur)} · ${esc(p.video_name || 'device')} (${kind(cur) === 'Network' ? 'not heard' : 'not plugged in'})</option>`;
  return o;
}
// an adapter's name for people: the one given on Adapters & scan, else "Adapter <USB port>"
const adapterName = id => { const a = S.state?.adapters?.find(x => x.id === id); return a?.label || (id ? 'Adapter ' + id : ''); };
// Slots: the adapter picker for a slot (Auto, or one adapter by its USB port)
function adapterOpts(p) {
  const ads = S.state?.adapters || [];
  const cur = p.pin || '';
  let o = `<option value="" ${cur ? '' : 'selected'}>Auto${!cur && p.adapter ? ' · ' + esc(adapterName(p.adapter)) : ''}</option>`;
  for (const a of ads) o += `<option value="${esc(a.id)}" ${a.id === cur ? 'selected' : ''}>${esc(a.label || 'Adapter ' + a.id)}${a.label ? ' · ' + esc(a.id) : ''}</option>`;
  if (cur && !ads.some(a => a.id === cur)) o += `<option value="${esc(cur)}" selected>${esc(adapterName(cur))} (not plugged in)</option>`;
  return o;
}
const statePill = s => {
  const [t, c] = STATE_LABEL[s.state] || [s.state || '?', ''];
  return `<span class="pill ${c}">${esc(t)}</span>`;
};
// two-antenna video: what each antenna adds (the radio's last second)
function abInfo(p) {
  const d = p.ab;
  if (!d || !d.on) return '<span class="faint">Off</span>';
  if (d.snr_a <= -99) return '<span class="faint">On</span>';
  if (d.snr_a < 0 && d.snr_b < 0) return '<span class="faint">On · no signal</span>';
  const share = Math.round(d.b_share * 100);
  const gain = d.snr_c - Math.max(d.snr_a, d.snr_b);
  const tip = `A ${d.snr_a.toFixed(1)} dB, B ${d.snr_b.toFixed(1)} dB, combined ${d.snr_c.toFixed(1)} dB SNR · A faded ${(d.a_fade * 100).toFixed(0)}% of the time${d.a_fade > 0 ? `, ${Math.round(d.rescued * 100)}% of that rescued by B` : ''}`;
  const cls = d.coherence < 0.1 && d.snr_b < 0 ? 'pill-warn' : 'pill-good';
  const txt = d.coherence < 0.1 && d.snr_b < 0 ? 'B hears nothing' : `B ${share}% · +${Math.max(0, gain).toFixed(1)} dB`;
  return `<span class="pill ${cls}" title="${esc(tip)}">${txt}</span>`;
}
const usedSlots = () => (S.state ? S.state.slots.filter(s => s.used) : []);
const raceOf = n => S.race?.pilots[n - 1];

// <option>s for a channel picker: the bands enabled in Settings, plus the current channel if it is in a band
// that has since been turned off. Values are channel names ("R7"); data-mhz carries the frequency.
function channelOptions(mhz, name = '') {
  const on = new Set(S.settings.bands);
  let cur = S.channels.find(c => c.name === name && Math.abs(c.mhz - mhz) < 3);
  if (!cur && mhz) {
    const near = [...S.channels].sort((a, b) => Math.abs(a.mhz - mhz) - Math.abs(b.mhz - mhz) || on.has(b.band) - on.has(a.band));
    cur = near.find(c => on.has(c.band) && Math.abs(c.mhz - mhz) < 3) || near[0];
  }
  const list = S.channels.filter(c => on.has(c.band) || c === cur);
  if (!cur) cur = list[0];
  return list.map(c => `<option value="${esc(c.name)}" data-mhz="${c.mhz}" ${c === cur ? 'selected' : ''}>${esc(c.name)} · ${c.mhz}${on.has(c.band) ? '' : ' (band off)'}</option>`).join('');
}
const chosen = select => ({channel: select.value, mhz: Number(select.selectedOptions[0]?.dataset.mhz)});

// ---------------------------------------------------------------- race bar + shell
function renderBar() {
  const st = S.state;
  $('#sim-pill').classList.toggle('hidden', !st.sim);
  $('#foot').innerHTML = `${usedSlots().length} feed(s) · ${st.adapters.length} adapter(s)<br>` +
    `OBS: <span class="mono">localhost:${st.port}</span>` + (st.feed_error ? `<br><span style="color:var(--bad)">${esc(st.feed_error)}</span>` : '');
}

function immersive(on, slot) {
  const was = document.body.classList.contains('immersive');
  on = on ?? !was;
  document.body.classList.toggle('immersive', on);
  if (on !== was) {
    // the app window: borderless full screen (a native toggle); a browser: the page's own full screen
    if (window.arxFullscreen) window.arxFullscreen();
    else if (on) document.documentElement.requestFullscreen?.().catch(() => {});
    else if (document.fullscreenElement) document.exitFullscreen?.().catch(() => {});
  }
  if (on && location.hash.split('/')[1] !== 'live') location.hash = slot ? `#/live/${slot}` : '#/live';
  setTimeout(() => window.dispatchEvent(new Event('resize')), 50);
}
// a browser left its own full screen (its Esc): leave the video-only view too
document.addEventListener('fullscreenchange', () => {
  if (!document.fullscreenElement && !window.arxFullscreen && document.body.classList.contains('immersive')) immersive(false);
});
$('#fullscreen').onclick = () => immersive(true);
// the Discord link: in the app window it opens in the system browser (a browser just opens a new tab)
$('#nav-discord').addEventListener('click', e => {
  if (!window.arxOpenExternal) return;
  e.preventDefault();
  window.arxOpenExternal(e.currentTarget.href);
});
addEventListener('keydown', e => {
  if (e.key === 'F11') { e.preventDefault(); immersive(); }
  else if (e.key === 'Escape' && document.body.classList.contains('immersive')) immersive(false);
});

// ---------------------------------------------------------------- editions
// One UI for both products: Play League (receivers + racing) and Play RX (receivers only: feeds to the app
// and OBS). In Play RX the racing parts (class league-only) are hidden and "pilots" read as "feeds".
const isRx = () => S.state?.edition === 'rx';
function applyEdition() {
  const rx = isRx(), product = S.state?.product || (rx ? 'Play RX' : 'Play League');
  document.body.classList.toggle('rx', rx);
  const ver = S.state?.version || '';
  document.title = ver ? `${product} ${ver}` : product;
  $('#brand-name').textContent = product;
  $('#brand-ver').textContent = ver ? 'v' + ver : '';
  $('#brand-mark').textContent = rx ? 'RX' : 'PL';
  $('#nav-pilots').textContent = rx ? 'Feeds' : 'Slots';
}

let obsShow = () => {};  // the Settings card replaces it while open

// ---------------------------------------------------------------- views
const views = {};
let current = null;

function route() {
  const [, name0, arg] = location.hash.split('/');
  const nodeMode = !!S.settings?.node_mode;
  document.body.classList.toggle('node-mode', nodeMode);
  const name = name0 || (nodeMode ? 'node' : S.state?.edition === 'rx' ? 'live' : 'dash');  // Play League opens on the Dashboard; a feed node on its page
  const rxOff = false;  // the racing views are not in Play RX
  const nodeOff = nodeMode && !['node', 'live', 'pilots', 'adapters', 'settings', 'log', 'timing'].includes(name);  // a feed node: no racing pages (Timing: view only)
  const view = nodeOff ? 'node' : views[name] && !rxOff ? name : 'live';
  $$('#nav .nav-item').forEach(a => a.classList.toggle('active', a.dataset.view === view));
  if (current?.unmount) current.unmount();
  // a fresh container for every page: listeners a page adds to it go away with it (never answer another page)
  const old = $('#view'), el = old.cloneNode(false);
  for (const k of Object.keys(el.dataset)) delete el.dataset[k];  // nothing a page marked on its container carries over to the next page
  old.replaceWith(el);
  current = views[view](el, arg) || {};
}
addEventListener('hashchange', route);

// ---- Live: every pilot's video
views.live = (el, arg) => {
  let focus = Number(arg) || 0, fsFocus = false;  // fsFocus: a tile's full-screen button chose the feed shown alone
  el.innerHTML = `
    <div class="view-head"><h1>Live</h1><span class="sub" id="live-sub"></span><div class="spacer"></div>
      <span class="faint">Click a feed to show it alone · F11 full screen</span>
      <button class="btn hidden" id="live-ll" title="Low latency: the feed you clicked (or the first) in its own window, each line drawn the moment it arrives instead of a whole picture at a time. F11 or double-click in that window for full screen: the lowest latency."><svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round" style="vertical-align:-3px;margin-right:4px"><path d="M13 2 4 14h7l-1 8 9-12h-7z"/></svg><span>Low latency</span></button>
      <span class="faint hidden" id="live-llstat" title="In the app: from a line arriving over USB to the moment it is handed to the display (average · worst in the last second). Add the display's own scan-out: about half a refresh (8 ms at 60 Hz, 2 ms at 240 Hz)."></span>
      <label class="row tight hidden" id="live-llsolo" title="While low latency is on, the other feeds stop decoding (their timing and pictures too), so the low latency feed has the PC to itself. They come back when low latency ends."><input type="checkbox" class="switch"><span class="faint">Pause other feeds</span></label>
      <select class="select" id="live-add" title="Watch a channel: adds a feed on it, no setup needed"></select></div>
    <div class="tiles" id="tiles"></div>`;
  // the quick way in: pick a channel, get a feed on it (named "Pilot n"; rename it on the Slots / Feeds page later)
  // low latency: one feed in its own Direct3D window (Windows), drawn line by line
  const llBtn = $('#live-ll', el);
  llBtn.onclick = async () => {
    const cur = S.state?.lowlat?.slot || 0, first = usedSlots()[0]?.slot || 0;
    const want = cur ? 0 : (focus || first);
    if (!cur && !want) return toast('Watch a channel first');
    try {
      const r = await post('/api/lowlat', {slot: want});
      if (want && !r.slot) toast('Could not open the low latency window');
      else if (want) toast('Low latency window open · F11 there for full screen');
      await pollState();
    } catch { toast('Could not change low latency'); }
  };
  function llUpdate() {
    const ll = S.state?.lowlat || {}, on = ll.slot || 0;
    llBtn.classList.toggle('hidden', !ll.supported);
    llBtn.classList.toggle('btn-primary', !!on);
    const s = on && usedSlots().find(x => x.slot === on);
    const txt = on ? `Low latency: ${s?.name || 'Feed ' + on} · stop` : 'Low latency';
    const span = llBtn.querySelector('span');
    if (span.textContent !== txt) span.textContent = txt;
    if (ll.error && !on) llBtn.title = 'Low latency: ' + ll.error;
    const stat = $('#live-llstat', el), solo = $('#live-llsolo', el), box = solo.querySelector('input');
    stat.classList.toggle('hidden', !on);
    const st = on ? (ll.age_ms ? `${ll.age_ms.toFixed(1)} ms in app · worst ${ll.age_max_ms.toFixed(1)}` : 'waiting for a picture') + (ll.fullscreen ? ' · full screen' : ' · windowed (F11 there)') : '';
    if (stat.textContent !== st) stat.textContent = st;
    solo.classList.toggle('hidden', !ll.supported);
    if (document.activeElement !== box) box.checked = !!ll.solo;
  }
  $('#live-llsolo input', el).onchange = async e => {
    try { await post('/api/lowlat', {solo: e.target.checked}); await pollState(); }
    catch { toast('Could not change it'); }
  };
  const addSel = $('#live-add', el);
  let addSig = '';
  function fillAdd() {
    const full = usedSlots().length >= 8;
    const sig = S.settings.bands.join('') + '/' + S.channels.length + '/' + full;
    if (sig === addSig || document.activeElement === addSel) return;
    addSig = sig;
    addSel.innerHTML = `<option value="">${full ? 'All 8 slots in use' : '+ Watch a channel…'}</option>` + (full ? '' : channelOptions(0).replace(/ selected/g, ''));
    addSel.value = '';
    addSel.disabled = full;
  }
  addSel.onchange = async () => {
    if (!addSel.value) return;
    const ch = chosen(addSel);
    addSel.value = '';
    addSel.blur();
    try { await post('/api/pilots', {name: '', pilot_id: 0, ...ch}); toast(`Watching ${ch.channel}`); await pollState(); }
    catch (err) { toast(err.status === 409 ? 'All 8 slots are in use' : 'Could not add the feed'); }
  };
  const tilesEl = $('#tiles', el), tiles = new Map();
  let stream = null, streamKey = '';  // one WebSocket for every tile, reopened when the pilots change
  const tileHtml = n => `<canvas></canvas><div class="tile-cap"><span class="tile-n">${n}</span><span class="tile-name"></span><span class="tile-ch"></span></div>
    <div class="tile-ctl"><select class="select tile-sel" title="Channel"></select>
      <button class="btn btn-icon tile-fs" title="Full screen (Esc to leave)"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/></svg></button></div>
    <div class="tile-laps"></div><div class="tile-msg"></div><div class="tile-foot"></div>`;

  function layout() {
    if (fsFocus && !document.body.classList.contains('immersive')) { fsFocus = false; focus = 0; return update(); }  // left full screen: back to every feed
    const shown = [...tiles.values()].filter(t => !t.hidden);
    const n = shown.length;
    if (!n) return;
    // full screen on one feed: it fills the screen edge to edge (the picture keeps its shape)
    const solo = document.body.classList.contains('immersive') && n === 1;
    tilesEl.classList.toggle('solo', solo);
    if (solo) { tilesEl.style.gridTemplateColumns = ''; tilesEl.style.width = ''; return; }
    const immersiveOn = document.body.classList.contains('immersive');
    // full screen: an 8 px edge all round, the same as the gap between feeds
    const W = immersiveOn ? innerWidth - 16 : tilesEl.clientWidth, H = (immersiveOn ? innerHeight - 16 : innerHeight - tilesEl.getBoundingClientRect().top - 24);
    let best = 1, bw = 0;
    for (let c = 1; c <= n; c++) {
      const r = Math.ceil(n / c), w = Math.min((W - (c - 1) * 8) / c, ((H - (r - 1) * 8) / r) * 4 / 3);
      if (w > bw) { bw = w; best = c; }
    }
    // full screen, several feeds: a wall centred both ways, a short last row centred under the others
    tilesEl.classList.toggle('wall', immersiveOn);
    const tw = Math.floor(bw);
    if (immersiveOn) {
      tilesEl.style.gridTemplateColumns = '';
      tilesEl.style.width = `${best * tw + (best - 1) * 8}px`;
      tilesEl.style.setProperty('--tw', `${tw}px`);
      return;
    }
    tilesEl.style.width = '';
    tilesEl.style.gridTemplateColumns = `repeat(${best}, ${tw}px)`;
    tilesEl.style.justifyContent = 'center';
  }
  addEventListener('resize', layout);

  function update() {
    const slots = usedSlots();
    fillAdd();
    llUpdate();
    $('#live-sub', el).textContent = slots.length ? `${slots.length} ${isRx() ? 'feed' : 'pilot'}${slots.length > 1 ? 's' : ''}` : '';
    const want = new Set(slots.map(s => s.slot));
    for (const [n, t] of tiles) if (!want.has(n)) { t.remove(); tiles.delete(n); }
    const key = [...want].join(',');
    if (key !== streamKey) {
      stream?.close();
      streamKey = key;
      stream = key ? video([...want], n => tiles.get(n)?.querySelector('canvas')) : null;
    }
    for (const s of slots) {
      let t = tiles.get(s.slot);
      if (!t) {
        t = document.createElement('div');
        t.className = 'tile';
        t.innerHTML = tileHtml(s.slot);
        t.onclick = () => { focus = focus === s.slot ? 0 : s.slot; update(); };
        const ctl = t.querySelector('.tile-ctl'), sel = t.querySelector('.tile-sel');
        ctl.onclick = e => e.stopPropagation();  // the controls: not "show this feed alone"
        sel.onchange = async () => {
          const ch = chosen(sel);
          sel.blur();
          try { await post(`/api/pilots/${s.slot}`, ch); toast(`Feed ${s.slot} → ${ch.channel}`); await pollState(); }
          catch { toast('Could not change the channel'); }
        };
        t.querySelector('.tile-fs').onclick = () => {
          if (document.body.classList.contains('immersive') && focus === s.slot) { immersive(false); layout(); return; }
          if (focus !== s.slot) fsFocus = true;  // the grid comes back when full screen ends
          focus = s.slot;
          immersive(true);
          update();
        };
        tiles.set(s.slot, t);
        tilesEl.append(t);
      }
      const sel = t.querySelector('.tile-sel'), selSig = s.channel + '/' + Math.round(s.mhz) + '/' + S.settings.bands.join('') + '/' + S.channels.length;
      if (sel.dataset.sig !== selSig && document.activeElement !== sel) { sel.dataset.sig = selSig; sel.innerHTML = channelOptions(s.mhz, s.channel); }
      t.hidden = focus && focus !== s.slot && want.has(focus);
      t.querySelector('.tile-name').textContent = s.name;
      t.querySelector('.tile-ch').textContent = `${s.channel} ${Math.round(s.mhz)}`;
      const msg = {paused: 'PAUSED · LOW LATENCY', no_signal: 'NO SIGNAL', opening: 'OPENING ADAPTER', no_adapter: 'NO ADAPTER', error: 'ADAPTER ERROR', timing: 'TIMING ONLY', rotorhazard: 'ROTORHAZARD TIMING', usb_wait: 'NO USB PICTURE', usb_error: 'USB VIDEO ERROR'}[s.state] || '';
      // not locked, but pictures still arriving (a weak or breaking-up signal): don't cover them with "NO SIGNAL"
      const pics = performance.now() - Number(t.querySelector('canvas').dataset.at || -1e9) < 1500;
      const weak = s.state === 'no_signal' && pics;
      t.querySelector('.tile-msg').textContent = weak ? '' : msg;
      const r = raceOf(s.slot);
      const llPill = S.state?.lowlat?.slot === s.slot ? ' <span class="pill pill-accent plain" title="Also in the low latency window">LOW LATENCY</span>' : '';
      const foot = s.state === 'locked' ? `<span class="pill pill-good">${s.fps} fps</span>${llPill}` : weak ? '<span class="pill pill-warn" title="The receiver is not locked onto this picture: move closer, or check the channel">Weak signal</span>' : '';
      const fe = t.querySelector('.tile-foot');
      if (fe.innerHTML !== foot) fe.innerHTML = foot;
    }
    if (!slots.length && !$('.empty', el)) tilesEl.insertAdjacentHTML('beforebegin',
      `<div class="card empty"><b>Nothing to watch yet</b>Pick a channel from <b>+ Watch a channel</b> (top right) to see it straight away, or set up named ${isRx() ? 'feeds on the <a href="#/pilots">Feeds</a>' : 'pilots on the <a href="#/pilots">Slots</a>'} page.</div>`);
    if (slots.length) $('.empty', el)?.remove();
    layout();
  }
  const off = onState(update);
  if (S.state) update();
  return {unmount() { off(); removeEventListener('resize', layout); stream?.close(); }};
};

// ---- virtual cameras (Windows): bit n of settings.vcam_mask lists slot n as "<product> Pilot n" in other programs
const vcamOk = () => !!S.state?.vcam?.supported;
const camName = n => `${S.state?.product || 'Play League'} Pilot ${n}`;
const camOn = n => !!((S.settings.vcam_mask || 0) >> n & 1);
async function setCam(n, on) {
  const m = on ? (S.settings.vcam_mask || 0) | 1 << n : (S.settings.vcam_mask || 0) & ~(1 << n);
  S.settings = await post('/api/settings', {vcam_mask: m});
  await pollState();
  const err = S.state?.vcam?.error;
  toast(err ? `Camera: ${err}` : on ? `“${camName(n)}” is now a camera: restart Discord / Trackside / OBS to see it` : `“${camName(n)}” removed from the camera list`);
}

// ---- Pilots: add, rename, change channel, remove; OBS URLs
views.pilots = el => {
  // Play RX calls them feeds, and sets each feed's gain here (it has no Timing page)
  const rx = isRx(), Noun = rx ? 'Feed' : 'Pilot', noun = Noun.toLowerCase();
  el.innerHTML = `
    <div class="view-head"><h1>${rx ? 'Feeds' : 'Slots'}</h1><span class="sub">${rx ? 'Each feed gets a slot, an adapter and an OBS feed of its own.'
      : 'Who flies on each adapter right now. Pick a pilot from your <a href="#/roster">pilot list</a> (or type a guest’s name); loading a heat from an <a href="#/events">event</a> fills them too. Each slot keeps its adapter, gate tuning and OBS feed.'}</span></div>
    <div class="card"><div class="card-b"><form class="row wide" id="add">
      ${rx ? '' : '<label class="field" style="flex:1;min-width:200px"><span>Pilot</span><select class="select" name="pid"></select></label>'}
      <label class="field ${rx ? '' : 'hidden'}" data-guest style="flex:1;min-width:180px"><span>${rx ? 'Feed name' : 'Guest name'}</span><input class="input" name="name" placeholder="${rx ? 'e.g. Alex, or Gate cam' : 'e.g. Sam'}" maxlength="40"></label>
      <label class="field"><span>Channel</span><select class="select" name="ch">${channelOptions(0)}</select></label>
      <div class="field"><span>&nbsp;</span><button class="btn btn-primary">Add ${noun}</button></div>
    </form></div></div>
    <div class="card"><div class="card-b flush"><table class="table">
      <thead><tr><th>Slot</th><th>${Noun}</th><th>Channel</th><th>Status</th><th title="Where the pictures come from: the adapter, or a USB video receiver / capture card plugged into this computer">Video</th><th>Adapter</th><th title="Settings → Diversity: both antennas combined">Diversity</th>${rx ? '<th>Gain</th>' : ''}${vcamOk() ? '<th title="List this slot as a camera in other programs (Discord, FPV Trackside, OBS, Zoom ...)">Camera</th>' : ''}<th>OBS Browser Source</th><th></th></tr></thead>
      <tbody id="rows"></tbody></table></div></div>
    <p class="hint">In OBS: Sources → + → <b>Browser</b>, paste a ${noun}'s URL, width 1280, height 960. A slot keeps its URL when
    the ${noun} in it changes, so set OBS up once.${rx ? ' Gain: start at 30 dB; <b>Auto</b> sets it from the strongest signal of the last 20 s (fly close to the antenna first).' : ' The race standings overlay is on the <a href="#/race">Race</a> page.'}</p>`;
  $('#add', el).onsubmit = async e => {
    e.preventDefault();
    const f = e.target;
    try {
      const pid = rx ? 0 : Number(f.pid.value) || 0;
      if (!rx && f.pid.value === '') return toast('Choose a pilot (or Guest)');
      await post('/api/pilots', {name: f.name.value.trim(), pilot_id: pid, ...chosen(f.ch)});
      f.name.value = '';
      if (!rx) f.pid.value = '';
      f.name.focus();
      pollState();
    } catch (err) { toast(err.status === 409 ? `All 8 ${noun} slots are in use` : `Could not add the ${noun}`); }
  };
  const rows = $('#rows', el);
  let sig = '', list = [];
  // the pilot list: a picker for each slot (choose who flies on that adapter) and for Add
  const pilotOpts = (id, inUse) => `${list.map(p => `<option value="${p.id}" ${p.id === id ? 'selected' : ''} ${inUse.has(p.id) && p.id !== id ? 'disabled' : ''}>${esc(p.name)}${p.callsign ? ` · ${esc(p.callsign)}` : ''}${inUse.has(p.id) && p.id !== id ? ' (in a slot)' : ''}</option>`).join('')}`;
  function fillAdd() {
    if (rx) return;
    const sel = $('#add [name=pid]', el), v = sel.value;
    const inUse = new Set(usedSlots().map(s => s.pilot_id).filter(Boolean));
    sel.innerHTML = `<option value="">Choose…</option>${pilotOpts(0, inUse)}<option value="0">Guest (type a name)</option>`;
    sel.value = v;
  }
  async function loadList() {
    if (rx) return;
    sig = '';
    fillAdd();
    update();
  }
  if (!rx) {
    $('#add [name=pid]', el).onchange = e => {
      const f = e.target.form;
      $('[data-guest]', f).classList.toggle('hidden', e.target.value !== '0');
      const p = list.find(x => String(x.id) === e.target.value);
      if (p?.channel) for (const o of f.ch.options) if (o.value === p.channel) f.ch.value = p.channel;  // their usual channel
    };
  }
  function update() {
    const slots = usedSlots();
    // rebuild only when the set of pilots changes (and not while someone is typing in the table)
    const s = slots.map(p => p.slot + ':' + p.name + ':' + p.pilot_id + ':' + p.mhz + ':' + p.channel + ':' + p.gain_db + ':' + p.pin + ':' + (p.pin ? '' : p.adapter) + ':' + p.video).join('|') + '/' + CAPTURES.map(c => c.id).join(',') + '/' + S.settings.bands.join('') + '/' + list.length +
      '/' + (S.state?.adapters || []).map(a => a.id + '=' + a.label).join(',');
    const inUse = new Set(slots.map(p => p.pilot_id).filter(Boolean));
    if (s !== sig && !editing(rows)) {
      sig = s;
      if (!editing($('#add', el))) fillAdd();
      rows.innerHTML = slots.length ? slots.map(p => `<tr data-slot="${p.slot}">
        <td class="slot-n">${p.slot}</td>
        <td>${rx ? `<input class="input inline" data-k="name" value="${esc(p.name)}" maxlength="40">`
          : `<div class="row tight" style="flex-wrap:nowrap"><select class="select" data-k="pid" title="Who flies on this adapter">${pilotOpts(p.pilot_id, inUse)}
              <option value="0" ${p.pilot_id ? '' : 'selected'}>Guest</option></select>
            ${p.pilot_id ? '' : `<input class="input inline" data-k="name" value="${esc(p.name)}" maxlength="40" placeholder="Guest name" style="min-width:110px">`}</div>`}</td>
        <td><select class="select" data-k="mhz">${channelOptions(p.mhz, p.channel)}</select></td>
        <td data-k="state"></td><td><select class="select" data-k="video" title="Video from the adapter, or from a USB video receiver / capture card (set its channel on the receiver itself). With USB video the adapter only times laps (none needed with RotorHazard).">${videoOpts(p)}</select></td><td><select class="select" data-k="adapter" title="Which adapter this slot uses. Auto: whichever is free. Name adapters on Adapters &amp; scan.">${adapterOpts(p)}</select></td>
        <td><span data-k="abinfo"></span></td>
        ${rx ? `<td><div class="row tight"><input class="input num" data-k="gain" type="number" min="0" max="45" step="1" value="${p.gain_db < 0 ? 30 : p.gain_db}" style="width:64px"><span class="faint">dB</span><button class="btn btn-sm btn-ghost" data-act="autogain" title="Set from the strongest signal of the last 20 s">Auto</button></div></td>` : ''}
        ${vcamOk() ? `<td><label class="row tight" style="flex-wrap:nowrap" title="Listed as “${esc(camName(p.slot))}” in Discord, FPV Trackside, OBS and other programs (restart them to see a new one)"><input type="checkbox" class="switch" data-k="cam"><span class="faint" data-k="camname" style="white-space:nowrap"></span></label></td>` : ''}
        <td><div class="row tight"><span class="mono" style="word-break:break-all">${esc(p.url)}</span><button class="btn btn-sm btn-ghost" data-act="copy">Copy</button></div></td>
        <td><div class="row tight">${rx ? '' : `<a class="btn btn-sm" href="#/timing/${p.slot}">Tune gate</a>`}<button class="btn btn-sm btn-ghost" data-act="remove">Remove</button></div></td>
      </tr>`).join('') : `<tr><td colspan="10"><div class="empty"><b>No ${noun}s</b>Add the first one above.</div></td></tr>`;
    }
    for (const p of slots) {
      const tr = rows.querySelector(`tr[data-slot="${p.slot}"]`);
      if (!tr) continue;
      const gl = p.glitches, gh = statePill(p) + (p.video_error || p.video_format ? `<div class="faint" style="font-size:var(--fs-xs);margin-top:2px;max-width:260px;white-space:normal">${esc(p.video_error || p.video_format)}</div>` : '') + (p.share_note ? `<div class="faint" style="font-size:var(--fs-xs);margin-top:2px;max-width:260px;white-space:normal">${esc(p.share_note)}</div>` : '') + (gl && (gl.restarts || gl.rephases || gl.dropped || gl.lost) ? `<div class="faint" style="font-size:var(--fs-xs);margin-top:2px" title="Since the adapter opened. USB restarts: the stream paused and restarted. Re-syncs: the picture jumped and the decoder caught up. Dropped: a block of samples the PC collected too late, left out while the stream carried on (one now and then is normal). Lost: the picture was lost and found again. Each shows as a brief tear or roll.">USB restarts ${gl.restarts} · re-syncs ${gl.rephases}${gl.dropped ? ` · dropped ${gl.dropped}` : ''}${gl.lost ? ` · lost ${gl.lost}` : ''}</div>` : '');
      const sc = tr.querySelector('[data-k=state]');
      if (sc.innerHTML !== gh) sc.innerHTML = gh;
      const ai = tr.querySelector('[data-k=abinfo]'), h = abInfo(p);
      if (ai.innerHTML !== h) ai.innerHTML = h;
      const cam = tr.querySelector('[data-k=cam]');
      if (cam && document.activeElement !== cam) {
        cam.checked = camOn(p.slot);
        const t = cam.checked ? camName(p.slot) : 'Off', cn = tr.querySelector('[data-k=camname]');
        if (cn.textContent !== t) cn.textContent = t;
      }
    }
  }
  rows.addEventListener('change', async e => {
    const tr = e.target.closest('tr'), k = e.target.dataset.k;
    if (!tr || !k) return;
    const n = tr.dataset.slot;
    if (k === 'gain') await post(`/api/gate/${n}`, {gain_db: Math.max(0, Math.min(45, Number(e.target.value) || 0))});
    else if (k === 'cam') {
      await setCam(Number(n), e.target.checked);
      e.target.blur();
    }
    else if (k === 'video') {
      await post(`/api/pilots/${n}`, {video: e.target.value});
      const c = CAPTURES.find(x => x.id === e.target.value);
      toast(!c ? `Slot ${n}: video from the adapter` : c.net ? `Slot ${n}: video from feed node ${c.name} (it tunes to ${S.state.slots[n - 1].channel} itself)` : `Slot ${n}: video from ${c.name}. Set the receiver to ${S.state.slots[n - 1].channel} yourself.`);
    }
    else if (k === 'adapter') {
      await post(`/api/pilots/${n}`, {adapter: e.target.value});
      toast(e.target.value ? `Slot ${n} uses ${adapterName(e.target.value)}` : `Slot ${n}: any free adapter`);
    }
    else if (k === 'pid') {
      const id = Number(e.target.value);
      const p = list.find(x => x.id === id), cur = S.state.slots[n - 1].channel;
      // their usual channel too, unless a heat is loaded (the heat planned the channels)
      const usual = p?.channel && p.channel !== cur && !S.state?.heat ? S.channels.find(c => c.name === p.channel) : null;
      await post(`/api/pilots/${n}`, usual ? {pilot_id: id, mhz: usual.mhz, channel: usual.name} : {pilot_id: id});
      if (p) toast(`${p.name} on slot ${n}${usual ? ` on their usual channel ${usual.name}` : p.channel && p.channel !== cur ? ` (their usual channel is ${p.channel})` : ''}`);
    }
    else await post(`/api/pilots/${n}`, k === 'mhz' ? chosen(e.target) : {name: e.target.value.trim()});
    sig = '';
    pollState();
  });
  rows.addEventListener('keydown', e => { if (e.key === 'Enter' && e.target.matches('input')) e.target.blur(); });
  // the device list again when a picker opens (a receiver plugged in since)
  rows.addEventListener('focusin', e => { if (e.target.dataset.k === 'video') loadCaptures().then(() => { const v = e.target.value; const p = S.state.slots[e.target.closest('tr').dataset.slot - 1]; e.target.innerHTML = videoOpts(p); e.target.value = v; }); });
  rows.addEventListener('click', async e => {
    const b = e.target.closest('[data-act]');
    if (!b) return;
    const n = b.closest('tr').dataset.slot, p = S.state.slots[n - 1];
    if (b.dataset.act === 'copy') copy(p.url);
    if (b.dataset.act === 'autogain') {
      const g = await post(`/api/gate/${n}/autogain`);
      toast(`${p.name}: gain ${g.gain_db} dB`);
      sig = '';
      setTimeout(pollState, 300);
    }
    if (b.dataset.act === 'remove' && confirm(`Remove ${p.name} from slot ${n}?`)) {
      try { await post(`/api/pilots/${n}/remove`); } catch { toast('Wait until the adapter has finished opening'); }
      pollState();
    }
  });
  const off = onState(update), offL = onLeague(loadList);
  if (S.state) update();
  loadList();
  loadCaptures().then(() => { sig = ''; if (S.state) update(); });
  return {unmount() { off(); offL(); }};
};

function setHtml(box, html) { if (box.dataset.html !== html) { box.dataset.html = html; box.innerHTML = html; } }

// ---- Adapters & scan
views.adapters = el => {
  el.innerHTML = `
    <div class="view-head"><h1>Adapters &amp; scan</h1></div>
    <div class="grid-2"><div>
      <div class="card"><div class="card-h"><h3>Adapters</h3><div class="spacer"></div><button class="btn btn-sm" id="rescan">Rescan USB</button></div>
        <div class="card-b flush"><table class="table"><thead><tr><th>USB port</th><th>Name</th><th>Adapter</th><th>Link</th><th>Used by</th></tr></thead><tbody id="ad"></tbody></table></div></div>
      <div class="card"><div class="card-h"><h3>Find transmitters</h3><span class="faint">sweeps 5.3–6.0 GHz on a spare adapter</span><div class="spacer"></div>
        <button class="btn btn-sm btn-primary" id="scan">Scan the bands</button></div>
        <div class="card-b flush"><table class="table"><thead><tr><th>Channel</th><th class="r">MHz</th><th>Video</th><th class="r">SNR</th><th></th></tr></thead><tbody id="found"></tbody></table></div></div>
    </div><div class="card"><div class="card-h"><h3>Setting up an adapter</h3></div><div class="card-b hint" style="line-height:1.7">
      <b style="color:var(--text)">RTL8812BU only</b> (e.g. TP-Link Archer T4U v3). Plug it into a <b style="color:var(--text)">USB 3</b> port, at most two per USB controller.<br><br>
      Once per adapter, switch it to the <b style="color:var(--text)">WinUSB</b> driver with Zadig: Options → List All Devices, pick the adapter, WinUSB, Replace Driver. Then Rescan USB.<br><br>
      Antenna 1 (path A) feeds the picture; with Diversity on (Settings → Receivers) antenna 2 (path B) joins it.<br><br>
      <b style="color:var(--text)">Names</b> (e.g. "Desk left") make the adapter picker on the Feeds page easy to read. Windows tells adapters apart only by
      the USB port they are plugged into, so a name belongs to the port: keep each adapter in the same port.</div></div></div>`;
  $('#rescan', el).onclick = () => post('/api/adapters/rescan').then(pollState);
  // naming an adapter (a USB port: the name stays with the port, whichever adapter is plugged into it)
  $('#ad', el).addEventListener('change', async e => {
    const id = e.target.dataset.name;
    if (id === undefined) return;
    await post('/api/adapters/name', {id, name: e.target.value.trim()});
    toast(e.target.value.trim() ? `Port ${id}: "${e.target.value.trim()}"` : `Port ${id}: name cleared`);
    pollState();
  });
  $('#ad', el).addEventListener('keydown', e => { if (e.key === 'Enter' && e.target.matches('input')) e.target.blur(); });
  $('#scan', el).onclick = () => post('/api/scan').then(pollState);
  $('#found', el).addEventListener('click', async e => {
    const b = e.target.closest('[data-mhz]');
    if (!b) return;
    const name = prompt((isRx() ? 'Feed' : 'Pilot') + ' name for ' + b.dataset.name + '?', '');
    if (name === null) return;
    try { await post('/api/pilots', {name, mhz: Number(b.dataset.mhz), channel: b.dataset.ch}); toast(isRx() ? 'Feed added' : 'Pilot added'); pollState(); } catch { toast('All 8 slots are in use'); }
  });
  function update() {
    const st = S.state;
    const adSig = JSON.stringify(st.adapters) + st.usb_busy;
    if ($('#ad', el).dataset.sig !== adSig && !editing($('#ad', el))) {
    $('#ad', el).dataset.sig = adSig;
    $('#ad', el).innerHTML = st.adapters.length ? st.adapters.map(a => `<tr><td class="mono">${esc(a.id)}</td>
      <td><input class="input inline" data-name="${esc(a.id)}" value="${esc(a.label || '')}" placeholder="e.g. Desk left" maxlength="40" style="min-width:120px"></td><td>${esc(a.name)}</td>
      <td>${a.usb3 ? '<span class="pill pill-good">USB 3</span>' : '<span class="pill pill-bad">USB 2 · too slow</span>'}</td>
      <td>${a.user === 'free' ? '<span class="faint">free</span>' : esc(a.user)}</td></tr>`).join('')
      : `<tr><td colspan="5"><div class="empty"><b>${st.usb_busy ? 'Looking for adapters…' : 'No adapter found'}</b>${st.usb_busy ? '' : 'Check the WinUSB driver and the USB 3 port, then Rescan.'}</div></td></tr>`;
    }
    $('#rescan', el).disabled = st.usb_busy;
    const sc = st.scan;
    const btn = $('#scan', el);
    btn.disabled = sc.running || !sc.spare;
    btn.textContent = sc.running ? `Scanning ${sc.step + 1} / ${sc.steps || '…'}` : 'Scan the bands';
    btn.title = sc.spare ? '' : 'Every adapter is in use by a pilot';
    $('#found', el).innerHTML = sc.found.length ? sc.found.map(f => `<tr><td><b>${esc(f.name || '?')}</b></td><td class="r">${f.mhz.toFixed(1)}</td>
      <td>${esc(f.std)}</td><td class="r">${f.snr.toFixed(0)} dB</td>
      <td class="r"><button class="btn btn-sm" data-mhz="${f.mhz}" data-ch="${esc(f.name)}" data-name="${esc(f.name || f.mhz.toFixed(0))}">${isRx() ? 'Add feed' : 'Add pilot'}</button></td></tr>`).join('')
      : `<tr><td colspan="5"><div class="empty">${sc.running ? 'Scanning…' : sc.spare ? 'No scan yet.' : 'Scanning needs an adapter that no pilot is using.'}</div></td></tr>`;
  }
  const off = onState(update);
  if (S.state) update();
  return {unmount: off};
};

// ---- Settings
// ---- OBS: every source Play League offers (what it is, its size, a copy button, a live preview), and the scenes
// (switch from here; automatic switching follows the race). Scene control needs the Play League window on the race
// PC (it alone can read the saved OBS password).
views.obs = el => {
  const rx = isRx(), base = location.origin;
  let alive = true, current = '', sig = '';
  el.innerHTML = `
    <div class="view-head"><h1>OBS</h1><span class="sub">Everything to put in OBS: each feed as a source.</span></div>
    <div class="card"><div class="card-h"><h3>Sources</h3><div class="spacer"></div><span class="faint">OBS: Sources → + → Browser, paste the URL, set the size shown</span></div>
      <div class="card-b flush" id="o-src"></div></div>
    <p class="hint">Raw video (<span class="mono">/feed/N.mjpg</span>) is 720 × 480 with non-square pixels: in OBS, right-click it → Transform →
    Edit Transform → Bounding Box Type <b>Stretch to bounds</b>, 4:3. For every source, right-click → Scale Filtering → <b>Lanczos</b>.</p>`;

  // ---------------- sources
  const pick = {};
  try { Object.assign(pick, JSON.parse(localStorage.getItem('obs.pick') || '{}')); } catch { /* none */ }
  // what each source shows (the options build its URL; OBS needs the URL copied again after a change)
  const qs = o => { const s = Object.entries(o).filter(([, v]) => v != null).map(([k, v]) => `${k}=${v}`).join('&'); return s ? '?' + s : ''; };
  const pilotOpts = () => qs({name: pick.pname === false ? 0 : null, laps: pick.plaps === false ? 0 : null, slot: pick.pslot ? 1 : null});
  const chk = (k, label, on) => `<label class="row tight"><input type="checkbox" class="switch" data-pick="${k}" ${on ? 'checked' : ''}><span>${label}</span></label>`;
  const sources = () => {
    const out = [];
    const feeds = usedSlots();
    for (const s of feeds) out.push({group: rx ? 'Feeds' : 'Pilot feeds', key: 'p' + s.slot, title: `${esc(s.name)} <span class="faint">${esc(s.channel)}</span>`,
      what: rx ? 'This feed\'s video.' : 'This slot\'s video, with the pilot\'s name tag and lap counter. The URL stays with the slot when the pilot changes.',
      path: `/pilot/${s.slot}${rx ? '' : pilotOpts()}`, size: '1440 × 1080', aspect: '4 / 3',
      opt: rx ? '' : `<div class="row" style="gap:6px 16px;flex-wrap:wrap">${chk('pname', 'Name tag', pick.pname !== false)}${chk('plaps', 'Lap counter', pick.plaps !== false)}${chk('pslot', 'Slot number', !!pick.pslot)}
        <span class="faint">(all pilot feeds)</span></div>`});
    for (const s of usedSlots()) out.push({group: 'Raw video', key: 'r' + s.slot, title: `${esc(s.name)} <span class="faint">${esc(s.channel)}</span>`,
      what: 'The picture only, as Motion JPEG (for an OBS Media Source or other software). <span class="mono">/snapshot/' + s.slot + '.jpg</span> is one frame.',
      path: `/feed/${s.slot}.mjpg`, size: '720 × 480 → stretch to 4:3', raw: true});
    return out;
  };
  const open = new Set();
  function drawSources() {
    const list = sources();
    const s = list.map(x => x.key + x.path).join('|') + [...open].join(',');
    if (s === sig) return;
    sig = s;
    let html = '', g = '';
    for (const x of list) {
      if (x.group !== g) { g = x.group; html += `<div class="o-group">${esc(g)}</div>`; }
      const url = base + x.path;
      html += `<div class="o-src" data-key="${x.key}"><div class="o-main"><div class="o-t">${x.title}</div><div class="o-what">${x.what}</div>
          ${x.opt ? `<div class="o-opt">${x.opt}</div>` : ''}
          <div class="row tight o-url"><span class="mono">${esc(url)}</span><button class="btn btn-sm" data-copy="${esc(url)}">Copy</button>
            <span class="pill plain">${x.size}</span><div class="spacer"></div>
            ${x.raw ? '' : `<button class="btn btn-sm btn-ghost" data-prev="${x.key}">${open.has(x.key) ? 'Hide preview' : 'Preview'}</button>`}</div></div>
        ${open.has(x.key) ? `<div class="o-prev${x.aspect ? ' feed' : ''}"><iframe src="${esc(x.path)}" title="preview" loading="lazy"></iframe></div>` : ''}</div>`;
    }
    $('#o-src', el).innerHTML = html || '<div class="empty" style="padding:var(--s4)">No sources yet: add a pilot on the Slots page.</div>';
  }
  $('#o-src', el).addEventListener('click', e => {
    const c = e.target.closest('[data-copy]');
    if (c) { copy(c.dataset.copy).then(() => toast('Copied: paste it into an OBS Browser Source')); return; }
    const p = e.target.closest('[data-prev]');
    if (p) { open.has(p.dataset.prev) ? open.delete(p.dataset.prev) : open.add(p.dataset.prev); drawSources(); }
  });
  $('#o-src', el).addEventListener('change', e => {
    const k = e.target.dataset.pick;
    if (!k) return;
    pick[k] = e.target.type === 'checkbox' ? e.target.checked : k === 'top' ? Math.max(3, Math.min(32, Number(e.target.value) || 10)) : e.target.value;
    try { localStorage.setItem('obs.pick', JSON.stringify(pick)); } catch { /* private window */ }
    drawSources();
  });
  const off = onState(drawSources);
  drawSources();
  return {unmount() { alive = false; off(); obsShow = () => {}; }};
};

const SETTINGS_SECTIONS = [
  ['receivers', 'Receivers', 'Bands, sample rate, Diversity, tuning', ''],
  ['picture', 'Picture', 'Smooth lines, OSD edges, sparkle repair, comb filter', ''],
  ['obs', 'Video out', 'FPV Trackside, cameras', ''],
  ['data', 'Network', 'Other devices', ''],
];
views.settings = (el, arg) => {
  const secs = SETTINGS_SECTIONS.filter(x => !(isRx() && x[3]));
  let sec = secs.some(x => x[0] === arg) ? arg : (() => { try { return localStorage.getItem('settings.sec'); } catch { return null; } })();
  if (!secs.some(x => x[0] === sec)) sec = secs[0][0];
  el.innerHTML = `
    <div class="view-head"><h1>Settings</h1><span class="sub">Saved on this PC and applied straight away unless noted.</span></div>
    <div class="set-layout"><nav class="set-nav" id="set-nav">${secs.map(([k, t, d, c]) => `<a href="#/settings/${k}" data-sec="${k}" class="${c}">
        <b>${t}</b><span>${d}</span></a>`).join('')}</nav>
    <div class="set-body">
    <div class="card" data-sec="receivers"><div class="card-h"><h3>Channels</h3></div><div class="card-b" style="padding-top:0;padding-bottom:0">
      <div class="setting"><div><div class="setting-t">Bands available</div>
        <div class="setting-d">Which bands the channel pickers offer. Pilots already on a band you turn off keep their channel.</div></div>
        <div id="bands"></div></div>
    </div></div>
    <div class="card" data-sec="receivers"><div class="card-h"><h3>Receivers</h3></div><div class="card-b" style="padding-top:0;padding-bottom:0">
      <div class="setting"><div><div class="setting-t">Sample rate</div>
        <div class="setting-d"><b>20 MS/s</b> (recommended): streams without dropouts on Windows; captures ~18 MHz around the channel.
        <b>40 MS/s</b>: ~35 MHz, more margin for off-frequency VTXs, but needs a USB port that never pauses more than ~0.2 ms;
        on many Windows PCs it drops out about once a second. Applies to every receiver straight away.</div></div>
        <select class="select" data-s="msps"><option value="20">20 MS/s</option><option value="40">40 MS/s</option></select></div>
      <div class="setting"><div><div class="setting-t">Diversity</div>
        <div class="setting-d">Video from both of each adapter's antennas, like diversity goggles: A and B are combined into one
        cleaner picture, so a fade on one antenna is covered by the other. Fit two video antennas to every adapter.
        Costs about 10% of a CPU core per adapter. Applies to every receiver straight away.</div></div>
        <input type="checkbox" class="switch" data-s="diversity"></div>
      <div class="setting"><div><div class="setting-t">Tune beside the channel</div>
        <div class="setting-d">Each adapter tunes about 4 MHz to one side of its pilot's channel. Its chip filters out whatever
        sits at the very centre of its tuning, and on the channel itself that is the picture's black level: the result is
        stripes at the left edge and faint ghost outlines. On (recommended) keeps the picture clear of it. Retunes every
        receiver straight away.</div></div>
        <input type="checkbox" class="switch" data-s="offset_tune"></div>
    </div></div>
    <div class="card" data-sec="picture"><div class="card-h"><h3>Picture</h3><div class="spacer"></div><span class="faint">every receiver, straight away; none adds delay</span></div><div class="card-b" style="padding-top:0;padding-bottom:0">
      <div class="setting"><div><div class="setting-t">Smooth lines</div>
        <div class="setting-d">Analog video sends every other line of the picture at a time. Off: each of those lines is shown
        twice (stair-stepped edges, and a slight up-and-down shimmer). On: each line goes in its own place and the ones
        between are filled in along the edges. Smoother diagonals, a steady picture, no delay. Applies straight away.</div></div>
        <input type="checkbox" class="switch" data-s="smooth_lines"></div>
      <div class="setting"><div><div class="setting-t">Clean OSD edges</div>
        <div class="setting-d">An adaptive comb filter (NTSC, with the comb filter on): sharp black / white edges, like OSD text,
        no longer show rainbow fringes or a faint halo. Where the line above is clearly different the picture falls back to
        the simpler filter there, and the colour is separated line against line. Adds no delay. Applies straight away.</div></div>
        <input type="checkbox" class="switch" data-s="acomb"></div>
      <div class="setting"><div><div class="setting-t">Sparkle repair</div>
        <div class="setting-d">For a weak picture on a single antenna: the white and black sparkles (where the signal slips for an instant)
        are patched over from the picture either side, and a badly broken line is redrawn from the lines above and below it.
        Little to gain with Diversity on or a strong signal: leave it off there. Adds no delay. Applies straight away.</div></div>
        <input type="checkbox" class="switch" data-s="repair"></div>
      <div class="setting"><div><div class="setting-t">Comb filter (NTSC)</div>
        <div class="setting-d">Separates colour from brightness using the line before: no echo rings after edges. Off: the classic
        notch filter. Applies to every receiver straight away.</div></div>
        <input type="checkbox" class="switch" data-s="comb"></div>
    </div></div>
    <div class="card" data-sec="obs" id="ts-card"><div class="card-h"><h3>FPV Trackside</h3><div class="spacer"></div><span class="pill" id="ts-pill"></span></div>
      <div class="card-b" style="padding-top:0;padding-bottom:0">
      <div class="setting"><div><div class="setting-t">Send the video feeds to FPV Trackside</div>
        <div class="setting-d">Every pilot's video goes to Trackside as its own stream. In Trackside: <b>Video Setup → Add → RTMP Server</b>,
        once per pilot, keeping the port it suggests: the first one (19301) shows pilot 1, the next (19302) pilot 2, and so on.
        </div></div>
        <input type="checkbox" class="switch" data-s="ts_on"></div>
      <div class="setting"><div><div class="setting-t">Trackside computer</div>
        <div class="setting-d">127.0.0.1 when Trackside runs on this computer; otherwise its address on the network (e.g. 192.168.1.20),
        for example to send a Raspberry Pi's feeds to the Trackside PC.</div></div>
        <input class="input" data-s="ts_host" data-t="str" style="width:160px"></div>
      <div class="setting"><div><div class="setting-t">First port, frame rate and quality</div>
        <div class="setting-d">Pilot 1 uses the first port, pilot 2 the next, and so on (Trackside starts at 19301). 30 frames a second is
        plenty for a screen and halves the work; 2500 kbit/s per pilot looks clean.</div></div>
        <div class="row"><input class="input num" data-s="ts_port" type="number" min="1" max="65000" style="width:80px">
        <select class="select" data-s="ts_fps"><option value="30">30 fps</option><option value="60">60 fps</option></select>
        <input class="input num" data-s="ts_kbps" type="number" min="300" max="20000" step="100" style="width:80px"><span class="faint">kbit/s</span></div></div>
      <div class="setting"><div><div class="setting-t">Picture shape</div>
        <div class="setting-d">Trackside shows each stream in a 4:3 pilot view, so <b>Fill</b> (recommended) sends the picture filling the stream
        and Trackside brings it back to its true 4:3 shape. <b>Side bars</b> keeps it 4:3 inside a 16:9 stream with black bars either side:
        use it only if the picture looks too wide in a 16:9 layout.</div></div>
        <select class="select" data-s="ts_bars" data-t="bool"><option value="false">Fill</option><option value="true">Side bars</option></select></div>
      <div class="setting" style="display:block"><div class="setting-t" style="margin-bottom:6px">Streams</div><div id="ts-list" class="stack" style="gap:4px"></div></div>
    </div></div>
    <div class="card ${vcamOk() ? '' : 'hidden'}" data-sec="obs" id="cam-card"><div class="card-h"><h3>Cameras</h3><div class="spacer"></div><span class="pill" id="cam-pill"></span></div>
      <div class="card-b" style="padding-top:0;padding-bottom:0">
      <div class="setting"><div><div class="setting-t">Slots as cameras</div>
        <div class="setting-d">A slot switched on here (or in its row on the ${isRx() ? 'Feeds' : 'Slots'} page) shows up as a webcam,
        “${esc(camName(1))}”, “${esc(camName(2))}” …, in Discord, FPV Trackside, OBS, Zoom and other programs, with no delay added.
        Pick it in that program's camera or video settings. Restart the program after switching one on: most only look for cameras when they start.
        Only the ones switched on are listed. A camera with no pilot (or no video) shows a “no video” card.</div>
        <div class="setting-d" id="cam-err"></div></div></div>
      <div class="setting" style="display:block"><div class="row" id="cam-list" style="gap:8px 18px"></div></div>
    </div></div>
    <div class="card" data-sec="data"><div class="card-h"><h3>Network</h3></div><div class="card-b" style="padding-top:0;padding-bottom:0">
      <div class="setting"><div><div class="setting-t">Allow other devices on the network</div>
        <div class="setting-d">Lets a tablet or another PC open the app and the OBS feeds at this PC's address.
        Windows asks to allow it through the firewall the first time. <b>Takes effect when the app restarts.</b></div>
        <div class="setting-d" id="lan-now"></div></div>
        <input type="checkbox" class="switch" id="lan"></div>
      <div class="setting"><div><div class="setting-t">Server address</div><div class="setting-d">The app and the OBS feeds.</div></div>
        <span class="mono" id="addr"></span></div>
    </div></div>
    </div></div>`;
  // one section at a time; the address follows it (#/settings/picture) without rebuilding the page
  const showSec = k => {
    sec = k;
    $$('#set-nav [data-sec]', el).forEach(x => x.classList.toggle('on', x.dataset.sec === k));
    $$('.set-body > .card[data-sec]', el).forEach(x => x.classList.toggle('hidden', x.dataset.sec !== k || (x.id === 'cam-card' && !vcamOk())));
    try { localStorage.setItem('settings.sec', k); } catch { /* private window */ }
    if (location.hash !== '#/settings/' + k) history.replaceState(null, '', '#/settings/' + k);
  };
  $('#set-nav', el).addEventListener('click', e => {
    const x = e.target.closest('[data-sec]');
    if (!x) return;
    e.preventDefault();
    showSec(x.dataset.sec);
  });
  showSec(sec);
  const s = S.settings;
  multiSelect($('#bands', el), {
    options: BANDS, selected: s.bands, min: 1,
    summary: (sel, names) => (sel.length === BANDS.length ? 'All bands' : names.join(', ')),
    async onChange(bands) {
      S.settings = await post('/api/settings', {bands});
      toast(`Channel pickers now offer ${bands.length === BANDS.length ? 'all bands' : bands.length === 1 ? BANDS.find(b => b.value === bands[0]).label : bands.length + ' bands'}`);
    },
  });
  // race start & voice: every control with data-s="key" saves on change
  for (const c of $$('[data-s]', el)) {
    const k = c.dataset.s;
    if (c.type === 'checkbox') c.checked = !!s[k]; else c.value = c.dataset.t === 'bool' ? String(!!s[k]) : s[k];
    c.addEventListener('change', async () => {
      const v = c.type === 'checkbox' ? c.checked : c.dataset.t === 'str' ? c.value : c.dataset.t === 'bool' ? c.value === 'true' : Number(c.value);
      S.settings = await post('/api/settings', {[k]: v});
      if (c.tagName === 'INPUT' && c.dataset.t === 'str' && v.trim() && String(S.settings[k] ?? '') !== v) {
        c.value = S.settings[k] ?? '';
        toast(k === 'obs_url' ? 'The OBS address starts with ws:// (e.g. ws://localhost:4455): kept the previous one' : 'Not accepted: kept the previous value');
      }
      for (const o of $$('[data-s="' + k + '"]', el)) if (o !== c && o.type === 'checkbox') o.checked = !!S.settings[k];  // the same switch shown twice
    });
  }
  const lan = $('#lan', el);
  lan.checked = s.lan;
  lan.onchange = async () => { S.settings = await post('/api/settings', {lan: lan.checked}); show(); toast('Restart the app to apply'); };
  // ---- FPV Trackside card: each pilot's stream and how it is doing
  const tsShow = () => {
    const t = S.state?.trackside, pill = $('#ts-pill', el), list = $('#ts-list', el);
    if (!t || !pill || !list) return;
    const slots = usedSlots(), live = t.slots.filter(x => x.state === 'live').length;
    pill.className = 'pill ' + (!t.on ? '' : !t.supported ? 'pill-bad' : live ? 'pill-good' : 'pill-warn');
    pill.textContent = !t.on ? 'Off' : !t.supported ? 'Not on this computer yet' : live ? `${live} live` : 'Waiting for Trackside';
    const lbl = {live: ['Live', 'pill-good'], waiting: ['Waiting for Trackside', 'pill-warn'], connecting: ['Connecting', 'pill-accent'],
                 error: ['Error', 'pill-bad'], unsupported: ['Not available', 'pill-bad'], idle: ['No video', ''], off: ['Off', '']};
    const h = !t.on ? '<span class="faint">Off.</span>' : slots.length ? slots.map(p => {
      const x = t.slots[p.slot - 1] || {}, [txt, cls] = lbl[x.state] || [x.state || '?', ''];
      return `<div class="row tight" style="flex-wrap:wrap"><b style="min-width:110px">${p.slot} · ${esc(p.name)}</b><span class="pill ${cls}">${txt}${x.state === 'live' && x.fps ? ' · ' + x.fps + ' fps' : ''}</span>
        <span class="mono faint" style="word-break:break-all">${esc(x.url || '')}</span>${x.error && x.state !== 'live' ? `<span class="faint">${esc(x.error)}</span>` : ''}</div>`;
    }).join('') : '<span class="faint">No pilots.</span>';
    if (list.innerHTML !== h) list.innerHTML = h;
  };
  const offTs = onState(tsShow);
  tsShow();
  // ---- Cameras card: which slots are listed as webcams
  const camShow = () => {
    const box = $('#cam-list', el);
    if (!box) return;
    $('#cam-card', el).classList.toggle('hidden', !vcamOk() || sec !== 'obs');  // its own section only
    const on = [1, 2, 3, 4, 5, 6, 7, 8].filter(camOn), err = S.state?.vcam?.error || '';
    const pill = $('#cam-pill', el);
    pill.className = 'pill ' + (err ? 'pill-bad' : on.length ? 'pill-good' : '');
    pill.textContent = err ? 'Problem' : on.length ? `${on.length} on` : 'Off';
    $('#cam-err', el).textContent = err;
    if (editing(box)) return;
    const names = Object.fromEntries(usedSlots().map(p => [p.slot, p.name]));
    const h = [1, 2, 3, 4, 5, 6, 7, 8].map(n => `<label class="row tight" style="flex-wrap:nowrap"><input type="checkbox" class="switch" data-cam="${n}" ${camOn(n) ? 'checked' : ''}>
      <span><b>${esc(camName(n))}</b>${names[n] ? ` <span class="faint">· ${esc(names[n])}</span>` : ''}</span></label>`).join('');
    if (box.dataset.h !== h) { box.dataset.h = h; box.innerHTML = h; }
  };
  $('#cam-list', el).addEventListener('change', async e => {
    const n = Number(e.target.dataset.cam);
    if (!n) return;
    await setCam(n, e.target.checked);
    e.target.blur();
    camShow();
  });
  const offCam = onState(camShow);
  camShow();
  $('[data-s=diversity]', el).addEventListener('change', e => toast(e.target.checked ? 'Diversity on: both antennas on every adapter' : 'Diversity off: antenna A only'));
  let alive = true;
  const show = () => {
    $('#addr', el).textContent = `http://${S.settings.lan_active ? '<this PC>' : 'localhost'}:${S.settings.port}/`;
    $('#lan-now', el).textContent = S.settings.lan_active ? 'Right now: on.' : 'Right now: this PC only.';
  };
  show();
  return {unmount() { alive = false; offTs(); offCam(); }};
};

// ---- Log
// ---- Feed node: lend this PC's adapters to a race PC on the network (shared cameras). Here: switch the mode
// on, see what is being sent. On a race PC: which feed nodes it can see, and nodes added by address.
views.node = el => {
  let alive = true, vid = null, sig = '';
  el.innerHTML = `
    <div class="view-head"><h1>Feed node</h1><span class="sub">Share cameras: more adapters than one PC's USB can carry, on a second PC.</span></div>
    <div class="card"><div class="card-h"><div><h3>This PC as a feed node</h3>
        <div class="faint">Its adapters receive feeds for another PC running Play RX on the network (the main PC), which picks the channels and shows and sends the video.</div></div>
        <div class="spacer"></div><input type="checkbox" class="switch" id="nd-on"></div>
      <div class="card-b stack" id="nd-body"></div></div>
    <div class="card" id="nd-race"><div class="card-h"><div><h3>On the main PC</h3>
        <div class="faint">Use a feed node's adapters for a feed: Feeds → the feed's <b>Video</b> → <b>Network · node name</b>.</div></div></div>
      <div class="card-b stack">
        <div class="setting" style="padding:0"><div><div class="setting-t">Feed nodes found on the network</div>
          <div class="setting-d" id="nd-heard">Looking…</div></div></div>
        <div class="setting" style="padding:0"><div><div class="setting-t">Add a feed node by address</div>
          <div class="setting-d">If it isn't found by itself (another subnet, a firewall): its address as the node's Feed node page shows it,
          e.g. <span class="mono">192.168.1.20:8090</span>. Separate several with commas.</div></div>
          <div class="row tight"><input class="input mono" id="nd-manual" style="width:260px" placeholder="192.168.1.20:8090"><button class="btn btn-sm" id="nd-save">Save</button></div></div>
      </div></div>
    <div class="card"><div class="card-h"><h3>How it works</h3></div><div class="card-b"><ul class="hint" style="margin:0;padding-left:18px;line-height:1.6">
      <li>Each USB 3 controller carries only so many adapters. A second PC (a laptop, a mini PC) with its own adapters adds more.</li>
      <li>Connect both PCs by <b>wired Ethernet</b> (gigabit) if you can: each feed is about 25 Mbit/s of video. Wi-Fi works for one or two feeds but adds delay and drop-outs.</li>
      <li>The node sends each feed's picture. OBS, FPV Trackside and the cameras all run on the main PC, exactly as for its own adapters.</li>
      <li>The first time, Windows asks to let the app through the firewall on both PCs: allow it on private networks.</li></ul></div></div>`;
  const on = () => !!S.settings.node_mode;
  function draw() {
    if (!alive) return;
    $('#nd-on', el).checked = on();
    $('#nd-race', el).classList.toggle('hidden', on());
    const st = S.state || {}, body = $('#nd-body', el);
    if (!on()) {
      if (vid) { vid.close(); vid = null; }
      sig = '';
        body.innerHTML = '<p class="hint" style="margin:0">Off: this PC shows its own feeds. Switch on to lend its adapters to a main PC: the menu narrows to what a node needs, and the main PC finds it on the network.</p>';
      return;
    }
    const lent = (st.slots || []).filter(s => s.used);
    const s = `${st.lan}|${(st.ips || []).join()}|${st.host}|` + lent.map(x => [x.slot, x.name, x.channel, x.state, x.node_streams, x.node_lent].join(':')).join('|');
    if (s === sig) return;
    sig = s;
    const addr = (st.ips || []).map(ip => `<div class="nd-addr">${esc(ip)}:${st.port}</div>`).join('') || '<div class="faint">No network address (not connected?)</div>';
    body.innerHTML = `
      ${st.lan ? '' : `<div class="card empty" style="text-align:left"><b>Restart the app to finish</b>The main PC can reach this PC only after a restart (other devices on the network were not allowed yet).</div>`}
      <div class="row" style="align-items:flex-start;gap:var(--s6);flex-wrap:wrap">
        <div><div class="faint">Main PCs see it as</div><input class="input" id="nd-name" style="width:220px;font-weight:600" maxlength="40" value="${esc(st.host || '')}" title="Change it, then press Enter"></div>
        <div><div class="faint">Address (if the main PC doesn't find it by itself)</div>${addr}</div>
        <div><div class="faint">Adapters</div><b style="font-size:var(--fs-lg)">${(st.adapters || []).length}</b></div></div>
      <div class="o-group" style="margin:0 calc(-1 * var(--s4))">Feeds the main PC takes from this node's adapters</div>
      ${lent.length ? `<div class="nd-slots">${lent.map(x => {
        const lab = STATE_LABEL[x.state] || [x.state, ''];
        return `<div class="nd-slot"><canvas data-slot="${x.slot}"></canvas><div class="nd-cap"><b>${esc(x.name)}</b><span class="faint">${esc(x.channel)}</span>
          <span class="pill ${lab[1]}">${esc(lab[0])}</span><div class="spacer"></div>
          ${x.node_streams ? '<span class="pill pill-good plain">Sending</span>' : '<span class="pill plain">Not sent</span>'}</div></div>`;
      }).join('')}</div>` : '<div class="empty" style="padding:var(--s4)"><b>Waiting for the main PC</b>On the main PC: Feeds → a feed\'s Video → Network · ' + esc(st.host || 'this node') + '.</div>'}`;
    if (vid) vid.close();
    vid = lent.length ? video(lent.map(x => x.slot), n => $(`canvas[data-slot="${n}"]`, el)) : null;
  }
  el.addEventListener('change', async e => {  // the node's name, as race PCs list it
    if (e.target.id !== 'nd-name') return;
    S.settings = await post('/api/settings', {node_name: e.target.value.trim()});
    toast('Renamed');
    sig = '';
    await pollState();
  });
  $('#nd-on', el).onchange = async e => {
    const want = e.target.checked;
    if (want && !await ask({title: 'Make this PC a feed node?', ok: 'Make it a feed node', text:
      'Its adapters will receive feeds for a main PC on the network. The menu narrows to the node\'s pages, '
      + 'and other devices on the network are allowed to connect (Windows may ask about the firewall).'
      + (S.state?.lan ? '' : '<br><br><b>The app needs a restart</b> afterwards so the main PC can reach it.')})) { e.target.checked = false; return; }
    S.settings = await post('/api/settings', {node_mode: want});
    toast(want ? 'Feed node mode on' : 'Feed node mode off: this PC shows its own feeds again');
    route();
  };
  // race PC: nodes heard (through /api/captures, the same list the Slots page offers)
  async function heard() {
    if (!alive || on()) return;
    const l = await get('/api/captures').catch(() => []);
    const nodes = l.filter(c => c.net);
    $('#nd-heard', el).innerHTML = nodes.length ? nodes.map(n => `<div><b>${esc(n.name)}</b> <span class="mono faint">${esc(n.id.slice(4))}</span></div>`).join('')
      : 'None yet. Switch on feed node mode on the other PC (and restart it once); it shows up here within a few seconds.';
  }
  $('#nd-manual', el).value = S.settings.nodes || '';
  $('#nd-save', el).onclick = async () => {
    S.settings = await post('/api/settings', {nodes: $('#nd-manual', el).value.trim()});
    toast('Saved');
    heard();
  };
  every(3000, heard, () => alive);
  heard();
  const off = onState(draw);
  draw();
  return {unmount() { alive = false; off(); if (vid) vid.close(); }};
};

views.log = el => {
  el.innerHTML = `<div class="view-head"><h1>Log</h1></div><div class="card"><div class="log" id="log"></div></div>`;
  const box = $('#log', el);
  let seq = 0, alive = true;
  const stop = every(500, async () => {
    const j = await get('/api/log?since=' + seq);
    if (!j.lines.length) return;
    seq = j.seq;
    const atEnd = box.scrollTop + box.clientHeight >= box.scrollHeight - 20;
    box.insertAdjacentHTML('beforeend', j.lines.map(l => `<div class="${esc(l[0])}">${esc(l)}</div>`).join(''));
    while (box.childElementCount > 1000) box.firstElementChild.remove();
    if (atEnd) box.scrollTop = box.scrollHeight;
  }, () => alive);
  return {unmount() { alive = false; stop(); }};
};

// ---------------------------------------------------------------- start
(async function start() {
  try { S.channels = await get('/api/channels'); } catch { S.channels = [{name: 'R1', band: 'R', mhz: 5658}]; }
  try { S.settings = await get('/api/settings'); } catch { /* defaults */ }
  try { S.state = await get('/api/state'); } catch { /* the engine may still be starting */ }
  applyEdition();  // before the first full poll: Play RX must not ask for the race start
  await pollState().catch(() => {});
  route();
  every(400, pollState);
  // live events: a pass, a phase change or a league edit refreshes at once (the poll is the fallback)
  let soon = 0;
  events(e => {
    if (e.type === 'league' || e.type === 'heat') for (const fn of leagueListeners) fn(e);
    if (!soon) soon = setTimeout(() => { soon = 0; pollState().catch(() => {}); }, 30);
  });
})();
