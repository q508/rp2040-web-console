/* RP2040 三合一 Web 控制台 - 前端逻辑
 * 由 build_web.py 从 combo_webserial.html 自动生成，不要直接改这个文件。
 * 依赖: WebSerial（Chrome / Edge 89+），无第三方库。
 */
/* ================= 公共 ================= */
const $ = id => document.getElementById(id);
const logEl = $('log');
function log(msg){
  const t = new Date().toLocaleTimeString('zh-CN', {hour12:false});
  logEl.textContent = `[${t}] ${msg}\n` + logEl.textContent;
  const lines = logEl.textContent.split('\n');
  if (lines.length > 100) logEl.textContent = lines.slice(0, 100).join('\n');
}
function setPill(el, text, cls){
  el.textContent = text;
  el.className = 'pill' + (cls ? ' ' + cls : '');
}
function sleep(ms){ return new Promise(r => setTimeout(r, ms)); }

/* 页面打开后自动连一次（用已授权的端口，不需要用户手势） */
setTimeout(() => autoConnect('启动'), 700);

if (!('serial' in navigator)) {
  $('warn').innerHTML = '当前浏览器不支持 WebSerial，请用 <b>Edge</b> 或 <b>Chrome</b> 打开本文件（Firefox / Safari 不支持）。';
  $('warn').style.display = 'block';
  $('btnPwmConnect').disabled = true;
  $('btnLaConnect').disabled = true;
} else if (location.protocol === 'file:') {
  $('warn').className = 'warn ok';
  $('warn').textContent = '本地文件模式：双击即可使用，WebSerial 正常。连接时在弹出的串口列表里选 RP2040 的对应端口。';
  $('warn').style.display = 'block';
}

/* ================= Tab ================= */
document.querySelectorAll('.tab').forEach(btn => {
  btn.onclick = () => {
    document.querySelectorAll('.tab').forEach(b => b.classList.toggle('active', b === btn));
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.toggle('active', p.id === 'tab-' + btn.dataset.tab));
  };
});

/* ================= PWM ================= */
let pwmPort = null, pwmWriter = null, pwmReader = null, pwmConnected = false;
let pwmSeenHeader = false, pwmRows = [];
const pwmState = [
  {freq:1000, duty:50, on:true},
  {freq:1000, duty:50, on:true},
];

function pwmCard(ch){
  const gpio = ch === 0 ? 6 : 7;
  const color = ch === 0 ? 'var(--ch0)' : 'var(--ch1)';
  return `<div class="card ch-card">
    <div class="ch-head">
      <i class="dot" style="background:${color}"></i>
      <b>CH${ch}</b><span class="gpio">GPIO${gpio}</span>
      <span id="pwmState${ch}" class="state">--</span>
    </div>
    <div class="row">
      <label>频率</label>
      <input id="pwmFreq${ch}" type="number" min="1" max="20000000" step="1" value="1000">
      <select id="pwmUnit${ch}">
        <option value="1">Hz</option>
        <option value="1000">kHz</option>
        <option value="1000000">MHz</option>
      </select>
    </div>
    <div class="row">
      <label>占空比</label>
      <input id="pwmDuty${ch}" type="range" min="0" max="100" step="0.1" value="50">
      <input id="pwmDutyNum${ch}" type="number" min="0" max="100" step="0.1" value="50">
      <span class="unit">%</span>
    </div>
    <div class="row">
      <label>输出</label>
      <label class="switch"><input id="pwmOn${ch}" type="checkbox" checked><span></span></label>
      <button id="pwmApply${ch}" class="ghost" disabled>应用</button>
    </div>
  </div>`;
}
$('pwmGrid').innerHTML = pwmCard(0) + pwmCard(1);

for (let ch = 0; ch < 2; ch++){
  const range = $('pwmDuty' + ch), num = $('pwmDutyNum' + ch);
  range.oninput = () => { num.value = range.value; };
  num.oninput = () => { range.value = num.value; };
  $('pwmApply' + ch).onclick = () => pwmApply(ch);
}

function pwmApply(ch){
  if (!pwmWriter) { log('[PWM] 未连接，无法下发'); return; }
  const f = parseFloat($('pwmFreq' + ch).value);
  const unit = parseInt($('pwmUnit' + ch).value, 10);
  let hz = Math.round((isFinite(f) ? f : 1) * unit);
  hz = Math.max(1, Math.min(20000000, hz));
  let duty = parseFloat($('pwmDutyNum' + ch).value);
  if (!isFinite(duty)) duty = 0;
  duty = Math.max(0, Math.min(100, duty));
  const on = $('pwmOn' + ch).checked;
  const cmd = `f${ch}:${hz}\nd${ch}:${duty.toFixed(1)}\n${on ? 'on' : 'off'}${ch}\ni\n`;
  pwmSend(cmd);
  log(`[PWM] CH${ch} → ${hz} Hz / ${duty.toFixed(1)}% / ${on ? 'ON' : 'OFF'}`);
}

async function pwmSend(text){
  if (!pwmWriter) return;
  try { await pwmWriter.write(new TextEncoder().encode(text)); }
  catch(e){ log('[PWM] 写入失败：' + e.message); }
}

function handlePwmLine(line){
  if (line.includes('Ch | GPIO')) { pwmSeenHeader = true; pwmRows = []; return; }
  if (line === 'end'){
    if (pwmRows.length){
      for (const r of pwmRows) if (r.ch < 2) pwmState[r.ch] = r;
      applyPwmState();
    }
    return;
  }
  const m = line.match(/^\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*([\d.]+)\s*\|\s*(ON|OFF)/);
  if (m){
    pwmRows.push({ch:+m[1], gpio:+m[2], freq:+m[3], duty:+m[4], on:m[5] === 'ON'});
    return;
  }
  if (line.trim()) log('[PWM] ' + line);
}

