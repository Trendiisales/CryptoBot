// CryptoBot app.js
// Adapted from ChimeraCrypto — amber theme, 5 symbols, 5 engines (MOM/MR/OFI/PERP/COMP)
// Polls /api/state at 1Hz. Trade log stored in localStorage.

const STORAGE_KEY = 'cryptobot_trades_v1';
const BOOT_TS = Date.now();

// 5 symbols matching main.cpp registration order
const SYMBOLS = [
  { short: 'BTC', full: 'BTCUSDT' },
  { short: 'ETH', full: 'ETHUSDT' },
  { short: 'SOL', full: 'SOLUSDT' },
  { short: 'BNB', full: 'BNBUSDT' },
  { short: 'XRP', full: 'XRPUSDT' },
];

// 5 engines matching SignalType enum in models.hpp
const ENGINES = ['momentum', 'mean_reversion', 'order_flow', 'perp_basis', 'composite'];
const ENGINE_LABELS = { momentum:'MOM', mean_reversion:'MR', order_flow:'OFI', perp_basis:'PERP', composite:'COMP' };

// Account size from config (shadow initial balance)
const ACCOUNT_SIZE = 100000;

let localTrades = [];
let audioCtx = null;
let audioUnlocked = false;
let lastPrices = {};
SYMBOLS.forEach(s => lastPrices[s.short.toLowerCase()] = 0);
let wins = 0, losses = 0;
let uptimeStart = null;
let lastKnownUptimeHours = null;
let firstPoll = true;

// ── AUDIO ──────────────────────────────────────────────────────────────────
function unlockAudio() {
  if (audioUnlocked) return;
  try {
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const buf = audioCtx.createBuffer(1, 1, 22050);
    const src = audioCtx.createBufferSource();
    src.buffer = buf; src.connect(audioCtx.destination); src.start(0);
    audioUnlocked = true;
    const btn = document.getElementById('audio-unlock');
    if (btn) { btn.textContent = '🔔 MUTED OFF'; btn.style.background = 'rgba(0,230,118,.1)'; btn.style.color = 'var(--green)'; btn.style.borderColor = 'var(--green)'; }
  } catch(e) {}
}

function playWin() {
  if (!audioUnlocked || !audioCtx) return;
  try {
    const now = audioCtx.currentTime;
    [[0, 880, 1040], [0.22, 1100, 1320]].forEach(([t, f1, f2]) => {
      [f1, f2].forEach((freq, i) => {
        const osc = audioCtx.createOscillator(), gain = audioCtx.createGain();
        osc.connect(gain); gain.connect(audioCtx.destination);
        osc.type = 'sine'; osc.frequency.setValueAtTime(freq, now + t);
        gain.gain.setValueAtTime(0, now + t);
        gain.gain.linearRampToValueAtTime(i === 0 ? 1.8 : 0.9, now + t + 0.008);
        gain.gain.exponentialRampToValueAtTime(0.3, now + t + 0.1);
        gain.gain.exponentialRampToValueAtTime(0.001, now + t + 1.4);
        osc.start(now + t); osc.stop(now + t + 1.5);
      });
    });
  } catch(e) {}
}

function playLoss() {
  if (!audioUnlocked || !audioCtx) return;
  try {
    const now = audioCtx.currentTime;
    const osc = audioCtx.createOscillator(), gain = audioCtx.createGain();
    osc.connect(gain); gain.connect(audioCtx.destination);
    osc.type = 'sawtooth'; osc.frequency.setValueAtTime(220, now);
    osc.frequency.linearRampToValueAtTime(110, now + 0.2);
    gain.gain.setValueAtTime(0.2, now); gain.gain.exponentialRampToValueAtTime(0.001, now + 0.25);
    osc.start(now); osc.stop(now + 0.3);
  } catch(e) {}
}

function flashWin(sym, pnl) {
  const el = document.getElementById('win-flash');
  if (!el) return;
  el.textContent = `⚡ ${sym}  +${(+pnl).toFixed(2)}bp`;
  el.classList.add('show');
  setTimeout(() => el.classList.remove('show'), 2200);
}

