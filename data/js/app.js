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

  // ------------------------------------------------ 路由

  const PAGES = ['status', 'wifi', 'di', 'do', 'mqtt', 'ota', 'user'];
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
        ['模式', w.ap ? 'AP 設定模式' : 'STA 連線模式'],
        ['SSID', esc(w.ap ? w.apSsid : w.ssid) || '--'],
        ['狀態', w.connected ? '已連線' : (w.ap ? 'AP 待設定' : '未連線')],
        ['IP 位址', esc(w.ap ? w.apIp : w.ip)],
        ['閘道', esc(w.gw)],
        ['子網路遮罩', esc(w.mask)],
        ['DNS', esc(w.dns)],
        ['MAC', esc(w.mac)],
        ['訊號強度', w.connected ? w.rssi + ' dBm' : '--'],
        ['頻道', w.ch],
        ['主機名稱', esc(w.host)]
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
  loaders.status = loadStatus;

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

  $('#rebootBtn').onclick = async () => {
    if (!confirm('確定重新啟動裝置？')) return;
    try { await post('/api/reboot'); toast('裝置重新啟動中...', 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  };

  // ------------------------------------------------ WiFi

  let scanTimer = null;

  async function loadWifi() {
    try {
      const w = await get('/api/wifi');
      if (!$('#wifiSsid').value) $('#wifiSsid').value = w.ssid || '';
      $('#wifiHost').value = w.host || '';
    } catch (e) { setOnline(false); }
  }
  loaders.wifi = loadWifi;

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
    if (d.failed) {
      $('#scanMsg').textContent = '掃描失敗，請稍候再試一次。';
      toast('掃描失敗', 'err');
    } else {
      const n = (d.list || []).length;
      $('#scanMsg').textContent = n
        ? `找到 ${n} 個網路，點選即可填入 SSID。`
        : '沒有掃描到任何網路，請確認附近有 2.4GHz 網路（ESP32 不支援 5GHz）。';
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
    try {
      const r = await post('/api/wifi', formToObj(e.target));
      toast(r.msg, 'ok');
    } catch (err) { toast(err.message, 'err'); }
  };

  $('#wifiClearBtn').onclick = async () => {
    if (!confirm('清除 WiFi 連線設定並重新啟動？')) return;
    try {
      const r = await post('/api/wifi/clear');
      toast(r.msg, 'ok');
    } catch (e) { toast(e.message, 'err'); }
  };

  // ------------------------------------------------ DI

  function renderDi(list) {
    $('#diChannels').innerHTML = list.map((c, i) => `
      <div class="di-card">
        <header>
          <strong>DI${c.ch}</strong>
          <span class="badge ${c.state ? 'bad' : 'ok'}" id="diBadge${i}">${c.state ? '告警中' : '正常'}
            (電位 ${c.level})</span>
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
        b.textContent = (c.state ? '告警中' : '正常') + ' (電位 ' + c.level + ')';
        b.className = 'badge ' + (c.state ? 'bad' : 'ok');
      });
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

  function renderSched(list) {
    $('#schedRows').innerHTML = list.map((s, i) => `
      <div class="sched-row">
        <label class="switch-row"><span>排程 ${i + 1}</span>
          <input type="checkbox" class="switch" name="sEn${i}" ${s.en ? 'checked' : ''}></label>
        <label>開啟時間
          <input type="time" name="sOn${i}"
                 value="${String(s.onH).padStart(2, '0')}:${String(s.onM).padStart(2, '0')}"></label>
        <label>關閉時間
          <input type="time" name="sOff${i}"
                 value="${String(s.offH).padStart(2, '0')}:${String(s.offM).padStart(2, '0')}"></label>
        <div class="days" data-idx="${i}">
          ${DAY.map((d, b) => `<label><input type="checkbox" data-day="${b}"
            ${(s.days >> b) & 1 ? 'checked' : ''}>${d}</label>`).join('')}
        </div>
      </div>`).join('');
  }

  function syncModeBoxes() {
    const m = $('#doMode').value;
    $('#pulseBox').style.display = (m === '2') ? '' : 'none';
    $('#schedBox').style.display = (m === '1') ? '' : 'none';
  }

  async function loadDo() {
    try {
      const d = await get('/api/do');
      const f = $('#doForm');
      $('#doMode').value = d.mode;
      f.low.checked = !!d.low;
      $('#pulseMs').value = d.pulseMs;
      renderSched(d.sched || []);
      syncModeBoxes();
      updateDoState(d.on);
    } catch (e) { setOnline(false); }
  }
  loaders.do = loadDo;

  function updateDoState(on) {
    $('#doSwitch').checked = !!on;
    const b = $('#doState');
    b.textContent = on ? '繼電器 ON' : '繼電器 OFF';
    b.className = 'badge ' + (on ? 'ok' : 'bad');
  }

  $('#doMode').onchange = syncModeBoxes;

  $('#doSwitch').onchange = async e => {
    try {
      const d = await post('/api/do/set', { state: e.target.checked ? 'on' : 'off' });
      updateDoState(d.on);
    } catch (err) { toast(err.message, 'err'); loadDo(); }
  };

  $('#pulseBtn').onclick = async () => {
    try {
      const d = await post('/api/do/pulse', { ms: $('#pulseMs').value });
      updateDoState(d.on);
      toast('點動 ' + $('#pulseMs').value + ' ms', 'ok');
      setTimeout(async () => {
        try { updateDoState((await get('/api/do/state')).on); } catch (e) {}
      }, Number($('#pulseMs').value) + 400);
    } catch (e) { toast(e.message, 'err'); }
  };

  $('#doForm').onsubmit = async e => {
    e.preventDefault();
    const o = formToObj(e.target);
    // 把 time 欄位與星期核取方塊轉成後端格式
    $$('.sched-row').forEach((row, i) => {
      const on = (o['sOn' + i] || '00:00').split(':');
      const off = (o['sOff' + i] || '00:00').split(':');
      o['sOnH' + i] = +on[0]; o['sOnM' + i] = +on[1];
      o['sOffH' + i] = +off[0]; o['sOffM' + i] = +off[1];
      delete o['sOn' + i]; delete o['sOff' + i];
      let days = 0;
      $$('.days[data-idx="' + i + '"] input').forEach(c => {
        if (c.checked) days |= (1 << +c.dataset.day);
      });
      o['sDays' + i] = days;
    });
    try {
      const r = await post('/api/do', o);
      toast(r.msg, 'ok');
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
      b.textContent = m.enabled ? (m.connected ? '已連線' : '未連線 (state ' + m.state + ')') : '已停用';
      b.className = 'badge ' + (m.connected ? 'ok' : 'bad');
      $('#subQosNote').textContent =
        'PubSubClient 訂閱最高支援 QoS1，目前實際使用 QoS' + m.effSubQos + '。';

      await loadMsgs();
    } catch (e) { setOnline(false); }
  }
  loaders.mqtt = loadMqtt;

  // 輪詢時只更新連線徽章與訊息列表
  async function refreshMqtt() {
    try {
      const m = await get('/api/mqtt');
      const b = $('#mqttBadge');
      b.textContent = m.enabled ? (m.connected ? '已連線' : '未連線 (state ' + m.state + ')') : '已停用';
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

  $('#msgClearBtn').onclick = async () => {
    try { await post('/api/mqtt/messages/clear'); loadMsgs(); toast('已清除', 'ok'); }
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
    else if (current === 'do') { get('/api/do/state').then(d => updateDoState(d.on)).catch(() => {}); }
    else if (current === 'mqtt') { refreshMqtt(); }
    else get('/api/status').then(() => setOnline(true)).catch(() => setOnline(false));
  }, 5000);
})();