function applyPwmState(){
  for (let ch = 0; ch < 2; ch++){
    const s = pwmState[ch];
    if (!s) continue;
    const freqEl = $('pwmFreq' + ch), unitEl = $('pwmUnit' + ch);
    let unit = 1, val = s.freq;
    if (s.freq >= 1000000 && s.freq % 1000000 === 0) { unit = 1000000; val = s.freq / 1000000; }
    else if (s.freq >= 1000 && s.freq % 1000 === 0) { unit = 1000; val = s.freq / 1000; }
    unitEl.value = String(unit);
    freqEl.value = String(val);
    $('pwmDuty' + ch).value = String(s.duty);
    $('pwmDutyNum' + ch).value = String(s.duty);
    $('pwmOn' + ch).checked = !!s.on;
    const st = $('pwmState' + ch);
    st.textContent = s.on ? 'ON' : 'OFF';
    st.className = 'state ' + (s.on ? 'on' : 'off');
  }
}

async function pwmReadLoop(){
  let buf = '';
  const dec = new TextDecoder();
  try {
    while (pwmConnected){
      const { value, done } = await pwmReader.read();
      if (done) break;
      buf += dec.decode(value, {stream:true});
      let idx;
      while ((idx = buf.indexOf('\n')) >= 0){
        const line = buf.slice(0, idx).replace(/\r$/, '');
        buf = buf.slice(idx + 1);
        handlePwmLine(line);
      }
    }
  } catch(e){
    if (pwmConnected) log('[PWM] 读取中断：' + e.message);
  }
}

async function openPwm(port){
    pwmPort = port;
    await pwmPort.open({ baudRate: 115200, bufferSize: 4096 });
    pwmWriter = pwmPort.writable.getWriter();
    pwmReader = pwmPort.readable.getReader();
    pwmConnected = true;
    pwmSeenHeader = false;
    setPill($('pwmStatus'), 'PWM 已连接', 'ok');
    $('btnPwmConnect').disabled = true;
    $('btnPwmRead').disabled = false;
    $('btnPwmSave').disabled = false;
    for (let ch = 0; ch < 2; ch++) $('pwmApply' + ch).disabled = false;
    pwmReadLoop();
    pwmSend('i\n');
    setTimeout(() => {
      if (!pwmSeenHeader){
        log('[PWM] 未收到状态表，可能选错端口（PWM 应为 CDC#2）');
        setPill($('pwmStatus'), 'PWM 无响应', 'err');
      }
    }, 1200);
    log('[PWM] 已连接 ' + JSON.stringify(pwmPort.getInfo()));
}

$('btnPwmConnect').onclick = async () => {
  if (!('serial' in navigator)) return;
  try {
    await openPwm(await navigator.serial.requestPort());
  } catch(e){
    log('[PWM] 连接失败：' + e.message);
    setPill($('pwmStatus'), 'PWM 连接失败', 'err');
  }
};
$('btnPwmRead').onclick = () => pwmSend('i\n');
$('btnPwmSave').onclick = () => { pwmSend('save\n'); log('[PWM] 保存设置'); };

/* ================= LA ================= */
const BUF = 1 << 20;
const MASK = BUF - 1;
const ring0 = new Uint8Array(BUF);
const ring1 = new Uint8Array(BUF);
let head = 0, count = 0, totalBytes = 0, totalSamples = 0;
let laPort = null, laWriter = null, laReader = null, laRunning = false;
let lastBytes = 0, lastTime = performance.now(), bps = 0;
let captureWallStart = null, lastDataWall = null;

const cv = $('cv'), ctx = cv.getContext('2d');
const winSel = $('win'), show0 = $('show0'), show1 = $('show1');
const glitchOn = $('glitchOn'), glitchUs = $('glitchUs');
const rateSel = $('sampleRate');
let sampleRate = 1000000;

const MAX_WIN = BUF;   /* 窗口最多铺满整个环，低频大窗口不再越界 */
const view0 = new Uint8Array(MAX_WIN);
const view1 = new Uint8Array(MAX_WIN);

function copyWindow(ring, start, n, out){
  const first = Math.min(n, BUF - start);
  out.set(ring.subarray(start, start + first), 0);
  if (n > first) out.set(ring.subarray(0, n - first), first);
}

function fmtSpan(ms){
  if (!isFinite(ms) || ms <= 0) return '0';
  if (ms < 1) return (ms * 1000).toFixed(0) + ' µs';
  if (ms < 1000) return ms.toFixed(ms < 10 ? 3 : 1) + ' ms';
  return (ms / 1000).toFixed(3) + ' s';
}

function filterRun(arr, n, minRun){
  let i = 0;
  while (i < n){
    let j = i + 1;
    while (j < n && arr[j] === arr[i]) j++;
    if (j - i < minRun && i > 0) arr.fill(arr[i - 1], i, j);
    i = j;
  }
}

/* 流式去毛刺：语义同 filterRun，但只输出已确定的电平；末尾不足 minRun 的游程暂存到下一块 */
function gfNew(){ return { held: new Uint8Array(0), prev: -1, g: 0 }; }

function gfPush(f, arr, g){
  if (f.g !== g){ f.held = new Uint8Array(0); f.prev = -1; f.g = g; }
  const hn = f.held.length, n = hn + arr.length;
  let buf = arr;
  if (hn){
    /* 复用临时缓冲，长跑（8h，约 1.7 亿块）避免持续分配 */
    if (!f.tmp || f.tmp.length < n) f.tmp = new Uint8Array(Math.max(n, 4096));
    buf = f.tmp;
    buf.set(f.held, 0);
    buf.set(arr, hn);
  }
  if (!f.out || f.out.length < n) f.out = new Uint8Array(Math.max(n, 4096));
  const out = f.out;
  let w = 0, i = 0, prev = f.prev, heldAt = -1, heldLen = 0;
  while (i < n){
    let j = i + 1;
    while (j < n && buf[j] === buf[i]) j++;
    const len = j - i;
    if (j === n && len < g){ heldAt = i; heldLen = len; break; }
    const lvl = (len < g && prev >= 0) ? prev : buf[i];
    out.fill(lvl, w, w + len);
    w += len;
    prev = lvl;
    i = j;
  }
  f.held = heldLen ? buf.slice(heldAt, heldAt + heldLen) : new Uint8Array(0);
  f.prev = prev;
  return out.subarray(0, w);
}