// ── STORAGE ────────────────────────────────────────────────────────────────
function loadTrades() {
  try { const raw = localStorage.getItem(STORAGE_KEY); localTrades = raw ? JSON.parse(raw) : []; }
  catch(e) { localTrades = []; }
}
function saveTrades() {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(localTrades.slice(0, 200))); } catch(e) {}
}
window.clearTrades = function() {
  localTrades = []; wins = 0; losses = 0;
  localStorage.removeItem(STORAGE_KEY);
  renderTradeLog(); updateWinRate();
};

function currentSessionTrades() {
  const all = [];
  for (const t of localTrades) {
    if (t.s === 'SESSION' || t.e === 'START') break;
    all.push(t);
  }
  return all;
}

// ── TRADE MERGE ────────────────────────────────────────────────────────────
function mergeTrades(serverLog, isBootLoad) {
  if (!serverLog || !serverLog.length) return;
  const before = localTrades.length;
  const existing = new Set(localTrades.map(t => `${t.t}|${t.s}|${t.e}|${t.p}`));
  let newCount = 0;
  serverLog.forEach(tr => {
    const key = `${tr.t}|${tr.s}|${tr.e}|${tr.p}`;
    if (!existing.has(key)) {
      localTrades.unshift(tr); existing.add(key); newCount++;
      if (!isBootLoad) {
        const tsStr = tr.t
          ? (tr.t.length < 12
              ? new Date().toISOString().slice(0,10) + 'T' + tr.t + 'Z'
              : (tr.t.endsWith('Z') || tr.t.includes('+') ? tr.t : tr.t + 'Z'))
          : null;
        const tradeAge = tsStr ? (Date.now() - new Date(tsStr).getTime()) : 99999;
        const isFresh = tradeAge < 60000;
        if (+tr.p > 0) { wins++; if (isFresh) { playWin(); flashWin(tr.s, tr.p); } }
        else if (+tr.p < 0) { losses++; if (isFresh) playLoss(); }
        else { wins++; }
      } else {
        if (+tr.p > 0) wins++; else if (+tr.p < 0) losses++;
      }
    }
  });
  if (newCount > 0 || before === 0) {
    localTrades = localTrades.slice(0, 200);
    saveTrades(); renderTradeLog(); renderSymbolTrades(); updateWinRate();
  }
}

// ── HELPERS ────────────────────────────────────────────────────────────────
const $ = id => document.getElementById(id);
const set = (id, val) => { const el = $(id); if (el) el.textContent = val; };
const fmtPnl = v => (v >= 0 ? '+' : '') + (+v).toFixed(2) + 'bp';
const bpToUsd = bp => (bp / 10000) * ACCOUNT_SIZE;
const fmtUsd = v => (v >= 0 ? '+$' : '-$') + Math.abs(v).toFixed(2);

function fmtHold(ms) {
  if (!ms || ms <= 0) return '--';
  if (ms < 1000) return ms + 'ms';
  if (ms < 60000) return (ms / 1000).toFixed(1) + 's';
  return Math.floor(ms / 60000) + 'm' + Math.floor((ms % 60000) / 1000) + 's';
}

function fmtPrice(p, sym) {
  if (!p || p <= 0) return '--';
  const s = (sym || '').toUpperCase();
  if (s === 'XRP' || s === 'SOL') return '$' + (+p).toFixed(3);
  if (s === 'BNB') return '$' + (+p).toFixed(2);
  return '$' + (+p).toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
}

function reasonClass(r) {
  if (!r) return 'timeout';
  const rl = r.toLowerCase();
  if (rl === 'tp') return 'tp';
  if (rl === 'sl' || rl === 'sc' || rl === 'stop_hit') return 'sl';
  if (rl === 'sig' || rl === 'signal' || rl === 'signal_exit') return 'trail';
  if (rl.startsWith('trail')) return 'trail';
  return 'timeout';
}

function normalizeReason(r) {
  if (!r) return 'TO';
  const ru = r.toUpperCase();
  if (ru === 'SC' || ru === 'STOP_HIT') return 'SL';
  if (ru === 'SIGNAL' || ru === 'SIGNAL_EXIT') return 'SIG';
  if (ru.startsWith('TRAIL')) return 'TRAIL';
  if (ru === 'TIMEOUT') return 'TO';
  return ru;
}

