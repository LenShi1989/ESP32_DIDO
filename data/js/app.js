/* ESP32 DIDO - 前端控制 */
(function () {
  'use strict';

  const $  = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const DAY = ['日', '一', '二', '三', '四', '五', '六'];

  // ------------------------------------------------ 共用工具

  let toastTimer = null;
  function toast(msg, kind) {
    const t = $('#toast');
    t.textContent = msg;
    t.className = 'toast show ' + (kind || '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (t.className = 'toast'), 3000);
  }

  function setOnline(ok) {
    $('#connDot').classList.toggle('on', !!ok);
  }

  async function get(url) {
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    setOnline(true);
    return r.json();
  }

  async function post(url, obj) {
    const r = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams(obj || {}).toString()
    });
    let data = {};
    try { data = await r.json(); } catch (e) { /* 無 JSON 回應 */ }
    if (!r.ok || data.ok === false) throw new Error(data.msg || ('HTTP ' + r.status));
    return data;
  }

  // 表單 → 物件 (checkbox 一律轉成 1/0，後端才收得到未勾選狀態)
  function formToObj(form) {
    const o = {};
    $$('input,select,textarea', form).forEach(el => {
      if (!el.name) return;
      o[el.name] = el.type === 'checkbox' ? (el.checked ? 1 : 0) : el.value;
    });
    return o;
  }

  function bytes(n) {
    if (n === undefined || n === null) return '--';
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1048576).toFixed(2) + ' MB';
  }

  function uptime(sec) {
    const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600);
    const m = Math.floor(sec % 3600 / 60), s = sec % 60;
    return (d ? d + ' 天 ' : '') + String(h).padStart(2, '0') + ':' +
           String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
  }

  function kvRows(tbl, rows) {
    tbl.querySelector('tbody').innerHTML =
      rows.map(r => `<tr><td>${r[0]}</td><td>${r[1]}</td></tr>`).join('');
  }

  const esc = s => String(s === undefined || s === null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');

  // ------------------------------------------------ 主題

  // 三種狀態：null = 跟隨系統、'light'、'dark'
  function currentTheme() {
    try { return localStorage.getItem('theme'); } catch (e) { return null; }
  }

  function systemIsLight() {
    return window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches;
  }

  function applyTheme(t) {
    const root = document.documentElement;
    if (t === 'light' || t === 'dark') root.dataset.theme = t;
    else delete root.dataset.theme;

    const effLight = t ? t === 'light' : systemIsLight();
    const btn = $('#themeBtn');
    btn.textContent = t ? (effLight ? '☀️' : '🌙') : '🌓';
    btn.title = t ? (effLight ? '亮色（點擊切換）' : '暗色（點擊切換）')
                  : '跟隨系統（點擊切換）';
  }

  // 依序循環：跟隨系統 → 亮色 → 暗色 → 跟隨系統
  $('#themeBtn').onclick = () => {
    const next = { null: 'light', light: 'dark', dark: null }[String(currentTheme())];
    try {
      if (next) localStorage.setItem('theme', next);
      else localStorage.removeItem('theme');
    } catch (e) { /* 寫不進去就只套用這次 */ }
    applyTheme(next);
    toast(next === 'light' ? '已切換為亮色' : next === 'dark' ? '已切換為暗色' : '已改為跟隨系統');
  };

  // 跟隨系統時，系統主題變了要即時反映
  if (window.matchMedia) {
    const mq = window.matchMedia('(prefers-color-scheme: light)');
    const onChange = () => { if (!currentTheme()) applyTheme(null); };
    if (mq.addEventListener) mq.addEventListener('change', onChange);
    else if (mq.addListener) mq.addListener(onChange);
  }

  applyTheme(currentTheme());

  // ------------------------------------------------ 路由

  const PAGES = ['status', 'wifi', 'di', 'do', 'mqtt', 'modbus', 'ota', 'user'];
  let current = 'status';
  const loaders = {};

  function show(page) {
    if (PAGES.indexOf(page) < 0) page = 'status';
    current = page;
    PAGES.forEach(p => $('#page-' + p).classList.toggle('hidden', p !== page));
    $$('.nav').forEach(a => a.classList.toggle('active', a.dataset.page === page));
    $('#sidebar').classList.remove('open');
    if (loaders[page]) loaders[page]();
  }

  window.addEventListener('hashchange', () => show(location.hash.slice(1)));
  $('#menuBtn').onclick = () => $('#sidebar').classList.toggle('open');

  // ------------------------------------------------ 系統狀態

  async function loadStatus() {
    try {
      const [w, s, fs] = await Promise.all([
        get('/api/wifi'), get('/api/status'), get('/api/fs')
      ]);

      kvRows($('#wifiInfo'), [
        ['狀態', w.connected ? '已連線' : (w.applying ? '連線中...' : '未連線')],
        ['SSID', esc(w.ssid) || '--'],
        ['DHCP IP', w.connected ? '<strong>' + esc(w.ip) + '</strong>' : '--'],
        ['閘道', esc(w.gw) || '--'],
        ['子網路遮罩', esc(w.mask) || '--'],
        ['DNS', esc(w.dns) || '--'],
        ['訊號強度', w.connected ? w.rssi + ' dBm' : '--'],
        ['頻道', w.ch],
        ['STA MAC', esc(w.mac)],
        ['主機名稱', esc(w.host)],
        ['AP（常開）', esc(w.apSsid) + '　<strong>' + esc(w.apIp) + '</strong>'],
        ['AP 連線裝置', (w.apClients || 0) + ' 台']
      ]);

      const pct = fs.total ? (fs.used / fs.total * 100) : 0;
      $('#fsBar').style.width = pct.toFixed(1) + '%';
      $('#fsUsage').textContent =
        `已用 ${bytes(fs.used)} / 總容量 ${bytes(fs.total)}　(剩餘 ${bytes(fs.free)}，${pct.toFixed(1)}%)`;

      $('#fsTable tbody').innerHTML = (fs.files || [])
        .sort((a, b) => a.name.localeCompare(b.name))
        .map(f => `<tr><td>${esc(f.name)}</td><td class="right">${bytes(f.size)}</td>
          <td class="right"><button class="btn btn-danger" data-del="${esc(f.name)}">刪除</button></td></tr>`)
        .join('') || '<tr><td colspan="3" class="muted">SPIFFS 內沒有檔案</td></tr>';

      kvRows($('#sysInfo'), [
        ['韌體版本', esc(s.fw) + '　<span class="muted">build ' + esc(s.build) + '</span>'],
        ['晶片', esc(s.chip) + ' / ' + s.cores + ' 核 / ' + s.cpuMhz + ' MHz'],
        ['SDK 版本', esc(s.sdkVer)],
        ['運行時間', uptime(s.uptime)],
        ['目前時間', esc(s.time)],
        ['可用記憶體', bytes(s.heap) + '　(最低 ' + bytes(s.minHeap) + ')'],
        ['Flash 容量', bytes(s.flash)],
        ['韌體大小', bytes(s.sketch) + '　(可用 ' + bytes(s.sketchFree) + ')']
      ]);
    } catch (e) {
      setOnline(false);
    }
  }
  let tftLoaded = false;
  loaders.status = () => {
    loadStatus();
    if (!tftLoaded) { tftLoaded = true; loadTft(); }
  };

  $('#fsTable').addEventListener('click', async e => {
    const path = e.target.dataset.del;
    if (!path) return;
    if (!confirm('確定刪除 ' + path + ' ？')) return;
    try {
      const r = await post('/api/fs/delete', { path });
      toast(r.msg, 'ok');
      loadStatus();
    } catch (err) { toast(err.message, 'err'); }
  });

  async function loadTft() {
    try {
      const d = await get('/api/display');
      const f = $('#tftForm');
      f.invert.checked = !!d.invert;
      f.bgr.checked = !!d.bgr;
      f.rotation.value = d.rotation;
      f.mhz.value = d.mhz;
      f.bl.value = d.bl;
      f.qrSec.value = d.qrSec;
      f.page.value = d.page;
      f.pageSec.value = d.pageSec;
      syncPageSec();
    } catch (e) { /* 顯示設定讀不到不影響其他頁面 */ }
  }

  $('#tftForm').onsubmit = async e => {
    e.preventDefault();
    try {
      const r = await post('/api/display', formToObj(e.target));
      toast(r.msg, 'ok');
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#tftTestBtn').onclick = async () => {
    try {
      const r = await post('/api/display/test');
      toast(r.msg, 'ok');
    } catch (e) { toast(e.message, 'err'); }
  };

  $('#tftSplashBtn').onclick = async () => {
    try {
      const r = await post('/api/display/splash');
      toast(r.msg, 'ok');
    } catch (e) { toast(e.message, 'err'); }
  };

  function syncPageSec() {
    $('#pageSecRow').style.display = $('#tftPage').value === '2' ? '' : 'none';
  }
  $('#tftPage').onchange = syncPageSec;

  $('#tftQrBtn').onclick = async () => {
    try { toast((await post('/api/display/qr', { sec: 60 })).msg, 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  };

  $('#rebootBtn').onclick = async () => {
    if (!confirm('確定重新啟動裝置？')) return;
    try { await post('/api/reboot'); toast('裝置重新啟動中...', 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  };

  // ------------------------------------------------ WiFi

  let scanTimer = null;

  function renderWifiNow(w) {
    const b = $('#wifiBadge');
    b.textContent = w.connected ? '已連線' : (w.applying ? '連線中...' : '未連線');
    b.className = 'badge ' + (w.connected ? 'ok' : 'bad');

    kvRows($('#wifiNow'), [
      ['SSID', esc(w.ssid) || '--'],
      ['DHCP IP', w.connected ? '<strong>' + esc(w.ip) + '</strong>' : '--'],
      ['閘道', esc(w.gw) || '--'],
      ['訊號強度', w.connected ? w.rssi + ' dBm' : '--']
    ]);
    $('#wifiApNote').innerHTML =
      `AP <strong>${esc(w.apSsid)}</strong> 持續開啟（${esc(w.apIp)}），` +
      `目前 ${w.apClients || 0} 台裝置連線。連上 STA 後仍可由此位址進入設定。`;
  }

  async function loadWifi(keepInputs) {
    try {
      const w = await get('/api/wifi');
      renderWifiNow(w);
      if (!keepInputs) {
        if (!$('#wifiSsid').value) $('#wifiSsid').value = w.ssid || '';
        $('#wifiHost').value = w.host || '';
      }
      return w;
    } catch (e) { setOnline(false); return null; }
  }
  loaders.wifi = () => loadWifi(false);

  function rssiBars(r) {
    const n = r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : r >= -85 ? 1 : 0;
    return '▂▄▆█'.slice(0, n).padEnd(4, '·');
  }

  function renderScan(list) {
    const sorted = (list || []).slice().sort((a, b) => b.rssi - a.rssi);
    $('#scanTable tbody').innerHTML = sorted.length
      ? sorted.map(n => `<tr class="clickable" data-ssid="${esc(n.ssid)}">
          <td>${n.hidden ? '<i class="muted">(隱藏網路)</i>' : esc(n.ssid)}</td>
          <td><span class="bars">${rssiBars(n.rssi)}</span> ${n.rssi}</td>
          <td>${n.ch}</td><td>${esc(n.enc)}</td></tr>`).join('')
      : '<tr><td colspan="4" class="muted">沒有掃描到任何網路</td></tr>';
  }

  function scanBusy(busy, text) {
    const btn = $('#scanBtn');
    btn.disabled = busy;
    btn.textContent = text || (busy ? '掃描中...' : '掃描 SSID');
  }

  $('#scanBtn').onclick = async () => {
    scanBusy(true);
    $('#scanMsg').textContent = '掃描中，約需 5~10 秒...';
    try {
      await post('/api/wifi/scan');
    } catch (e) {
      toast(e.message, 'err');
      scanBusy(false);
      return;
    }
    clearInterval(scanTimer);
    scanTimer = setInterval(pollScan, 1000);
  };

  async function pollScan() {
    let d;
    try {
      d = await get('/api/wifi/scan');
    } catch (e) {
      return;                       // 掃描期間連線可能短暫中斷，繼續等
    }
    renderScan(d.list);
    if (d.scanning) {
      scanBusy(true, '掃描中 ' + Math.round(d.elapsed / 1000) + 's');
      return;
    }
    clearInterval(scanTimer);
    scanBusy(false);
    const diag = ` [rc=${d.startRc} mode=${d.mode} raw=${d.rawCount}]`;
    if (d.failed) {
      $('#scanMsg').textContent = '掃描失敗，請稍候再試一次。' + diag;
      toast('掃描失敗', 'err');
    } else {
      const n = (d.list || []).length;
      $('#scanMsg').textContent = n
        ? `找到 ${n} 個網路，點選即可填入 SSID。`
        : '沒有掃描到任何網路，請確認附近有 2.4GHz 網路（ESP32 不支援 5GHz）。' + diag;
    }
  }

  $('#scanTable').addEventListener('click', e => {
    const tr = e.target.closest('tr[data-ssid]');
    if (!tr) return;
    $('#wifiSsid').value = tr.dataset.ssid;
    $('#wifiSsid').scrollIntoView({ behavior: 'smooth', block: 'center' });
    toast('已填入 SSID：' + tr.dataset.ssid);
  });

  $('#wifiForm').onsubmit = async e => {
    e.preventDefault();
    const btn = e.target.querySelector('button[type=submit]');
    btn.disabled = true;
    $('#wifiApplyMsg').textContent = '連線中，請稍候...';
    try {
      const r = await post('/api/wifi', formToObj(e.target));
      toast(r.msg, 'ok');
      // AP 不會中斷，可以持續輪詢直到拿到 DHCP IP
      let tries = 0;
      const t = setInterval(async () => {
        tries++;
        const w = await loadWifi(true);
        if (w && w.connected) {
          clearInterval(t);
          btn.disabled = false;
          $('#wifiApplyMsg').innerHTML =
            `連線成功，DHCP 取得 IP <strong>${esc(w.ip)}</strong>　` +
            `<a href="http://${esc(w.ip)}/">改用這個位址開啟 →</a>`;
          toast('連線成功，IP ' + w.ip, 'ok');
        } else if (tries >= 25 || (w && !w.applying)) {
          clearInterval(t);
          btn.disabled = false;
          $('#wifiApplyMsg').textContent = '連線失敗，請確認 SSID 與密碼是否正確。';
          toast('連線失敗', 'err');
        }
      }, 1000);
    } catch (err) {
      btn.disabled = false;
      $('#wifiApplyMsg').textContent = '';
      toast(err.message, 'err');
    }
  };

  $('#wifiClearBtn').onclick = async () => {
    if (!confirm('清除已儲存的 WiFi 連線設定？AP 會保持開啟。')) return;
    try {
      const r = await post('/api/wifi/clear');
      toast(r.msg, 'ok');
      $('#wifiApplyMsg').textContent = '';
      loadWifi(true);
    } catch (e) { toast(e.message, 'err'); }
  };

  // ------------------------------------------------ DI

  function renderDi(list) {
    $('#diChannels').innerHTML = list.map((c, i) => `
      <div class="di-card">
        <header>
          <strong>DI${c.ch}　<span class="muted">GPIO${c.pin}　電位 <span id="diLv${i}">${c.level}</span></span></strong>
          <span class="badge ${c.state ? 'bad' : 'ok'}" id="diBadge${i}">${c.state ? '告警中' : '正常'}</span>
        </header>
        <div class="form">
          <label>通道名稱<input name="name${i}" value="${esc(c.name)}" maxlength="24"></label>
          <label>觸發告警文字<input name="alarm${i}" value="${esc(c.alarm)}" maxlength="80"></label>
          <label>解除告警文字<input name="normal${i}" value="${esc(c.normal)}" maxlength="80"></label>
          <label class="switch-row"><span>啟用此通道</span>
            <input type="checkbox" class="switch" name="en${i}" ${c.en ? 'checked' : ''}></label>
          <label class="switch-row"><span>低電位 (接點短路) 視為告警</span>
            <input type="checkbox" class="switch" name="low${i}" ${c.low ? 'checked' : ''}></label>
        </div>
      </div>`).join('');
  }

  async function loadDi() {
    try {
      const d = await get('/api/di');
      renderDi(d.ch || []);
      const f = $('#notifyForm');
      f.dcEn.checked = !!d.notify.dcEn;
      f.tgEn.checked = !!d.notify.tgEn;
      f.tgCid.value = d.notify.tgCid || '';
      $('#dcState').textContent = d.notify.dcUrlSet ? 'Webhook 已設定' : '尚未設定 Webhook';
      $('#tgState').textContent = d.notify.tgTokSet ? 'Bot Token 已設定' : '尚未設定 Bot Token';
      $('#ioTicks').textContent = d.ioTicks;
      await loadAlarms();
    } catch (e) { setOnline(false); }
  }
  loaders.di = loadDi;

  // 輪詢時只更新狀態徽章與告警紀錄，避免覆蓋使用者正在編輯的欄位
  async function refreshDi() {
    try {
      const d = await get('/api/di');
      (d.ch || []).forEach((c, i) => {
        const b = $('#diBadge' + i);
        if (!b) return;
        b.textContent = c.state ? '告警中' : '正常';
        b.className = 'badge ' + (c.state ? 'bad' : 'ok');
        const lv = $('#diLv' + i);
        if (lv) lv.textContent = c.level;
      });
      $('#ioTicks').textContent = d.ioTicks;
      await loadAlarms();
    } catch (e) { setOnline(false); }
  }

  async function loadAlarms() {
    const list = await get('/api/alarms');
    $('#alarmTable tbody').innerHTML = list.length
      ? list.map(a => `<tr><td>${esc(a.time)}</td><td>DI${a.ch}</td>
          <td><span class="badge ${a.a ? 'bad' : 'ok'}">${a.a ? '觸發' : '解除'}</span></td>
          <td>${esc(a.t)}</td></tr>`).join('')
      : '<tr><td colspan="4" class="muted">尚無告警紀錄</td></tr>';
  }

  $('#diForm').onsubmit = async e => {
    e.preventDefault();
    try {
      const r = await post('/api/di', formToObj($('#diChannels')));
      toast(r.msg, 'ok');
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#alarmClearBtn').onclick = async () => {
    if (!confirm('清除全部告警紀錄？')) return;
    try { await post('/api/alarms/clear'); toast('已清除', 'ok'); loadAlarms(); }
    catch (e) { toast(e.message, 'err'); }
  };

  $('#notifyForm').onsubmit = async e => {
    e.preventDefault();
    try {
      const r = await post('/api/notify', formToObj(e.target));
      toast(r.msg, 'ok');
      e.target.dcUrl.value = '';
      e.target.tgTok.value = '';
      loadDi();
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#notifyTestBtn').onclick = async () => {
    try {
      await post('/api/notify/test', { msg: 'ESP32 DIDO 測試推播' });
      toast('測試訊息已排入佇列，請查看 Discord / Telegram', 'ok');
    } catch (e) { toast(e.message, 'err'); }
  };

  // ------------------------------------------------ DO

  const MODE_NAME = ['手動', '定時', '點動'];

  // 控制卡片（開關 / 點動 / 自我測試 / 腳位診斷）
  function renderDoChannels(list) {
    $('#doChannels').innerHTML = (list || []).map(c => `
      <div class="di-card">
        <header>
          <strong>${esc(c.name)}　<span class="muted">GPIO${c.pin}</span></strong>
          <span class="badge ${c.on ? 'ok' : 'bad'}" id="doBadge${c.ch}">
            ${c.on ? 'ON' : 'OFF'}</span>
        </header>
        <label class="switch-row big">
          <span>輸出 ON / OFF</span>
          <input type="checkbox" class="switch" data-doswitch="${c.ch}" ${c.on ? 'checked' : ''}>
        </label>
        <table class="kv"><tbody>
          <tr><td>模式</td><td>${MODE_NAME[c.mode] || '?'}</td></tr>
          <tr><td>實際準位</td><td><strong id="doLevel${c.ch}">--</strong></td></tr>
          <tr><td>導通準位</td><td>${c.low ? 'LOW (Active Low)' : 'HIGH'}</td></tr>
        </tbody></table>
        <div class="row">
          <button class="btn" type="button" data-dopulse="${c.ch}">點動 ${c.pulseMs} ms</button>
          <button class="btn" type="button" data-dotest="${c.ch}">自我測試</button>
        </div>
      </div>`).join('');
  }

  // 設定表單（名稱 / 模式 / 極性 / 點動時間 / 排程）
  function renderDoConfigs(list) {
    $('#doConfigs').innerHTML = (list || []).map((c, i) => `
      <fieldset>
        <legend>${esc(c.name)}（GPIO${c.pin}）</legend>
        <label>名稱<input name="c${i}_name" value="${esc(c.name)}" maxlength="24"></label>
        <label>模式
          <select name="c${i}_mode" data-modesel="${i}">
            <option value="0">手動 (Switch 控制)</option>
            <option value="1">定時 (時間排程)</option>
            <option value="2">點動 (保持時間後自動關閉)</option>
          </select>
        </label>
        <label class="switch-row">
          <span>輸出低電位導通 (Active Low)</span>
          <input type="checkbox" class="switch" name="c${i}_low" ${c.low ? 'checked' : ''}>
        </label>
        <label>點動保持時間 (ms)
          <input name="c${i}_pulseMs" type="number" min="100" max="600000" step="100"
                 value="${c.pulseMs}">
        </label>
        <div data-schedbox="${i}">
          <p class="muted">時間排程（需已完成 NTP 校時才會生效）</p>
          ${(c.sched || []).map((sd, j) => `
            <div class="sched-row">
              <label class="switch-row"><span>排程 ${j + 1}</span>
                <input type="checkbox" class="switch" name="c${i}_sEn${j}" ${sd.en ? 'checked' : ''}></label>
              <label>開啟時間
                <input type="time" name="c${i}_sOn${j}"
                       value="${String(sd.onH).padStart(2, '0')}:${String(sd.onM).padStart(2, '0')}"></label>
              <label>關閉時間
                <input type="time" name="c${i}_sOff${j}"
                       value="${String(sd.offH).padStart(2, '0')}:${String(sd.offM).padStart(2, '0')}"></label>
              <div class="days" data-ch="${i}" data-idx="${j}">
                ${DAY.map((d, b) => `<label><input type="checkbox" data-day="${b}"
                  ${(sd.days >> b) & 1 ? 'checked' : ''}>${d}</label>`).join('')}
              </div>
            </div>`).join('')}
        </div>
      </fieldset>`).join('');

    // select 的值要在插入 DOM 後才設得進去
    (list || []).forEach((c, i) => {
      const sel = $(`[data-modesel="${i}"]`);
      if (sel) sel.value = c.mode;
      syncSchedBox(i);
    });
  }

  function syncSchedBox(i) {
    const sel = $(`[data-modesel="${i}"]`);
    const box = $(`[data-schedbox="${i}"]`);
    if (sel && box) box.style.display = sel.value === '1' ? '' : 'none';
  }

  // 只更新狀態，不重畫表單（避免覆蓋正在編輯的欄位）
  function updateDoState(st) {
    (st && st.ch || []).forEach(c => {
      const b = $('#doBadge' + c.ch);
      if (b) {
        b.textContent = c.on ? 'ON' : 'OFF';
        b.className = 'badge ' + (c.on ? 'ok' : 'bad');
      }
      const lv = $('#doLevel' + c.ch);
      if (lv) lv.textContent = c.level;
      const sw = $(`[data-doswitch="${c.ch}"]`);
      if (sw) sw.checked = !!c.on;
    });
  }

  async function loadDo() {
    try {
      const d = await get('/api/do');
      renderDoChannels(d.ch || []);
      renderDoConfigs(d.ch || []);
      updateDoState(await get('/api/do/state'));
    } catch (e) { setOnline(false); }
  }
  loaders.do = loadDo;

  $('#doConfigs').addEventListener('change', e => {
    if (e.target.dataset.modesel !== undefined) syncSchedBox(e.target.dataset.modesel);
  });

  $('#doChannels').addEventListener('change', async e => {
    const ch = e.target.dataset.doswitch;
    if (!ch) return;
    try {
      updateDoState(await post('/api/do/set', { ch, state: e.target.checked ? 'on' : 'off' }));
    } catch (err) { toast(err.message, 'err'); loadDo(); }
  });

  $('#doChannels').addEventListener('click', async e => {
    const pulseCh = e.target.dataset.dopulse;
    const testCh  = e.target.dataset.dotest;
    if (pulseCh) {
      try {
        updateDoState(await post('/api/do/pulse', { ch: pulseCh }));
        toast('已送出點動', 'ok');
        setTimeout(async () => {
          try { updateDoState(await get('/api/do/state')); } catch (err) {}
        }, 1200);
      } catch (err) { toast(err.message, 'err'); }
    } else if (testCh) {
      e.target.disabled = true;
      e.target.textContent = '測試中...';
      try {
        updateDoState(await post('/api/do/selftest', { ch: testCh }));
        toast('測試完成，請確認是否聽到繼電器動作', 'ok');
      } catch (err) { toast(err.message, 'err'); }
      e.target.disabled = false;
      e.target.textContent = '自我測試';
    }
  });

  $('#doForm').onsubmit = async e => {
    e.preventDefault();
    const o = formToObj(e.target);
    // time 欄位與星期核取方塊轉成後端格式
    $$('[data-schedbox]').forEach(box => {
      const i = box.dataset.schedbox;
      $$('.sched-row', box).forEach((row, j) => {
        const on  = (o[`c${i}_sOn${j}`]  || '00:00').split(':');
        const off = (o[`c${i}_sOff${j}`] || '00:00').split(':');
        o[`c${i}_sOnH${j}`]  = +on[0];  o[`c${i}_sOnM${j}`]  = +on[1];
        o[`c${i}_sOffH${j}`] = +off[0]; o[`c${i}_sOffM${j}`] = +off[1];
        delete o[`c${i}_sOn${j}`]; delete o[`c${i}_sOff${j}`];
        let days = 0;
        $$(`.days[data-ch="${i}"][data-idx="${j}"] input`).forEach(c => {
          if (c.checked) days |= (1 << +c.dataset.day);
        });
        o[`c${i}_sDays${j}`] = days;
      });
    });
    try {
      toast((await post('/api/do', o)).msg, 'ok');
      loadDo();
    } catch (err) { toast(err.message, 'err'); }
  };

  // ------------------------------------------------ MQTT

  async function loadMqtt() {
    try {
      const m = await get('/api/mqtt');
      const f = $('#mqttForm');
      f.en.checked = !!m.enabled;
      $('#mqttHost').value = m.host || '';
      $('#mqttPort').value = m.port || 1883;
      $('#idAuto').checked = !!m.idAuto;
      $('#mqttId').value = m.clientId || '';
      $('#mqttId').disabled = !!m.idAuto;
      $('#mqttUser').value = m.user || '';
      $('#pubTopic').value = m.pubTopic || '';
      $('#pubQos').value = m.pubQos;
      $('#subTopic').value = m.subTopic || '';
      $('#subQos').value = m.subQos;

      const b = $('#mqttBadge');
      b.textContent = m.enabled
        ? (m.connected ? '已連線' : '未連線：' + (m.stateText || m.state))
        : '已停用';
      b.className = 'badge ' + (m.connected ? 'ok' : 'bad');
      const f2 = $('#statePubForm');
      f2.retain.checked = !!m.retain;
      f2.statusSec.value = m.statusSec;
      renderTopics(m.pubTopic || '');

      $('#subQosNote').textContent =
        'PubSubClient 訂閱最高支援 QoS1，目前實際使用 QoS' + m.effSubQos + '。';

      // 發佈主題與訂閱主題不同時，自己發的訊息自己收不到，先講清楚
      const pt = m.pubTopic || '', st = m.subTopic || '';
      $('#pubHint').textContent = (pt && st && pt !== st)
        ? `注意：裝置訂閱的是「${st}」，發佈到「${pt}」不會回到下方訊息表。`
        : '';

      await loadMsgs();
    } catch (e) { setOnline(false); }
  }
  loaders.mqtt = loadMqtt;

  function renderTopics(pub) {
    const p0 = esc(pub) || '<i>(未設定 Topic)</i>';
    const rows = [
      [`${p0}/di/1`, '<code>on</code>=告警中　<code>off</code>=正常', 'DI 電位變化時'],
      [`${p0}/di/2`, '同上', 'DI 電位變化時'],
      [`${p0}/do/1`, '<code>on</code>=導通　<code>off</code>=斷開', 'DO 狀態變化時'],
      [`${p0}/do/2`, '同上', 'DO 狀態變化時'],
      [`${p0}/alarm`, 'JSON：ch / alarm / text / time', 'DI 告警觸發與解除時'],
      [`${p0}/status`, 'JSON：全部 DI、DO 與連線資訊', '連線後與每隔設定秒數']
    ];
    $('#topicTable').innerHTML = rows
      .map(r => `<tr><td>${r[0]}</td><td>${r[1]}</td><td>${r[2]}</td></tr>`).join('');
  }

  $('#statePubForm').onsubmit = async e => {
    e.preventDefault();
    try {
      toast((await post('/api/mqtt', formToObj(e.target))).msg, 'ok');
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#pushNowBtn').onclick = async () => {
    try { toast((await post('/api/mqtt/pushnow')).msg, 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  };

  // 輪詢時只更新連線徽章與訊息列表
  async function refreshMqtt() {
    try {
      const m = await get('/api/mqtt');
      const b = $('#mqttBadge');
      b.textContent = m.enabled
        ? (m.connected ? '已連線' : '未連線：' + (m.stateText || m.state))
        : '已停用';
      b.className = 'badge ' + (m.connected ? 'ok' : 'bad');
      await loadMsgs();
    } catch (e) { setOnline(false); }
  }

  async function loadMsgs() {
    const list = await get('/api/mqtt/messages');
    $('#msgTable tbody').innerHTML = list.length
      ? list.map(m => `<tr><td>${esc(m.time)}</td><td>${esc(m.topic)}</td>
          <td>${esc(m.payload)}</td></tr>`).join('')
      : '<tr><td colspan="3" class="muted">尚未收到訂閱訊息</td></tr>';
  }

  $('#idAuto').onchange = e => { $('#mqttId').disabled = e.target.checked; };

  $('#mqttForm').onsubmit = async e => {
    e.preventDefault();
    try {
      const r = await post('/api/mqtt', formToObj(e.target));
      toast(r.msg, 'ok');
      e.target.pass.value = '';
      setTimeout(loadMqtt, 1500);
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#pubForm').onsubmit = async e => {
    e.preventDefault();
    try {
      // QoS 一併存回設定
      await post('/api/mqtt', { pubTopic: $('#pubTopic').value, pubQos: $('#pubQos').value });
      const r = await post('/api/mqtt/publish', {
        topic: $('#pubTopic').value, msg: $('#pubMsg').value
      });
      toast(r.msg, 'ok');
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#subForm').onsubmit = async e => {
    e.preventDefault();
    try {
      const r = await post('/api/mqtt', {
        subTopic: $('#subTopic').value, subQos: $('#subQos').value
      });
      toast(r.msg, 'ok');
      setTimeout(loadMqtt, 1500);
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#loopTestBtn').onclick = async () => {
    const topic = $('#subTopic').value.trim();
    if (!topic) { toast('請先填寫訂閱 Topic', 'err'); return; }
    const msg = 'loopback test ' + new Date().toLocaleTimeString();
    try {
      await post('/api/mqtt/publish', { topic, msg });
      toast('已發佈到 ' + topic + '，等待回傳...', 'ok');
      let tries = 0;
      const t = setInterval(async () => {
        tries++;
        await loadMsgs();
        const hit = $('#msgTable tbody').textContent.indexOf(msg) >= 0;
        if (hit) { clearInterval(t); toast('迴圈測試成功，訂閱正常運作', 'ok'); }
        else if (tries >= 6) { clearInterval(t); toast('已發佈但未收到回傳，請檢查訂閱設定', 'err'); }
      }, 700);
    } catch (e) { toast(e.message, 'err'); }
  };

  $('#msgClearBtn').onclick = async () => {
    try { await post('/api/mqtt/messages/clear'); loadMsgs(); toast('已清除', 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  };

  // ------------------------------------------------ Modbus RTU

  const FC_NAME = { 1: '01 讀線圈', 2: '02 讀離散輸入', 3: '03 讀保持暫存器', 4: '04 讀輸入暫存器' };

  function syncMbMode() {
    const master = $('#mbMode').value === '1';
    $('#mbSlaveBox').style.display     = master ? 'none' : '';
    $('#mbMasterBox').style.display    = master ? '' : 'none';
    $('#mbSlaveMapCard').style.display = master ? 'none' : '';
    $('#mbMasterCard').style.display   = master ? '' : 'none';
  }

  function renderMbPolls(list) {
    $('#mbPollRows').innerHTML = (list || []).map(p => `
      <fieldset>
        <legend>${esc(p.name)}　<span class="badge ${p.valid ? 'ok' : 'bad'}"
          id="mbPollBadge${p.idx}">${p.valid ? '正常' : '--'}</span></legend>
        <label class="switch-row"><span>啟用</span>
          <input type="checkbox" class="switch" name="p${p.idx}_en" ${p.en ? 'checked' : ''}></label>
        <label>名稱（面板僅能顯示英數字，取前 7 字元）
          <input name="p${p.idx}_name" value="${esc(p.name)}" maxlength="20"></label>
        <div class="sched-row">
          <label>從站站號<input name="p${p.idx}_id" type="number" min="1" max="247" value="${p.id}"></label>
          <label>功能碼
            <select name="p${p.idx}_fc" data-fc="${p.idx}">
              <option value="1">01 讀線圈</option>
              <option value="2">02 讀離散輸入</option>
              <option value="3">03 讀保持暫存器</option>
              <option value="4">04 讀輸入暫存器</option>
            </select>
          </label>
          <label>起始位址<input name="p${p.idx}_addr" type="number" min="0" max="65535" value="${p.addr}"></label>
          <label>數量 (1~16)<input name="p${p.idx}_count" type="number" min="1" max="16" value="${p.count}"></label>
          <label>週期 (秒)<input name="p${p.idx}_period" type="number" min="0" max="3600" value="${p.period}"></label>
        </div>
        <p class="muted">讀值：<span id="mbPollVal${p.idx}">--</span></p>
      </fieldset>`).join('');
    (list || []).forEach(p => {
      const sel = $(`[data-fc="${p.idx}"]`);
      if (sel) sel.value = p.fc;
    });
  }

  function updateMbPolls(list) {
    (list || []).forEach(p => {
      const b = $('#mbPollBadge' + p.idx);
      if (b) {
        b.textContent = !p.en ? '停用' : (p.valid ? `正常 (${Math.round(p.ageMs / 1000)}s 前)`
                                                  : (p.errMsg || '尚無資料'));
        b.className = 'badge ' + (p.en && p.valid ? 'ok' : 'bad');
      }
      const v = $('#mbPollVal' + p.idx);
      if (v) {
        v.textContent = (p.values && p.values.length)
          ? p.values.map((x, k) => `[${p.addr + k}] ${x}`).join('　')
          : '--';
      }
    });
  }

  async function loadModbus(keepInputs) {
    try {
      const m = await get('/api/modbus');
      const f = $('#mbForm');
      if (!keepInputs) {
        f.en.checked = !!m.enabled;
        $('#mbMode').value = m.mode;
        f.baud.value = m.baud;
        f.parity.value = m.parity;
        f.stopBits.value = m.stopBits;
        f.slaveId.value = m.slaveId;
        f.timeout.value = m.timeout;
        f.publish.checked = !!m.publish;
        syncMbMode();
      }

      const b = $('#mbBadge');
      b.textContent = m.enabled ? (m.mode === 1 ? 'Master 執行中' : `Slave 站號 ${m.slaveId}`) : '已停用';
      b.className = 'badge ' + (m.enabled ? 'ok' : 'bad');
      $('#mbPins').textContent =
        `接線：TX(DI) GPIO${m.txPin}　RX(RO) GPIO${m.rxPin}　DE/RE GPIO${m.dePin}`;

      kvRows($('#mbStat'), [
        ['接收訊框', m.stat.rx],
        ['送出訊框', m.stat.tx],
        ['CRC 錯誤', m.stat.crcErr],
        ['例外回應', m.stat.exc],
        ['逾時無回應', m.stat.timeout]
      ]);

      const polls = await get('/api/modbus/poll');
      if (!keepInputs) renderMbPolls(polls);
      updateMbPolls(polls);
      refreshMbPage();
    } catch (e) { setOnline(false); }
  }
  loaders.modbus = () => loadModbus(false);

  $('#mbMode').onchange = syncMbMode;

  $('#mbForm').onsubmit = async e => {
    e.preventDefault();
    try {
      toast((await post('/api/modbus', formToObj(e.target))).msg, 'ok');
      setTimeout(() => loadModbus(true), 800);
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#mbPollForm').onsubmit = async e => {
    e.preventDefault();
    try {
      toast((await post('/api/modbus', formToObj(e.target))).msg, 'ok');
      setTimeout(() => loadModbus(true), 800);
    } catch (err) { toast(err.message, 'err'); }
  };

  const PAGE_NAME = ['狀態畫面', 'Modbus 輪詢數值', '自動輪替'];

  async function refreshMbPage() {
    try {
      const d = await get('/api/display');
      $('#mbPageNow').textContent = PAGE_NAME[d.page] || '--';
    } catch (e) { /* 顯示設定讀不到不影響本頁其他功能 */ }
  }

  $('#page-modbus').addEventListener('click', async e => {
    const pg = e.target.dataset.page;
    if (pg === undefined) return;
    try {
      toast((await post('/api/display/page', { page: pg })).msg, 'ok');
      refreshMbPage();
      tftLoaded = false;                 // 下次進系統狀態時重新載入顯示設定
    } catch (err) { toast(err.message, 'err'); }
  });

  $('#mbResetBtn').onclick = async () => {
    try { toast((await post('/api/modbus/reset')).msg, 'ok'); loadModbus(true); }
    catch (e) { toast(e.message, 'err'); }
  };

  // ------------------------------------------------ OTA

  $('#otaForm').onsubmit = e => {
    e.preventDefault();
    const file = $('#otaFile').files[0];
    if (!file) { toast('請先選擇 .bin 檔', 'err'); return; }
    if (!confirm('確定上傳 ' + file.name + ' 進行更新？')) return;

    const fd = new FormData();
    fd.append('update', file, file.name);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota?target=' + $('#otaTarget').value);
    xhr.upload.onprogress = ev => {
      if (!ev.lengthComputable) return;
      const pct = ev.loaded / ev.total * 100;
      $('#otaBar').style.width = pct.toFixed(1) + '%';
      $('#otaMsg').textContent = '上傳中 ' + pct.toFixed(1) + '%　(' +
        bytes(ev.loaded) + ' / ' + bytes(ev.total) + ')';
    };
    xhr.onload = () => {
      let d = {};
      try { d = JSON.parse(xhr.responseText); } catch (err) {}
      if (xhr.status === 200 && d.ok !== false) {
        $('#otaMsg').textContent = d.msg || '更新成功，裝置重新啟動中...';
        toast('更新成功，裝置重新啟動中', 'ok');
      } else {
        $('#otaMsg').textContent = d.msg || ('更新失敗 (HTTP ' + xhr.status + ')');
        toast($('#otaMsg').textContent, 'err');
      }
    };
    xhr.onerror = () => {
      $('#otaMsg').textContent = '上傳失敗，連線中斷';
      toast('上傳失敗', 'err');
    };
    $('#otaMsg').textContent = '開始上傳...';
    xhr.send(fd);
  };

  // ------------------------------------------------ 使用者

  let authConfigured = true;

  function applyAuthState(d) {
    authConfigured = !!d.configured;
    $('#authWarn').hidden = authConfigured;
    $('#oldPassRow').hidden = !authConfigured;
    $('#userForm').oldPass.required = authConfigured;
    $('#userHint').textContent = authConfigured
      ? '已設定登入帳號，變更時需輸入原密碼。'
      : '尚未設定登入帳號密碼，目前網頁免登入即可操作，請立即設定。';
  }

  async function loadUser() {
    try { const d = await get('/api/user'); $('#userName').value = d.user || ''; applyAuthState(d); }
    catch (e) { setOnline(false); }
  }
  loaders.user = loadUser;

  // 任何頁面都先確認一次是否已設定帳密，未設定就顯示橫幅
  get('/api/user').then(applyAuthState).catch(() => {});

  $('#userForm').onsubmit = async e => {
    e.preventDefault();
    if (e.target.newPass.value !== $('#newPass2').value) {
      toast('兩次輸入的新密碼不一致', 'err');
      return;
    }
    const o = formToObj(e.target);
    if (!authConfigured) delete o.oldPass;
    try {
      const r = await post('/api/user', o);
      toast(r.msg, 'ok');
      e.target.reset();
      setTimeout(() => location.reload(), 1500);
    } catch (err) { toast(err.message, 'err'); }
  };

  // ------------------------------------------------ 啟動

  show(location.hash.slice(1) || 'status');

  // 依目前頁面做輕量輪詢
  setInterval(() => {
    if (document.hidden) return;
    if (current === 'status') loadStatus();
    else if (current === 'di') { refreshDi(); }
    else if (current === 'do') { get('/api/do/state').then(updateDoState).catch(() => {}); }
    else if (current === 'mqtt') { refreshMqtt(); }
    else if (current === 'wifi') { loadWifi(true); }
    else if (current === 'modbus') { loadModbus(true); }
    else get('/api/status').then(() => setOnline(true)).catch(() => setOnline(false));
  }, 5000);
})();