/* ===== 无限时长统计：只累计统计量，不保存全量原始采样 ===== */
const longRunOn = $('longRunOn');
const targetHz = [$('targetHz0'), $('targetHz1')];
const targetH = $('targetH');
let statSamples = 0, lastStatLogT = 0, targetDone = false;
let statLog = [];
const stat = [
  {pos:0, edges:0, last:0, high:0, samples:0, prev:0, nPeriods:0, mean:0, m2:0, min:0, max:0, lastPeriod:0, gaps:0},
  {pos:0, edges:0, last:0, high:0, samples:0, prev:0, nPeriods:0, mean:0, m2:0, min:0, max:0, lastPeriod:0, gaps:0}
];
const gf = [gfNew(), gfNew()];

function fmtDur(sec){
  if (!isFinite(sec) || sec < 0) sec = 0;
  if (sec < 60) return sec.toFixed(3) + ' s';
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  const parts = h > 0 ? [h, m, s] : [m, s];
  return parts.map(v => String(v).padStart(2, '0')).join(':');
}

function fmtPeriod(us){
  if (!isFinite(us) || us <= 0) return '--';
  return us >= 1000 ? (us / 1000).toFixed(3) + ' ms' : us.toFixed(3) + ' µs';
}

function statValues(s, ch){
  const meanUs = s.mean / sampleRate * 1e6;
  const lastUs = s.lastPeriod / sampleRate * 1e6;
  const minUs = s.min / sampleRate * 1e6;
  const maxUs = s.max / sampleRate * 1e6;
  const jitUs = s.nPeriods ? Math.sqrt(s.m2 / s.nPeriods) / sampleRate * 1e6 : 0;
  const duty = s.samples ? s.high / s.samples * 100 : 0;
  const freqHz = s.nPeriods ? sampleRate / s.mean : null;
  const target = parseFloat(targetHz[ch].value);
  const ppm = (freqHz !== null && target > 0) ? (freqHz - target) / target * 1e6 : null;
  return {meanUs, lastUs, minUs, maxUs, jitUs, duty, freqHz, ppm};
}

function resetLongStats(){
  statSamples = 0; lastStatLogT = 0; statLog = []; targetDone = false;
  for (const s of stat){
    Object.assign(s, {pos:0, edges:0, last:0, high:0, samples:0, prev:0,
                      nPeriods:0, mean:0, m2:0, min:0, max:0, lastPeriod:0, gaps:0});
  }
  gf[0] = gfNew(); gf[1] = gfNew();
  updateLongUI();
}

/* 去毛刺长度（采样点）：显示、导出、统计共用。1 = 不过滤。
 * 下限 3 个采样：每个边沿旁的窄脉冲宽度是按采样点计的（1~2 点），
 * 低频采样时按 µs 换算会不足 1 点而完全失效（250 kHz/5 µs -> 1 点）。 */
function glitchLen(){
  if (!glitchOn.checked) return 1;
  const us = parseFloat(glitchUs.value) || 0;
  return Math.max(3, Math.round(us * sampleRate / 1e6));
}
const statGlitchLen = glitchLen;

function statSample(ch, v){
  const s = stat[ch];
  const idx = s.pos++;
  if (v) s.high++;
  if (v && !s.prev){
    if (s.edges > 0){
      const p = idx - s.last;
      s.lastPeriod = p;
      const gap = s.nPeriods >= 10 && (p > s.mean * 1.5 || p < s.mean * 0.5);
      if (gap){
        s.gaps++;
      } else {
        s.nPeriods++;
        const d = p - s.mean;
        s.mean += d / s.nPeriods;
        s.m2 += d * (p - s.mean);
        if (s.nPeriods === 1){ s.min = s.max = p; }
        else { if (p < s.min) s.min = p; if (p > s.max) s.max = p; }
      }
    }
    s.edges++;
    s.last = idx;
  }
  s.prev = v;
  s.samples++;
}

function updateLongUI(){
  const on = longRunOn.checked;
  const runS = statSamples / sampleRate;
  const wantH = parseFloat(targetH.value) || 0;
  if (!on){
    $('lrTime').textContent = '统计未启用';
  } else if (wantH > 0){
    const pct = Math.min(100, runS / (wantH * 3600) * 100);
    $('lrTime').textContent = `运行 ${fmtDur(runS)} / ${wantH} h（${pct.toFixed(1)}%）`;
  } else {
    $('lrTime').textContent = '运行 ' + fmtDur(runS);
  }
  const g = statGlitchLen();
  $('lrFilter').textContent = g > 1
    ? `${((g - 0.5) / sampleRate * 1e6).toFixed(1)} µs 以下窄脉冲被剔除`
    : (glitchOn.checked
        ? `阈值低于 1 个采样点（${(1e6 / sampleRate).toFixed(1)} µs），未过滤`
        : '未去毛刺（原始采样）');
  for (let ch = 0; ch < 2; ch++){
    const s = stat[ch];
    $('lrEdges' + ch).textContent = on ? s.edges.toLocaleString() : '--';
    if (!on || s.nPeriods === 0){
      $('lrFreq' + ch).textContent = '--';
      $('lrMean' + ch).textContent = (on && s.edges > 0) ? '等待第 2 个边沿' : '--';
      $('lrLast' + ch).textContent = '--';
      $('lrMinMax' + ch).textContent = '--';
      $('lrJit' + ch).textContent = '--';
      $('lrDuty' + ch).textContent = (on && s.samples) ? (s.high / s.samples * 100).toFixed(2) + '%' : '--';
      $('lrPpm' + ch).textContent = '--';
      $('lrGap' + ch).textContent = on ? s.gaps : '--';
      continue;
    }
    const v = statValues(s, ch);
    $('lrFreq' + ch).textContent = fmtFreq(v.freqHz);
    $('lrMean' + ch).textContent = fmtPeriod(v.meanUs);
    $('lrLast' + ch).textContent = fmtPeriod(v.lastUs);
    $('lrMinMax' + ch).textContent = fmtPeriod(v.minUs) + ' / ' + fmtPeriod(v.maxUs);
    $('lrJit' + ch).textContent = v.jitUs.toFixed(3) + ' µs';
    $('lrDuty' + ch).textContent = v.duty.toFixed(2) + '%';
    $('lrPpm' + ch).textContent = v.ppm === null ? '--' : v.ppm.toFixed(1) + ' ppm';
    $('lrGap' + ch).textContent = s.gaps;
  }
}