function signalTypeClass(type) {
  if (!type) return 'r-none';
  const t = type.toLowerCase().replace(/[\s-]/g, '_');
  return 'r-' + t;
}

function signalTypeLabel(type) {
  if (!type) return '--';
  const map = { momentum:'MOM', mean_reversion:'MR', order_flow:'OFI', perp_basis:'PERP', composite:'COMP', none:'--' };
  return map[type.toLowerCase()] || type.toUpperCase();
}

// ── RENDER: SYMBOL TRADES ─────────────────────────────────────────────────
function renderSymbolTrades() {
  SYMBOLS.forEach(({ short }) => {
    const el = $('str-' + short.toLowerCase());
    if (!el) return;
    const trades = currentSessionTrades().filter(t => (t.s || '').replace('USDT','').toUpperCase() === short).slice(0, 8);
    if (!trades.length) { el.className = 'trades-empty'; el.innerHTML = 'No trades yet'; return; }
    el.className = '';
    el.innerHTML = trades.map(tr => {
      const pnl = +tr.p || 0, isWin = pnl >= 0, usd = bpToUsd(pnl);
      const rc = reasonClass(tr.why || tr.reason || '');
      const why = normalizeReason(tr.why || tr.reason || '?');
      const time = tr.t ? (tr.t.length > 8 ? tr.t.substring(11, 19) : tr.t) : '--';
      const en = tr.en ? fmtPrice(tr.en, short) : '--';
      const ex = tr.ex ? fmtPrice(tr.ex, short) : '--';
      return `<div class="trade-row ${isWin?'win':'loss'}">
        <span class="tr-tag ${isWin?'win':'loss'}">${isWin?'WIN':'LOSS'}</span>
        <span class="tr-pnl ${isWin?'pos':'neg'}">${fmtPnl(pnl)}</span>
        <span class="tr-usd ${isWin?'pos':'neg'}">${fmtUsd(usd)}</span>
        <span class="tr-eng">${(tr.e||'--').toUpperCase()}</span>
        <span class="tr-val">${en}→${ex}</span>
        <span class="tr-val">${fmtHold(tr.hold)}</span>
        <span class="tr-badge ${rc}">${why}</span>
        <span class="tr-time">${time}</span>
      </div>`;
    }).join('');
  });
}

// ── RENDER: WIN RATE ──────────────────────────────────────────────────────
function updateWinRate() {
  const t = wins + losses;
  const wr = t > 0 ? (wins / t * 100).toFixed(0) + '%' : '--%';
  const wrEl = $('ts-wr');
  if (wrEl) { wrEl.textContent = wr; wrEl.className = 'tl-stat-val ' + (t > 0 ? (wins >= losses ? 'pos' : 'neg') : ''); }
}

// ── RENDER: UPTIME ────────────────────────────────────────────────────────
function updateUptime() {
  if (!uptimeStart) return;
  const s = Math.floor((Date.now() - uptimeStart) / 1000);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  set('tb-uptime', `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`);
}

// ── RENDER: TRADE LOG + BOTTOM FEED ───────────────────────────────────────
function renderTradeLog() {
  let totalPnl = 0, winPnl = 0, lossPnl = 0, winCount = 0, lossCount = 0;
  let tpCount = 0, slCount = 0;
  currentSessionTrades().forEach(t => {
    const p = +t.p || 0; totalPnl += p;
    if (p >= 0) { winPnl += p; winCount++; } else { lossPnl += p; lossCount++; }
    const why = (t.why || t.reason || '').toUpperCase();
    if (why === 'TP') tpCount++;
    else if (why === 'SL' || why === 'SC' || why === 'STOP_HIT') slCount++;
  });
  const avgWin  = winCount  > 0 ? winPnl  / winCount  : null;
  const avgLoss = lossCount > 0 ? lossPnl / lossCount : null;
  const total   = winCount + lossCount;
  const wr      = total > 0 ? winCount / total : null;
  const exp     = (wr !== null && avgWin !== null && avgLoss !== null) ? (wr * avgWin + (1 - wr) * avgLoss) : null;

  set('ts-count', total); set('ts-wins', winCount); set('ts-losses', lossCount);
  set('st-tp', tpCount); set('st-sl', slCount);
  set('tp-wins', winCount); set('tp-losses', lossCount);
  set('tp-tp', tpCount); set('tp-sl', slCount);

  const fmtWr = () => wr !== null ? (wr*100).toFixed(0)+'%' : '--%';
  const wrCls = () => wr !== null ? (wr >= 0.5 ? 'pos' : 'neg') : '';

  const pnlEl = $('ts-pnl'); if (pnlEl) { pnlEl.textContent = fmtPnl(totalPnl); pnlEl.className = 'tl-stat-val ' + (totalPnl >= 0 ? 'pos' : 'neg'); }
  const usdEl = $('ts-usd'); if (usdEl) { usdEl.textContent = fmtUsd(bpToUsd(totalPnl)); usdEl.className = 'tl-stat-val ' + (totalPnl >= 0 ? 'pos' : 'neg'); }
  const tswr = $('ts-wr'); if (tswr) { tswr.textContent = fmtWr(); tswr.className = 'tl-stat-val ' + wrCls(); }
  const awEl = $('ts-avgwin'); if (awEl) awEl.textContent = avgWin !== null ? '+' + avgWin.toFixed(2) + 'bp' : '--';
  const alEl = $('ts-avgloss'); if (alEl) alEl.textContent = avgLoss !== null ? avgLoss.toFixed(2) + 'bp' : '--';
  const expEl = $('ts-exp'); if (expEl) { expEl.textContent = exp !== null ? (exp >= 0 ? '+' : '') + exp.toFixed(2) + 'bp' : '--'; expEl.className = 'tl-stat-val ' + (exp !== null ? (exp >= 0 ? 'pos' : 'neg') : ''); }

  // Mirror to trades-panel header
  const tpWrEl = $('tp-wr'); if (tpWrEl) { tpWrEl.textContent = fmtWr(); tpWrEl.className = 'tp-val ' + wrCls(); }
  const tpPnlEl = $('tp-pnl'); if (tpPnlEl) { tpPnlEl.textContent = fmtPnl(totalPnl); tpPnlEl.className = 'tp-val ' + (totalPnl >= 0 ? 'pos' : 'neg'); }
  const tpUsdEl = $('tp-usd'); if (tpUsdEl) { tpUsdEl.textContent = fmtUsd(bpToUsd(totalPnl)); tpUsdEl.className = 'tp-val ' + (totalPnl >= 0 ? 'pos' : 'neg'); }
  const tpAwEl = $('tp-avgwin'); if (tpAwEl) tpAwEl.textContent = avgWin !== null ? '+' + avgWin.toFixed(2) + 'bp' : '--';
  const tpAlEl = $('tp-avgloss'); if (tpAlEl) tpAlEl.textContent = avgLoss !== null ? avgLoss.toFixed(2) + 'bp' : '--';
  const tpExpEl = $('tp-exp'); if (tpExpEl) { tpExpEl.textContent = exp !== null ? (exp >= 0 ? '+' : '') + exp.toFixed(2) + 'bp' : '--'; tpExpEl.className = 'tp-val ' + (exp !== null ? (exp >= 0 ? 'pos' : 'neg') : ''); }

  // Recent trades list
  const rtList = $('recent-trades-list');
  if (rtList) {
    const trades = currentSessionTrades();
    if (!trades.length) {
      rtList.innerHTML = '<div style="padding:10px 12px;color:var(--muted);font-size:11px;font-style:italic">Waiting for first trade...</div>';
    } else {
      rtList.innerHTML = trades.slice(0, 10).map(tr => {
        const p = +tr.p || 0, isWin = p >= 0;
        const sym = (tr.s || '').replace('USDT','');
        const eng = ENGINE_LABELS[(tr.e||'').toLowerCase()] || (tr.e||'?').toUpperCase();
        const why = normalizeReason(tr.why || tr.reason || '?');
        const whyCls = why==='TP'?'why-tp':why==='SL'?'why-sl':why==='TRAIL'?'why-trail':'';
        const time = tr.t ? (tr.t.length > 10 ? tr.t.substring(11,16) : tr.t) : '--';
        return `<div class="rt-row ${isWin ? 'rt-win' : 'rt-loss'}">
          <span class="rt-time">${time}</span>
          <span class="rt-sym">${sym}</span>
          <span class="rt-eng">${eng}</span>
          <span class="rt-why ${whyCls}">${why}</span>
          <span class="rt-pnl ${isWin ? 'pos' : 'neg'}">${isWin?'+':''}${p.toFixed(2)}bp</span>
          <span class="rt-hold">${fmtHold(tr.hold)}</span>
        </div>`;
      }).join('');
    }
  }

  // Bottom feed rows
  const btmRows = $('btm-trade-rows');
  if (btmRows) {
    const trades = currentSessionTrades();
    if (!trades.length) {
      btmRows.innerHTML = '<div style="padding:5px 12px;color:var(--muted);font-size:11px;font-style:italic">No trades yet this session</div>';
    } else {
      btmRows.innerHTML = trades.slice(0, 10).map(tr => {
        const p = +tr.p || 0, isWin = p >= 0;
        const sym = (tr.s || '').replace('USDT','');
        const eng = ENGINE_LABELS[(tr.e||'').toLowerCase()] || (tr.e||'?').toUpperCase();
        const why = normalizeReason(tr.why || tr.reason || '?');
        const whyCls = why==='TP'?'why-tp':why==='SL'?'why-sl':why==='TRAIL'?'why-trail':'why-to';
        const time = tr.t ? (tr.t.length > 10 ? tr.t.substring(11,16) : tr.t) : '--';
        const mfe = tr.mfe != null ? '+' + (+tr.mfe).toFixed(1) + 'bp' : '--';
        const mae = tr.mae != null ? (+tr.mae).toFixed(1) + 'bp' : '--';
        const usd = bpToUsd(p);
        return `<div class="btm-trade-row ${isWin?'btr-win':'btr-loss'}">
          <span class="btr-time">${time}</span>
          <span class="btr-sym">${sym}</span>
          <span class="btr-eng">${eng}</span>
          <span class="btr-why ${whyCls}">${why}</span>
          <span class="btr-pnl ${isWin?'pos':'neg'}">${isWin?'+':''}${p.toFixed(2)}bp</span>
          <span class="btr-usd ${isWin?'pos':'neg'}">${fmtUsd(usd)}</span>
          <span class="btr-mfe">${mfe}</span>
          <span class="btr-mae">${mae}</span>
          <span class="btr-hold">${fmtHold(tr.hold)}</span>
        </div>`;
      }).join('');
    }
  }

  // Engine summary table
  renderEngineSummary();
}