/* 统计 CSV 采样：前 1 小时每秒一行，之后每 5 秒一行（8 小时约 8640 行） */
const STAT_CSV_HEAD = 'elapsed_s,' +
  'ch0_mean_us,ch0_last_us,ch0_jitter_us,ch0_ppm,ch0_gaps,' +
  'ch1_mean_us,ch1_last_us,ch1_jitter_us,ch1_ppm,ch1_gaps';

function statLogEvery(t){ return t < 3600 ? 1 : 5; }

function statRow(t){
  const row = [t.toFixed(3)];
  for (let ch = 0; ch < 2; ch++){
    const s = stat[ch], v = statValues(s, ch);
    row.push(s.nPeriods ? v.meanUs.toFixed(6) : '');
    row.push(s.nPeriods ? v.lastUs.toFixed(3) : '');
    row.push(s.nPeriods ? v.jitUs.toFixed(3) : '');
    row.push(v.ppm === null ? '' : v.ppm.toFixed(2));
    row.push(s.gaps);
  }
  return row;
}

$('btnStatReset').onclick = () => {
  resetLongStats();
  log('[LA] 统计已重置（原始缓冲与波形不受影响）');
};

function rateTag(){
  return sampleRate >= 1e6 ? (sampleRate / 1e6).toFixed(0) + 'MHz'
                           : (sampleRate / 1e3).toFixed(0) + 'kHz';
}