// ── RENDER: ENGINE SUMMARY TABLE ──────────────────────────────────────────
function renderEngineSummary() {
  const tbody = $('eng-summary-body');
  if (!tbody) return;
  const trades = currentSessionTrades();
  if (!trades.length) {
    tbody.innerHTML = '<tr><td colspan="9" style="color:var(--muted);font-size:11px;font-style:italic;text-align:center;padding:8px">No trades yet</td></tr>';
    return;
  }

  // Aggregate by engine
  const stats = {};
  ENGINES.forEach(e => stats[e] = { wins:0, losses:0, winPnl:0, lossPnl:0 });
  stats['other'] = { wins:0, losses:0, winPnl:0, lossPnl:0 };

  trades.forEach(t => {
    const eng = (t.e || '').toLowerCase();
    const bucket = stats[eng] ? eng : 'other';
    const p = +t.p || 0;
    if (p >= 0) { stats[bucket].wins++; stats[bucket].winPnl += p; }
    else         { stats[bucket].losses++; stats[bucket].lossPnl += p; }
  });

  let totalW = 0, totalL = 0, totalPnl = 0;
  const rows = [...ENGINES, 'other'].map(eng => {
    const s = stats[eng];
    const t = s.wins + s.losses;
    if (t === 0) return null;
    totalW += s.wins; totalL += s.losses; totalPnl += s.winPnl + s.lossPnl;
    const wr = t > 0 ? (s.wins / t * 100).toFixed(0) + '%' : '--%';
    const wrCls = s.wins >= s.losses ? 'etd-wr pos' : 'etd-wr neg';
    const pnl = s.winPnl + s.lossPnl;
    const pnlCls = pnl >= 0 ? 'etd-pnl pos' : 'etd-pnl neg';
    const avgW = s.wins > 0 ? '+' + (s.winPnl / s.wins).toFixed(2) + 'bp' : '--';
    const avgL = s.losses > 0 ? (s.lossPnl / s.losses).toFixed(2) + 'bp' : '--';
    const expV = t > 0 ? ((s.wins/t) * (s.winPnl/Math.max(1,s.wins)) + (s.losses/t) * (s.lossPnl/Math.max(1,s.losses))) : null;
    const expStr = expV !== null ? (expV >= 0 ? '+' : '') + expV.toFixed(2) + 'bp' : '--';
    const label = ENGINE_LABELS[eng] || eng.toUpperCase();
    return `<tr>
      <td>${label}</td>
      <td>${t}</td><td style="color:var(--green)">${s.wins}</td><td style="color:var(--red)">${s.losses}</td>
      <td class="${wrCls}">${wr}</td>
      <td class="${pnlCls}">${fmtPnl(pnl)}</td>
      <td style="color:rgba(0,230,118,.8)">${avgW}</td>
      <td style="color:rgba(255,61,87,.8)">${avgL}</td>
      <td class="${expV !== null && expV >= 0 ? 'etd-pnl pos' : 'etd-pnl neg'}">${expStr}</td>
    </tr>`;
  }).filter(Boolean);

  const tTotal = totalW + totalL;
  const tWr = tTotal > 0 ? (totalW / tTotal * 100).toFixed(0) + '%' : '--%';
  const tPnlCls = totalPnl >= 0 ? 'etd-pnl pos' : 'etd-pnl neg';
  rows.push(`<tr class="total-row">
    <td>TOTAL</td>
    <td>${tTotal}</td><td style="color:var(--green)">${totalW}</td><td style="color:var(--red)">${totalL}</td>
    <td class="${totalW >= totalL ? 'etd-wr pos' : 'etd-wr neg'}">${tWr}</td>
    <td class="${tPnlCls}">${fmtPnl(totalPnl)}</td>
    <td></td><td></td><td></td>
  </tr>`);

  tbody.innerHTML = rows.join('');
}

// ── MAIN UPDATE ───────────────────────────────────────────────────────────
function updateAll(data) {
  if (!data) return;
  if (!uptimeStart) uptimeStart = Date.now();

  // Restart detection
  const serverUptime = data.uptime_hours || 0;
  if (lastKnownUptimeHours !== null && serverUptime < lastKnownUptimeHours - 0.01) {
    localTrades = [];
    try { localStorage.removeItem(STORAGE_KEY); } catch(e) {}
    uptimeStart = Date.now();
    firstPoll = true;
  }
  lastKnownUptimeHours = serverUptime;

  // Portfolio / topbar
  const port = data.portfolio || {};
  const equity = port.equity_usd || 0;
  const dailyPnl = port.daily_pnl_usd || 0;
  const openCount = port.open_trade_count || 0;

  const equEl = $('tb-equity');
  if (equEl) equEl.textContent = '$' + equity.toLocaleString('en-US', {minimumFractionDigits:2, maximumFractionDigits:2});
  const pnlEl = $('tb-pnl');
  if (pnlEl) { const bps = equity > 0 ? (dailyPnl / equity * 10000) : 0; pnlEl.textContent = fmtPnl(bps); pnlEl.className = 'tb-val ' + (bps >= 0 ? 'pos' : 'neg'); }

  set('tb-trades', data.total_trades || 0);
  set('tb-positions', openCount);

  // Fee gate pass rate
  const fgRate = data.fee_gate_pass_rate;
  if (fgRate != null) {
    const fgEl = $('tb-feegate');
    if (fgEl) { fgEl.textContent = (fgRate * 100).toFixed(1) + '%'; fgEl.className = 'tb-val accent'; }
    const rcFg = $('rc-feegate');
    if (rcFg) { rcFg.textContent = (fgRate * 100).toFixed(1) + '%'; rcFg.className = 'rc-val ' + (fgRate > 0.5 ? 'accent' : 'neg'); }
  }

  // Risk panel
  const blocked_fee  = data.signals_blocked_fee  || 0;
  const blocked_risk = data.signals_blocked_risk || 0;
  set('rc-blocked', blocked_fee + blocked_risk);
  set('rc-blocked-sub', `fee: ${blocked_fee} | risk: ${blocked_risk}`);

  const rcDailyEl = $('rc-daily-pnl');
  if (rcDailyEl) { rcDailyEl.textContent = fmtUsd(dailyPnl); rcDailyEl.className = 'rc-val ' + (dailyPnl >= 0 ? 'pos' : 'neg'); }
  const dd = port.drawdown_pct || 0;
  set('rc-daily-dd', 'DD: ' + dd.toFixed(2) + '%');

  const isShadow = data.is_shadow !== false;
  const modeEl = $('rc-mode');
  if (modeEl) { modeEl.textContent = isShadow ? 'SHADOW' : 'LIVE'; modeEl.className = 'rc-val ' + (isShadow ? 'accent' : 'pos'); }
  const modeBadge = $('mode-badge');
  if (modeBadge) { modeBadge.textContent = isShadow ? 'SHADOW' : 'LIVE'; modeBadge.className = 'badge ' + (isShadow ? 'badge-shadow' : 'badge-live'); }

  const gwLat = data.gateway_avg_lat_us;
  if (gwLat != null) set('rc-gw-lat', 'gw: ' + Math.round(gwLat) + 'µs');

  // Price update helper
  const updatePrice = (id, val, prev, sym) => {
    const el = $(id); if (!el) return;
    el.textContent = fmtPrice(val, sym);
    el.className = 'sym-px' + (val > prev ? ' up' : val < prev ? ' down' : '');
  };

  // Per-symbol data
  const symbols_data = data.symbols || {};
  SYMBOLS.forEach(({ short, full }) => {
    const sym = short.toLowerCase();
    const d = symbols_data[full] || symbols_data[short] || symbols_data[sym] || {};

    // Price
    const px = d.price || data[full.toLowerCase() + '_price'] || data[short.toLowerCase() + '_price'] || 0;
    updatePrice('px-' + sym, px, lastPrices[sym], short);
    lastPrices[sym] = px;

    // Regime / signal type
    const sigType = (d.last_signal_type || d.regime || 'none').toLowerCase();
    const regEl = $('reg-' + sym);
    if (regEl) { regEl.textContent = signalTypeLabel(sigType); regEl.className = 'sym-regime ' + signalTypeClass(sigType); }

    // Meta: spread, OFI, position
    if (d.spread_bps != null) set('spread-' + sym, d.spread_bps.toFixed(1) + 'bp');
    if (d.ofi != null) { const ofiEl = $('ofi-' + sym); if (ofiEl) { ofiEl.textContent = d.ofi.toFixed(2); ofiEl.style.color = d.ofi > 0 ? 'var(--green)' : d.ofi < 0 ? 'var(--red)' : 'var(--muted)'; } }
    if (d.position_usd != null) set('pos-' + sym, '$' + Math.round(d.position_usd));

    // Mini pnl badge from local trades
    const symTrades = currentSessionTrades().filter(t => (t.s || '').replace('USDT','').toUpperCase() === short);
    const symPnl = symTrades.reduce((acc, t) => acc + (+t.p || 0), 0);
    const miniPnl = $('mini-pnl-' + sym);
    if (miniPnl) { miniPnl.textContent = fmtPnl(symPnl); miniPnl.className = 'sym-pnl-badge ' + (symPnl > 0 ? 'pos' : symPnl < 0 ? 'neg' : 'zero'); }
    set('mini-t-' + sym, symTrades.length ? symTrades.length + 'T' : '0T');

    // Engine cells
    ENGINES.forEach(eng => {
      const engData = d.engines ? d.engines[eng] : {};
      const active  = engData ? !!engData.active : false;
      const pnlBp   = engData ? (engData.total_pnl_bp || 0) : 0;
      const trades  = engData ? (engData.trades || 0) : 0;

      const badge = $(`eb-${sym}-${eng}`);
      if (badge) { badge.textContent = active ? 'ACTIVE' : 'OFF'; badge.className = 'eng-badge ' + (active ? 'eb-active' : 'eb-off'); }
      const cell = $(`ec-${sym}-${eng}`);
      if (cell) cell.className = 'eng-cell' + (active ? ' has-pos' : '');
      const pEl = $(`pnl-${sym}-${eng}`);
      if (pEl) { pEl.textContent = fmtPnl(pnlBp); pEl.className = 'eng-pnl ' + (pnlBp > 0 ? 'pos' : pnlBp < 0 ? 'neg' : 'zero'); }
      set(`trades-${sym}-${eng}`, trades ? trades + 'T' : '0T');

      // Auto-expand if active
      if (active) {
        const block = $('sb-' + sym);
        if (block && !block.classList.contains('expanded')) block.classList.add('expanded');
      }
    });
  });

  // Trade log merge
  if (data.trade_log) mergeTrades(data.trade_log, firstPoll);
  firstPoll = false;
}