$('btnStatCsv').onclick = () => {
  if (!stat[0].samples && !stat[1].samples){ log('[LA] 还没有统计数据'); return; }
  const t = statSamples / sampleRate;
  const row = statRow(t);
  const last = statLog[statLog.length - 1];
  // 末尾若与最后一行同一时刻，替换而不是追加，避免重复行
  if (last && Math.abs(parseFloat(last[0]) - t) < 1e-9) statLog[statLog.length - 1] = row;
  else statLog.push(row);
  const rows = [STAT_CSV_HEAD].concat(statLog.map(r => r.join(',')));
  const blob = new Blob([rows.join('\n') + '\n'], { type: 'text/csv' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `la_stats_${Math.round(t)}s_${rateTag()}.csv`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 3000);
  log(`[LA] 已导出统计 CSV：${statLog.length} 行，累计 ${fmtDur(t)}，` +
      `目标频率 CH0 ${parseFloat(targetHz[0].value) || 0} / CH1 ${parseFloat(targetHz[1].value) || 0} Hz`);
};

let scr0 = new Uint8Array(0), scr1 = new Uint8Array(0);

function ensureScratch(n){
  if (scr0.length < n){
    const cap = 1 << (32 - Math.clz32(Math.max(n, 8192) - 1));
    scr0 = new Uint8Array(cap);
    scr1 = new Uint8Array(cap);
  }
}

function statFeed(ch, arr, n){
  for (let i = 0; i < n; i++) statSample(ch, arr[i]);
}

function decode(bytes){
  const nb = bytes.length, n = nb * 4;
  ensureScratch(n);
  for (let i = 0; i < nb; i++){
    const b = bytes[i], o = i << 2;
    scr0[o]     = b & 1;          scr1[o]     = (b >> 1) & 1;
    scr0[o + 1] = (b >> 2) & 1;   scr1[o + 1] = (b >> 3) & 1;
    scr0[o + 2] = (b >> 4) & 1;   scr1[o + 2] = (b >> 5) & 1;
    scr0[o + 3] = (b >> 6) & 1;   scr1[o + 3] = (b >> 7) & 1;
  }
  let h = head;
  for (let i = 0; i < n; i++){ ring0[h] = scr0[i]; ring1[h] = scr1[i]; h = (h + 1) & MASK; }
  head = h;
  if (count < BUF) count = Math.min(BUF, count + n);
  totalBytes += nb;
  totalSamples += n;
  if (!longRunOn.checked) return;
  const g = statGlitchLen();
  if (g > 1){
    const a0 = gfPush(gf[0], scr0.subarray(0, n), g);
    if (a0.length) statFeed(0, a0, a0.length);
    const a1 = gfPush(gf[1], scr1.subarray(0, n), g);
    if (a1.length) statFeed(1, a1, a1.length);
  } else {
    statFeed(0, scr0, n);
    statFeed(1, scr1, n);
  }
  statSamples += n;
}

async function laReadLoop(){
  try {
    while (laRunning){
      const { value, done } = await laReader.read();
      if (done) break;
      if (value && value.length) {
        decode(value);
        lastDataWall = new Date();
      }
    }
  } catch(e){
    if (laRunning) log('[LA] 读取中断：' + e.message);
  }
}

async function openLa(port){
    laPort = port;
    await laPort.open({ baudRate: 115200, bufferSize: 65536 });
    laWriter = laPort.writable.getWriter();
    setPill($('laStatus'), 'LA 已连接', 'ok');
    $('btnLaConnect').disabled = true;
    $('btnLaStart').disabled = false;
    log('[LA] 已连接 ' + JSON.stringify(laPort.getInfo()));
}

$('btnLaConnect').onclick = async () => {
  if (!('serial' in navigator)) return;
  try {
    await openLa(await navigator.serial.requestPort());
  } catch(e){
    log('[LA] 连接失败：' + e.message);
    setPill($('laStatus'), 'LA 连接失败', 'err');
  }
};

$('btnLaStart').onclick = async () => {
  if (!laPort || !laWriter || laRunning) return;
  try {
    if (!laPort.readable) { log('[LA] 串口不可读，请刷新页面重连'); return; }
    sampleRate = parseInt(rateSel.value, 10) || 2000000;
    rateSel.disabled = true;
    resetLongStats();
    head = 0; count = 0; totalBytes = 0; totalSamples = 0;
    ring0.fill(0); ring1.fill(0);
    captureWallStart = new Date(); lastDataWall = null;
    laReader = laPort.readable.getReader();
    laRunning = true;
    const div = Math.round(100000000 / sampleRate) - 1;
    await laWriter.write(new Uint8Array([
      0x80, div & 0xff, (div >> 8) & 0xff, (div >> 16) & 0xff, (div >> 24) & 0xff,
      0x82, 0, 0, 0, 0,
      0x01
    ]));
    setPill($('laStatus'), 'LA 采集中', 'run');
    $('btnLaStart').disabled = true;
    $('btnLaStop').disabled = false;
    log(`[LA] 开始流式采集：${rateTag()}，divisor=${div}`);
    const before = totalBytes;
    setTimeout(() => {
      if (laRunning && totalBytes === before)
        log('[LA] 1 秒内无数据，可能选错端口（LA 应为 CDC#1）');
    }, 1200);
    laReadLoop();
  } catch(e){
    laRunning = false;
    log('[LA] 启动失败：' + e.message);
    setPill($('laStatus'), 'LA 启动失败', 'err');
  }
};

$('btnLaStop').onclick = async () => {
  laRunning = false;
  try { if (laWriter) await laWriter.write(new Uint8Array([0x00])); } catch(e){}
  try { if (laReader) await laReader.cancel(); } catch(e){}
  laReader = null;
  setPill($('laStatus'), 'LA 已停止', 'idle');
  $('btnLaStart').disabled = false;
  $('btnLaStop').disabled = true;
  rateSel.disabled = false;
  log('[LA] 停止采集（0x00）');
};

/* ================= 自动连接 / 掉线重连 ================= */
/* getPorts() 拿不到接口号，只能靠发命令探出角色：
 *   LA(CDC#1)  认 'mi' -> 回 "LA: ..."
 *   PWM(CDC#2) 认 'i'  -> 回状态表 "Ch | GPIO | ..."
 * 两个口对不认识的命令都是静默（PWM 返回 false、SUMP 的 0x69 不在命令表），所以探测无副作用。 */
/* file:// 下 localStorage 可能抛 SecurityError，包一层免得整段脚本挂掉 */
function lsGet(k, dflt){ try { const v = localStorage.getItem(k); return v === null ? dflt : v; } catch(e){ return dflt; } }
function lsSet(k, v){ try { localStorage.setItem(k, v); } catch(e){} }
let autoEnabled = lsGet('combo.autoConn', '1') !== '0';
let reconnectTimer = null;
/* 断开时记下哪些角色需要恢复，重连只针对它们（否则只连 LA 时会因为 PWM 永远为空而无限重试） */
const wantRoles = { la: false, pwm: false };

async function probePort(port){
  try {
    await port.open({ baudRate: 115200, bufferSize: 4096 });
  } catch(e){
    return null;
  }
  const dec = new TextDecoder();
  let reader = null, writer = null, text = '';
  try {
    reader = port.readable.getReader();
    writer = port.writable.getWriter();

    /* 同一个 reader 里连发两条命令再统一收：一次 open 只能 getReader() 一次，
     * 前面拆成两段读会把流 cancel 掉，第二段永远读不到。 */
    /* 注意：超时后不能丢掉挂起的 reader.read()，否则下次再调 read() 会抛
     * "read already pending"，第二条命令的响应就永远收不到。这里把 pending 存下来复用。 */
    let pending = null;
    const readFor = async (ms) => {
      const deadline = Date.now() + ms;
      let out = '';
      while (Date.now() < deadline){
        if (!pending) pending = reader.read();
        const r = await Promise.race([
          pending,
          new Promise(res => setTimeout(() => res({ timeout: true }), Math.max(40, deadline - Date.now())))
        ]);
        if (r.timeout) break;
        pending = null;
        if (r.done) break;
        if (r.value) out += dec.decode(r.value, { stream: true });
      }
      return out;
    };

    await writer.write(new TextEncoder().encode('mi\n'));
    text += await readFor(420);

    if (!text.includes('LA:')){
      await writer.write(new TextEncoder().encode('i\n'));
      text += await readFor(420);
    }
  } catch(e){
  } finally {
    try { if (writer) writer.releaseLock(); } catch(e){}
    try { if (reader) await reader.cancel(); } catch(e){}
    try { if (reader) reader.releaseLock(); } catch(e){}
  }
  await sleep(60);
  try { await port.close(); } catch(e){}

  if (text.includes('LA:')) return 'la';
  if (text.includes('Ch |') || text.includes('Freq(Hz)')) return 'pwm';
  return null;
}

async function autoConnect(tag){
  if (!autoEnabled || !('serial' in navigator)) return;
  if (laPort && pwmPort) return;
  let ports = [];
  try { ports = await navigator.serial.getPorts(); } catch(e){ return; }
  if (!ports.length){
    if (tag === '启动') log('[自动连接] 还没有授权过的串口，首次请手动点「连接」并选端口');
    return;
  }
  log(`[${tag}] 发现 ${ports.length} 个已授权端口，正在识别角色…`);
  for (const p of ports){
    if (p === laPort || p === pwmPort) continue;
    if (laPort && pwmPort) break;
    const role = await probePort(p);
    try {
      if (role === 'la' && !laPort){ await openLa(p); log('[自动连接] → LA（CDC#1）'); }
      else if (role === 'pwm' && !pwmPort){ await openPwm(p); log('[自动连接] → PWM（CDC#2）'); }
      else if (!role) log('[自动连接] 有端口无响应，跳过');
    } catch(e){
      log('[自动连接] 连接失败：' + e.message);
    }
  }
}

function scheduleReconnect(){
  if (!autoEnabled) return;
  if (reconnectTimer) return;
  if (!wantRoles.la && !wantRoles.pwm) return;
  let tries = 0;
  const tick = async () => {
    reconnectTimer = null;
    if (!autoEnabled) return;
    const need = (wantRoles.la && !laPort) || (wantRoles.pwm && !pwmPort);
    if (!need){
      wantRoles.la = wantRoles.pwm = false;
      log('[自动重连] 已恢复');
      return;
    }
    tries++;
    if (tries > 6){
      wantRoles.la = wantRoles.pwm = false;
      log('[自动重连] 连续 6 次失败，已停止（可手动点连接）');
      return;
    }
    log(`[自动重连] 第 ${tries} 次尝试…`);
    await autoConnect('重连');
    if ((wantRoles.la && !laPort) || (wantRoles.pwm && !pwmPort)) reconnectTimer = setTimeout(tick, 1500);
    else { wantRoles.la = wantRoles.pwm = false; log('[自动重连] 已恢复'); }
  };
  reconnectTimer = setTimeout(tick, 1200);   /* 先等设备重启枚举完 */
}

$('autoConn').checked = autoEnabled;
$('autoConn').onchange = () => {
  autoEnabled = $('autoConn').checked;
  lsSet('combo.autoConn', autoEnabled ? '1' : '0');
  log(`[自动连接] 已${autoEnabled ? '开启' : '关闭'}`);
  if (!autoEnabled && reconnectTimer){ clearTimeout(reconnectTimer); reconnectTimer = null; }
};

navigator.serial?.addEventListener('disconnect', e => {
  if (pwmPort && e.target === pwmPort){
    /* 必须置空，否则 autoConnect 里"已连接"判断会把重连挡掉 */
    pwmPort = null; pwmWriter = null; pwmReader = null;
    pwmConnected = false;
    wantRoles.pwm = true;
    setPill($('pwmStatus'), 'PWM 已断开', 'err');
    $('btnPwmConnect').disabled = false;
    $('btnPwmRead').disabled = true;
    $('btnPwmSave').disabled = true;
    for (let ch = 0; ch < 2; ch++) $('pwmApply' + ch).disabled = true;
    log('[PWM] 串口已断开');
  }
  if (laPort && e.target === laPort){
    laPort = null; laWriter = null; laReader = null;
    laRunning = false;
    wantRoles.la = true;
    setPill($('laStatus'), 'LA 已断开', 'err');
    $('btnLaConnect').disabled = false;
    $('btnLaStart').disabled = true;
    $('btnLaStop').disabled = true;
    rateSel.disabled = false;
    log('[LA] 串口已断开');
  }
  scheduleReconnect();
});

function fmtClock(d){
  if (!d) return '--:--:--.---';
  const t = d.toLocaleTimeString('zh-CN', {hour12:false});
  return t + '.' + String(d.getMilliseconds()).padStart(3, '0');
}

function fmtFreq(f){
  if (!isFinite(f) || f <= 0) return '0 Hz';
  if (f >= 1e6) return (f / 1e6).toFixed(3) + ' MHz';
  if (f >= 1e3) return (f / 1e3).toFixed(3) + ' kHz';
  return f.toFixed(1) + ' Hz';
}

function measure(a0, a1, n){
  if (!n){
    $('mWall').textContent = '--:--:--.--- → --:--:--.---';
    $('mElapsed').textContent = '总采集 0.000 s';
    $('mWindow').textContent = '+0.000 s → +0.000 s';
    $('mWindowLen').textContent = '窗口 0.000 ms';
    $('mCh0').textContent = '--';
    $('mCh1').textContent = '--';
    return;
  }

  const dt = n / sampleRate;

  $('mInterval').textContent = (1e6 / sampleRate).toFixed(3) + ' µs';
  $('mRate').textContent = rateTag().replace('MHz', ' MHz').replace('kHz', ' kHz');

  $('mWall').textContent = fmtClock(captureWallStart) + ' → ' + fmtClock(lastDataWall);
  const elapsed = (captureWallStart && lastDataWall) ? (lastDataWall - captureWallStart) / 1000 : 0;
  $('mElapsed').textContent = '总采集 ' + elapsed.toFixed(3) + ' s';

  const ws = (totalSamples - n) / sampleRate, we = totalSamples / sampleRate;
  $('mWindow').textContent = '+' + ws.toFixed(3) + ' s → +' + we.toFixed(3) + ' s';
  $('mWindowLen').textContent = '窗口 ' + (dt * 1000).toFixed(3) + ' ms';

  for (let ch = 0; ch < 2; ch++){
    const arr = ch === 0 ? a0 : a1;
    let high = 0, edges = 0, falls = 0, transitions = 0;
    let firstEdge = -1, lastEdge = -1, firstFall = -1, lastFall = -1;
    let prev = arr[0];
    for (let i = 0; i < n; i++){
      const v = arr[i];
      if (v) high++;
      if (prev !== v) transitions++;
      if (!prev && v){
        edges++;
        if (firstEdge < 0) firstEdge = i;
        lastEdge = i;
      }
      if (prev && !v){
        falls++;
        if (firstFall < 0) firstFall = i;
        lastFall = i;
      }
      prev = v;
    }
    let duty;
    if (edges >= 2 && lastEdge > firstEdge){
      // 用首尾上升沿之间的完整周期算占空比，避免窗口首尾半周期造成偏差
      let highSpan = 0;
      for (let i = firstEdge; i < lastEdge; i++)
        if (arr[i]) highSpan++;
      duty = highSpan / (lastEdge - firstEdge) * 100;
    } else {
      duty = high / n * 100;
    }
    let freqText;
    if (edges >= 2 && lastEdge > firstEdge){
      // 首尾上升沿跨度法：不受窗口起点相位影响
      freqText = fmtFreq((edges - 1) * sampleRate / (lastEdge - firstEdge));
    } else if (falls >= 2 && lastFall > firstFall){
      // 窗口起点正好压在上升沿上时该沿会被漏掉；改用下降沿跨度，周期相同
      freqText = fmtFreq((falls - 1) * sampleRate / (lastFall - firstFall));
    } else if (transitions === 0){
      // 窗口内完全没有跳变：DC
      freqText = '0 Hz';
    } else {
      // 上升/下降沿都不足 2 个，无法确定周期；不要硬编一个假频率
      freqText = '--';
    }
    $('mCh' + ch).textContent = freqText + ' / ' + duty.toFixed(1) + '%';
  }
}

setInterval(() => {
  const now = performance.now();
  const dt = (now - lastTime) / 1000;
  if (dt > 0){
    bps = (totalBytes - lastBytes) / dt;
    lastBytes = totalBytes;
    lastTime = now;
  }
  $('sRate').textContent = (bps * 4 / 1e6).toFixed(2);
  $('sBps').textContent = (bps / 1024).toFixed(0);
  $('sTotal').textContent = totalSamples.toLocaleString();
  $('sBuf').textContent = (count / sampleRate).toFixed(2);
  if (laRunning && lastDataWall && (Date.now() - lastDataWall.getTime() > 1500)){
    setPill($('laStatus'), 'LA 无数据', 'err');
  } else if (laRunning){
    setPill($('laStatus'), 'LA 采集中', 'run');
  }
  updateLongUI();
  if (longRunOn.checked && statSamples > 0){
    const t = statSamples / sampleRate;
    if (t - lastStatLogT >= statLogEvery(t)){
      lastStatLogT = t;
      statLog.push(statRow(t));
      if (statLog.length > 30000) statLog.shift();   /* 30000 行 ≈ 8h 无裁剪 */
    }
    const wantH = parseFloat(targetH.value) || 0;
    if (wantH > 0 && !targetDone && t >= wantH * 3600){
      targetDone = true;
      const a = statValues(stat[0], 0), b = statValues(stat[1], 1);
      log(`[LA] 已达到目标时长 ${wantH} h：CH0 ${a.ppm === null ? '--' : a.ppm.toFixed(1) + ' ppm'}` +
          `，CH1 ${b.ppm === null ? '--' : b.ppm.toFixed(1) + ' ppm'}；可导出统计 CSV 收工`);
    }
  }
}, 500);

function resize(){
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  if (w === 0 || h === 0) return {w:0, h:0};
  if (cv.width !== Math.floor(w * dpr) || cv.height !== Math.floor(h * dpr)){
    cv.width = Math.floor(w * dpr);
    cv.height = Math.floor(h * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { w, h };
}

function drawTrace(arr, n, top, bottom, color, visible){
  if (!visible || n <= 0) return;
  const w = cv.clientWidth;
  const yH = top + 8, yL = bottom - 8;
  ctx.beginPath();
  let prev = arr[0];
  ctx.moveTo(0, prev ? yH : yL);
  for (let x = 1; x < w; x++){
    const v = arr[Math.floor(x * n / w)];
    ctx.lineTo(x, prev ? yH : yL);
    if (v !== prev) ctx.lineTo(x, v ? yH : yL);
    prev = v;
  }
  ctx.lineTo(w, prev ? yH : yL);
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.6;
  ctx.lineJoin = 'round';
  ctx.stroke();
}

let lastDrawTs = 0;

function draw(){
  /* 长窗口每帧要扫几十万采样，会和解码抢 CPU；降到约 10 fps */
  const reqMs = parseFloat(winSel.value) || 0;
  const now0 = performance.now();
  if (reqMs > 200 && now0 - lastDrawTs < 100){
    requestAnimationFrame(draw);
    return;
  }
  lastDrawTs = now0;
  const { w, h } = resize();
  if (w === 0 || h === 0){ requestAnimationFrame(draw); return; }
  ctx.clearRect(0, 0, w, h);

  const mid = h / 2;
  ctx.fillStyle = '#f8fafc'; ctx.fillRect(0, 0, w, mid - 1);
  ctx.fillStyle = '#ffffff'; ctx.fillRect(0, mid + 1, w, h - mid - 1);

  ctx.strokeStyle = '#e2e8f0'; ctx.lineWidth = 1;
  for (let i = 0; i <= 10; i++){
    const x = (w - 1) * i / 10;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  }
  for (let i = 0; i <= 4; i++){
    const y = (h - 1) * i / 4;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  }
  ctx.strokeStyle = '#cbd5e1';
  ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(w, mid); ctx.stroke();

  const winMs = parseFloat(winSel.value);
  const winSamples = Math.round(winMs * sampleRate / 1000);
  const n = Math.min(winSamples, count);
  const start = (head - n + BUF) & MASK;

  if (count === 0){
    measure(view0, view1, 0);
    ctx.fillStyle = '#94a3b8';
    ctx.font = '14px -apple-system, "Microsoft YaHei", sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('等待数据… 连接 CDC#1 后点“开始”', w / 2, h / 2);
    requestAnimationFrame(draw);
    return;
  }

  copyWindow(ring0, start, n, view0);
  copyWindow(ring1, start, n, view1);
  if (glitchOn.checked){
    const g = glitchLen();
    filterRun(view0, n, g);
    filterRun(view1, n, g);
  }

  measure(view0, view1, n);
  drawTrace(view0, n, 0, mid - 4, '#2563eb', show0.checked);
  drawTrace(view1, n, mid + 4, h, '#16a34a', show1.checked);

  ctx.font = '12px -apple-system, "Microsoft YaHei", sans-serif';
  ctx.textAlign = 'left';
  ctx.fillStyle = 'rgba(255,255,255,0.86)';
  ctx.fillRect(5, 5, 34, 16);
  ctx.fillStyle = '#2563eb'; ctx.fillText('CH0', 10, 17);
  ctx.fillStyle = 'rgba(255,255,255,0.86)';
  ctx.fillRect(5, mid + 7, 34, 16);
  ctx.fillStyle = '#16a34a'; ctx.fillText('CH1', 10, mid + 19);
  ctx.fillStyle = '#94a3b8'; ctx.textAlign = 'right';
  const actualMs = n / sampleRate * 1000;
  const clamped = n < winSamples;
  ctx.fillText(fmtSpan(actualMs), w - 10, h - 10);

  $('tspan').textContent = `0 – ${fmtSpan(actualMs)}` +
    (clamped ? `（环上限 ${fmtSpan(BUF / sampleRate * 1000)}）` : '');
  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);

function exportCount(){
  const range = $('exportRange').value;
  const winMs = parseFloat(winSel.value);
  const winSamples = Math.round(winMs * sampleRate / 1000);
  return range === 'window' ? Math.min(winSamples, count) : count;
}

function exportArrays(n){
  const start = (head - n + BUF) & MASK;
  const a0 = new Uint8Array(n), a1 = new Uint8Array(n);
  copyWindow(ring0, start, n, a0);
  copyWindow(ring1, start, n, a1);
  if (glitchOn.checked){
    const g = glitchLen();
    filterRun(a0, n, g);
    filterRun(a1, n, g);
  }
  return [a0, a1];
}

$('btnCsv').onclick = () => {
  const n = exportCount();
  if (!n){ log('[LA] 没有数据可导出'); return; }
  const [a0, a1] = exportArrays(n);
  const rows = ['time_us,ch0,ch1'];
  for (let i = 0; i < n; i++){
    const t = (i * 1e6 / sampleRate).toFixed(3);
    rows.push(`${t},${a0[i]},${a1[i]}`);
  }
  const tag = glitchOn.checked ? `_filtered${glitchUs.value}us` : '';
  const blob = new Blob([rows.join('\n')], { type: 'text/csv' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `la_capture_${n}samples${tag}.csv`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 3000);
  log(`[LA] 已导出 CSV：${n} 个采样${glitchOn.checked ? '（已去毛刺）' : ''}`);
};

$('btnVcd').onclick = () => {
  const n = exportCount();
  if (!n){ log('[LA] 没有数据可导出'); return; }
  log(`[LA] 正在生成 VCD：${n} 个采样...`);
  setTimeout(() => {
    const [a0, a1] = exportArrays(n);
    const filtered = glitchOn.checked;
    const nsPerSample = Math.round(1e9 / sampleRate);
    const out = [];
    out.push('$comment RP2040 Combo Web LA ' + rateTag() + ' 2ch' +
             (filtered ? ` filtered ${glitchUs.value}us` : '') + ' $end');
    out.push('$timescale 1ns $end');
    out.push('$scope module logic $end');
    out.push('$var wire 1 ! CH0 $end');
    out.push('$var wire 1 " CH1 $end');
    out.push('$upscope $end');
    out.push('$enddefinitions $end');

    let p0 = a0[0], p1 = a1[0];
    out.push('#0');
    out.push('$dumpvars');
    out.push(p0 ? '1!' : '0!');
    out.push(p1 ? '1"' : '0"');
    out.push('$end');

    for (let i = 1; i < n; i++){
      const v0 = a0[i], v1 = a1[i];
      if (v0 !== p0 || v1 !== p1){
        out.push('#' + (i * nsPerSample));
        if (v0 !== p0){ out.push(v0 ? '1!' : '0!'); p0 = v0; }
        if (v1 !== p1){ out.push(v1 ? '1"' : '0"'); p1 = v1; }
      }
    }

    const tag = filtered ? `_filtered${glitchUs.value}us` : '';
    const blob = new Blob([out.join('\n') + '\n'], { type: 'text/plain' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `la_capture_${n}samples${tag}.vcd`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 3000);
    log(`[LA] 已导出 VCD：${n} 个采样${filtered ? '（已去毛刺）' : ''}，${(blob.size / 1024).toFixed(1)} KB`);
  }, 0);
};

/* ================= 进入 BOOTSEL 升级模式 ================= */
const btnBootsel = $('btnBootsel');
const BOOTSEL_LABEL = '进入升级模式';

function bootselLabel(text, ms){
  if (!btnBootsel) return;
  btnBootsel.textContent = text;
  if (ms){
    btnBootsel.disabled = true;
    setTimeout(() => { btnBootsel.textContent = BOOTSEL_LABEL; btnBootsel.disabled = false; }, ms);
  }
}

if (btnBootsel) btnBootsel.onclick = async () => {
  /* CDC#2(PWM) 是纯文本口，直接发命令；CDC#1(LA) 必须带 'm' 前缀才会被当文本处理 */
  const viaPwm = !!pwmWriter;
  const w = viaPwm ? pwmWriter : laWriter;
  if (!w){
    log('[BOOTSEL] 请先连接 LA 或 PWM 串口（任一个即可），否则发不出命令');
    bootselLabel('未连接串口', 2000);
    return;
  }
  try {
    await w.write(new TextEncoder().encode(viaPwm ? 'bootsel\n' : 'mbootsel\n'));
    log(`[BOOTSEL] 已通过 ${viaPwm ? 'PWM(CDC#2)' : 'LA(CDC#1)'} 发送升级命令，`
        + '设备会重新枚举成 RPI-RP2 盘；之后运行 flash_uf2.bat 或手动拖 .uf2');
    bootselLabel('已发送 ✓', 5000);
  } catch (e){
    log('[BOOTSEL] 发送失败：' + e.message);
    bootselLabel('发送失败', 2000);
  }
};

window.addEventListener('beforeunload', (e) => {
  try { if (laWriter && laRunning) laWriter.write(new Uint8Array([0x00])); } catch(e){}
  if (laRunning){
    /* 长跑误关页面会丢掉全部统计，拦一下 */
    e.preventDefault();
    e.returnValue = '';
    return '';
  }
});