// ── POLL LOOP ─────────────────────────────────────────────────────────────
let connected = false;
let pollErrors = 0;
let lastPollOk = null;

function fmtAgo(ts) {
  if (!ts) return 'never';
  const s = Math.floor((Date.now() - ts) / 1000);
  if (s < 60) return s + 's ago';
  return Math.floor(s/60) + 'm' + (s%60) + 's ago';
}

function setPollOk() {
  pollErrors = 0; lastPollOk = Date.now();
  const cb = $('conn-badge'); if (cb) { cb.textContent = 'LIVE'; cb.className = 'badge badge-conn'; }
  const dot = $('live-dot'); if (dot) dot.className = 'dot live';
  const banner = $('poll-error'); if (banner) banner.classList.remove('show');
}

function setPollError(reason) {
  pollErrors++; connected = false;
  const cb = $('conn-badge'); if (cb) { cb.textContent = 'OFFLINE'; cb.className = 'badge badge-disc'; }
  const dot = $('live-dot'); if (dot) dot.className = 'dot';
  const banner = $('poll-error'); if (banner) banner.classList.add('show');
  const msg = $('poll-error-msg'); if (msg) msg.textContent = reason;
  const cnt = $('poll-error-count'); if (cnt) cnt.textContent = 'ERR' + pollErrors;
  const etime = $('poll-error-time'); if (etime) etime.textContent = 'last ok: ' + fmtAgo(lastPollOk);
}

async function poll() {
  let res;
  try {
    res = await fetch('/api/state', { cache: 'no-store', signal: AbortSignal.timeout(4000) });
  } catch(e) {
    const reason = e.name === 'TimeoutError' ? 'Fetch timeout (>4s) — backend hung?' :
                   e.name === 'TypeError'    ? 'Network error — backend down or unreachable' :
                   'Fetch failed: ' + e.message;
    setPollError(reason);
    return;
  }
  if (!res.ok) { setPollError('HTTP ' + res.status + ' ' + res.statusText); return; }
  let data;
  try { data = await res.json(); }
  catch(e) { setPollError('JSON parse error — backend may have crashed'); return; }
  connected = true;
  setPollOk();
  updateAll(data);
}

// Keep error age updated
setInterval(() => {
  if (pollErrors > 0) {
    const etime = $('poll-error-time'); if (etime) etime.textContent = 'last ok: ' + fmtAgo(lastPollOk);
  }
}, 1000);

// ── INIT ──────────────────────────────────────────────────────────────────
loadTrades(); renderTradeLog(); updateWinRate();
wins = 0; losses = 0;
currentSessionTrades().forEach(t => { if (+t.p > 0) wins++; else if (+t.p < 0) losses++; });
updateWinRate();
poll();
setInterval(poll, 1000);
setInterval(updateUptime, 1000);

// ── COLLAPSIBLE SYM ROWS ─────────────────────────────────────────────────
function toggleSym(sl, event) {
  if (event) event.stopPropagation();
  const block = document.getElementById('sb-' + sl);
  if (!block) return;
  block.classList.toggle('expanded');
}
