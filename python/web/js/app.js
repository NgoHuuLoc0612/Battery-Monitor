/**
 * Battery Monitor — Main Application
 * WebSocket client, state machine, UI orchestration,
 * view routing, alert management, CSV export, all realtime updates
 */

'use strict';

(function () {

  // ─── Config ─────────────────────────────────────────────────────────────────
  const WS_URL         = 'ws://localhost:8765';
  const RECONNECT_MS   = 3000;
  const HIST_REQUEST_INTERVAL = 30000; // request history every 30s

  // ─── State ───────────────────────────────────────────────────────────────────
  const state = {
    ws: null,
    connected: false,
    currentView: 'dashboard',
    latestSnap: null,
    latestReport: null,
    telemetryBuffer: [],
    activeAlerts: [],
    alertHistory: [],
    powerHistoryFull: [],
    reconnectTimer: null,
    chartsReady: false,
    viz3dActive: false,
  };

  // ─── DOM refs ─────────────────────────────────────────────────────────────
  const $ = id => document.getElementById(id);
  const dom = {
    connStatus:    document.querySelector('.conn-status'),
    connLabel:     $('connLabel'),
    alertBanner:   $('alertBanner'),
    alertBadge:    $('alertBadge'),
    clock:         $('clock'),
    exportBtn:     $('exportBtn'),
    battFill:      $('battFill'),
    battGlow:      $('battGlow'),
    battPct:       $('battPct'),
    statusPill:    $('statusPill'),
    statusEta:     $('statusEta'),
    activeAlertsList:  $('activeAlertsList'),
    alertHistoryList:  $('alertHistoryList'),
    anomalyList:   $('anomalyList'),
  };

  // ─── Clock ───────────────────────────────────────────────────────────────────
  function updateClock() {
    if (!dom.clock) return;
    const d = new Date();
    dom.clock.textContent = d.toLocaleTimeString('en-US', { hour12: false });
  }
  setInterval(updateClock, 1000);
  updateClock();

  // ─── WebSocket ───────────────────────────────────────────────────────────────
  function connect() {
    if (state.ws) {
      state.ws.onclose = null;
      state.ws.close();
    }
    setConnStatus('connecting');

    const ws = new WebSocket(WS_URL);
    state.ws = ws;

    ws.onopen = () => {
      state.connected = true;
      setConnStatus('connected');
      clearTimeout(state.reconnectTimer);
      ws.send(JSON.stringify({ cmd: 'get_health_report' }));
      ws.send(JSON.stringify({ cmd: 'get_alerts' }));
      scheduleHistoryRequests();
    };

    ws.onmessage = e => {
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      handleMessage(msg);
    };

    ws.onclose = () => {
      state.connected = false;
      setConnStatus('error');
      state.reconnectTimer = setTimeout(connect, RECONNECT_MS);
    };

    ws.onerror = () => ws.close();
  }

  function setConnStatus(status) {
    dom.connStatus.className = 'conn-status ' + status;
    dom.connLabel.textContent = {
      connecting: 'CONNECTING',
      connected:  'LIVE',
      error:      'OFFLINE',
    }[status] || status.toUpperCase();
  }

  function send(obj) {
    if (state.ws && state.ws.readyState === WebSocket.OPEN)
      state.ws.send(JSON.stringify(obj));
  }

  // ─── Message handler ──────────────────────────────────────────────────────
  function handleMessage(msg) {
    switch (msg.type) {
      case 'snapshot':
        state.latestSnap = msg.data;
        updateDashboard(msg.data);
        if (state.currentView === 'analytics') updateAnalytics(msg.data);
        if (state.currentView === 'viz3d' && state.viz3dActive) Viz3D.update3D(msg.data);
        if (msg.alerts) handleAlerts(msg.alerts);
        break;
      case 'health_report':
        state.latestReport = msg.data;
        updateHealthView(msg.data);
        break;
      case 'telemetry':
        state.telemetryBuffer = msg.data;
        rebuildAnalyticsFromBuffer(msg.data);
        break;
      case 'history':
        if (msg.kind === 'power') {
          state.powerHistoryFull = msg.data;
          BatteryCharts.rebuildHistogram(msg.data);
        }
        break;
      case 'alerts':
        state.activeAlerts  = msg.active || [];
        state.alertHistory  = msg.history || [];
        renderAlertsView();
        renderAlertBanner(state.activeAlerts);
        break;
      case 'export_csv':
        downloadCSV(msg.data);
        break;
      case 'pong':
        break;
    }
  }

  // ─── Dashboard update ─────────────────────────────────────────────────────
  function updateDashboard(snap) {
    if (!snap) return;

    // Battery visual
    const pct = snap.soc_accurate || snap.percent_remaining || 0;
    dom.battFill.style.height = `${Math.max(0, Math.min(100, pct))}%`;
    dom.battPct.textContent   = `${pct.toFixed(0)}%`;

    // Color based on level
    dom.battFill.classList.toggle('low',     pct < 15);
    dom.battFill.classList.toggle('warning', pct >= 15 && pct < 30);
    dom.battFill.classList.toggle('charging', snap.charging);

    // Status pill
    let pillText = 'IDLE';
    if (snap.charging)    pillText = 'CHARGING';
    else if (snap.discharging) pillText = 'DISCHARGING';
    else if (pct >= 99)   pillText = 'FULL';
    dom.statusPill.textContent = pillText;
    dom.statusPill.className   = 'status-pill ' + pillText.toLowerCase();

    // ETA
    if (snap.charging && snap.minutes_to_full > 0) {
      dom.statusEta.textContent = `Full in ${formatMinutes(snap.minutes_to_full)}`;
    } else if (!snap.charging && snap.minutes_to_empty > 0) {
      dom.statusEta.textContent = `${formatMinutes(snap.minutes_to_empty)} remaining`;
    } else if (snap.remaining_seconds > 0) {
      dom.statusEta.textContent = `${formatSeconds(snap.remaining_seconds)} remaining`;
    } else {
      dom.statusEta.textContent = '—';
    }

    // Metrics
    setMetric('mVoltage',    snap.voltage,            3);
    setMetric('mCurrent',    snap.current_now,        3);
    setMetric('mPower',      snap.power_now,          2);
    setMetric('mTemp',       snap.temperature_celsius, 1);
    setMetric('mHealth',     snap.health_percent,     1);
    setMetric('mCapacity',   snap.capacity_now,       0);
    setMetric('mResistance', snap.internal_resistance, 1);
    setMetric('mCrate',      snap.c_rate,             3);
    setMetric('mCycles',     snap.cycle_count,        0);
    setMetric('mThermal',    snap.thermal_risk_index, 1);
    setMetric('mEfficiency', snap.charging_efficiency, 1);
    setMetric('mEnergy',     snap.energy_accumulated, 3);

    // Info
    setText('infoName',   snap.device_name   || '—');
    setText('infoMfr',    snap.manufacturer  || '—');
    setText('infoSerial', snap.serial_number || '—');
    setText('infoChem',   snap.chemistry     || '—');
    setText('infoDesign', snap.design_capacity ? `${snap.design_capacity.toFixed(0)} mWh` : '—');
    setText('infoFull',   snap.full_charge_capacity ? `${snap.full_charge_capacity.toFixed(0)} mWh` : '—');
    setText('infoSoc',    snap.soc_accurate != null ? `${snap.soc_accurate.toFixed(2)}%` : '—');
    setText('infoAc',     snap.ac_online ? '✓ Connected' : '✗ Battery');

    // Charts
    if (state.chartsReady) BatteryCharts.updateCharts(snap);
  }

  function setMetric(id, val, decimals) {
    const el = $(id);
    if (!el) return;
    const formatted = val != null && !isNaN(val) ? val.toFixed(decimals) : '—';
    if (el.textContent !== formatted) {
      el.textContent = formatted;
      el.classList.add('updated');
      setTimeout(() => el.classList.remove('updated'), 400);
    }
  }

  function setText(id, val) {
    const el = $(id);
    if (el) el.textContent = val;
  }

  // ─── Analytics ────────────────────────────────────────────────────────────
  function updateAnalytics(snap) {
    // Charts update is driven by updateDashboard which always runs
  }

  function rebuildAnalyticsFromBuffer(buf) {
    if (!buf || buf.length === 0) return;
    // Rebuild full history on telemetry receive
    buf.forEach(snap => BatteryCharts.updateCharts(snap));
  }

  // ─── Health View ──────────────────────────────────────────────────────────
  function updateHealthView(report) {
    if (!report) return;

    const h = report.current_health || 0;
    const gradeEl = $('healthGrade'), pctEl = $('healthPct');
    if (gradeEl) gradeEl.textContent = report.grade || '—';
    if (pctEl)   pctEl.textContent   = `${h.toFixed(1)}%`;

    setBar('hbarRetention', h,                              `${h.toFixed(1)}%`,    'hbarRetentionVal');
    setBar('hbarThermal',   Math.max(0, 100 - (report.thermal_risk_index || 0)),
                            `${(report.thermal_risk_index || 0).toFixed(1)}`,      'hbarThermalVal');
    setBar('hbarResist',    Math.max(0, 100 - Math.min(100, ((report.internal_resistance || 0) / 300) * 100)),
                            `${(report.internal_resistance || 0).toFixed(0)}mΩ`,  'hbarResistVal');
    setBar('hbarCycle',     Math.min(100, ((report.estimated_remaining_cycles || 0) / 1000) * 100),
                            `${report.estimated_remaining_cycles || 0}`,           'hbarCycleVal');

    setText('hrCycles',    report.estimated_remaining_cycles || '—');
    setText('hrLife',      report.estimated_remaining_life ? `${report.estimated_remaining_life.toFixed(0)} days` : '—');
    setText('hrAvgPower',  report.avg_power_draw ? `${report.avg_power_draw.toFixed(2)} W` : '—');
    setText('hrMaxTemp',   report.max_temperature ? `${report.max_temperature.toFixed(1)}°C` : '—');

    // Anomalies
    if (dom.anomalyList) {
      if (!report.anomalies || report.anomalies.length === 0) {
        dom.anomalyList.innerHTML = '<div class="anomaly-none">✓ No anomalies detected</div>';
      } else {
        dom.anomalyList.innerHTML = report.anomalies.map(a =>
          `<div class="anomaly-tag">⚠ ${a.replace(/_/g, ' ')}</div>`
        ).join('');
      }
    }

    if (state.chartsReady) BatteryCharts.updateHealthCharts(report);
  }

  function setBar(barId, pct, label, labelId) {
    const bar = $(barId);
    if (bar) bar.style.width = `${Math.max(0, Math.min(100, pct))}%`;
    const lbl = $(labelId);
    if (lbl) lbl.textContent = label;
  }

  // ─── Alerts ───────────────────────────────────────────────────────────────
  function handleAlerts(newAlerts) {
    for (const a of newAlerts) {
      state.activeAlerts = state.activeAlerts.filter(x => x.id !== a.id);
      state.activeAlerts.push(a);
      state.alertHistory.push(a);
    }
    renderAlertBanner(state.activeAlerts);
    renderAlertsView();
    updateAlertBadge(state.activeAlerts.length);
  }

  function renderAlertBanner(alerts) {
    if (!dom.alertBanner) return;
    if (!alerts || alerts.length === 0) {
      dom.alertBanner.classList.remove('active');
      dom.alertBanner.innerHTML = '';
      return;
    }
    dom.alertBanner.classList.add('active');
    dom.alertBanner.innerHTML = alerts.slice(0, 3).map(a =>
      `<div class="alert-item-banner ${a.severity}">⚠ ${a.message}</div>`
    ).join('');
  }

  function renderAlertsView() {
    if (dom.activeAlertsList) {
      if (!state.activeAlerts.length) {
        dom.activeAlertsList.innerHTML = '<div class="no-alerts">✓ No active alerts</div>';
      } else {
        dom.activeAlertsList.innerHTML = state.activeAlerts.map(a => renderAlertEntry(a)).join('');
      }
    }
    if (dom.alertHistoryList) {
      if (!state.alertHistory.length) {
        dom.alertHistoryList.innerHTML = '<div class="no-alerts">No alert history</div>';
      } else {
        dom.alertHistoryList.innerHTML = [...state.alertHistory].reverse().slice(0, 30).map(a => renderAlertEntry(a)).join('');
      }
    }
    updateAlertBadge(state.activeAlerts.length);
  }

  function renderAlertEntry(alert) {
    const ts = alert.timestamp
      ? new Date(alert.timestamp).toLocaleTimeString('en-US', { hour12: false })
      : '';
    return `<div class="alert-entry ${alert.severity}">
      <span class="alert-msg">${alert.message}</span>
      <span class="alert-time">${ts} · ${alert.severity.toUpperCase()}</span>
    </div>`;
  }

  function updateAlertBadge(n) {
    if (dom.alertBadge) dom.alertBadge.textContent = n;
  }

  // ─── History request schedule ─────────────────────────────────────────────
  function scheduleHistoryRequests() {
    setInterval(() => {
      if (!state.connected) return;
      send({ cmd: 'get_history', kind: 'power' });
      send({ cmd: 'get_health_report' });
    }, HIST_REQUEST_INTERVAL);

    // Initial
    setTimeout(() => send({ cmd: 'get_history', kind: 'power' }), 5000);
    setTimeout(() => send({ cmd: 'get_telemetry', samples: 300 }), 3000);
  }

  // ─── View routing ──────────────────────────────────────────────────────────
  function switchView(viewName) {
    document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
    document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));

    const viewEl = document.getElementById(`view-${viewName}`);
    if (viewEl) viewEl.classList.add('active');

    const btnEl = document.querySelector(`[data-view="${viewName}"]`);
    if (btnEl) btnEl.classList.add('active');

    state.currentView = viewName;

    if (viewName === 'viz3d') {
      state.viz3dActive = true;
      // Small delay for DOM to render canvas
      setTimeout(() => {
        Viz3D.init3D();
        if (state.latestSnap) Viz3D.update3D(state.latestSnap);
      }, 100);
    } else {
      state.viz3dActive = false;
    }

    if (viewName === 'health') {
      if (state.latestReport) updateHealthView(state.latestReport);
      else send({ cmd: 'get_health_report' });
    }

    if (viewName === 'alerts') {
      renderAlertsView();
      send({ cmd: 'get_alerts' });
    }

    if (viewName === 'analytics') {
      send({ cmd: 'get_telemetry', samples: 300 });
    }
  }

  document.querySelectorAll('.nav-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const view = btn.dataset.view;
      if (view) switchView(view);
    });
  });

  // ─── Export CSV ────────────────────────────────────────────────────────────
  if (dom.exportBtn) {
    dom.exportBtn.addEventListener('click', () => {
      send({ cmd: 'export_csv', samples: 300 });
    });
  }

  function downloadCSV(csvData) {
    if (!csvData) return;
    const blob = new Blob([csvData], { type: 'text/csv' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href     = url;
    a.download = `battery_telemetry_${new Date().toISOString().slice(0,19).replace(/:/g,'-')}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  }

  // ─── Utilities ────────────────────────────────────────────────────────────
  function formatMinutes(mins) {
    if (!mins || mins <= 0) return '—';
    const h = Math.floor(mins / 60), m = Math.floor(mins % 60);
    return h > 0 ? `${h}h ${m}m` : `${m}m`;
  }

  function formatSeconds(sec) {
    if (!sec || sec <= 0) return '—';
    return formatMinutes(sec / 60);
  }

  // ─── Init ──────────────────────────────────────────────────────────────────
  document.addEventListener('DOMContentLoaded', () => {
    BatteryCharts.initCharts();
    state.chartsReady = true;

    // Simulate demo data if no WebSocket (for preview)
    startDemoIfOffline();
    connect();
  });

  // ─── Demo mode (no server) ────────────────────────────────────────────────
  function startDemoIfOffline() {
    let demoT = 0;
    const demoInterval = setInterval(() => {
      if (state.connected) { clearInterval(demoInterval); return; }
      demoT += 1;
      const soc = 75 - (demoT % 120) * 0.4;
      const snap = {
        timestamp: Date.now(),
        ac_online: false,
        charging: false,
        discharging: true,
        battery_present: true,
        percent_remaining: soc,
        soc_accurate: soc,
        voltage: 3.7 + soc / 400,
        current_now: -1.85,
        power_now: 7.24,
        temperature_celsius: 31.5 + Math.sin(demoT * 0.1) * 2,
        capacity_now: soc * 452,
        full_charge_capacity: 45200,
        design_capacity: 52000,
        health_percent: 87.3,
        wear_level: 12.7,
        cycle_count: 412,
        internal_resistance: 85,
        c_rate: 0.16,
        thermal_risk_index: 8,
        minutes_to_empty: soc * 1.8,
        minutes_to_full: 0,
        charging_efficiency: 0,
        energy_accumulated: demoT * 0.002,
        charge_rate: 0,
        discharge_rate: 7240,
        chemistry: 'LION',
        device_name: 'DEMO Battery',
        manufacturer: 'BatteryMonitor',
        serial_number: 'DEMO-0001',
        remaining_seconds: soc * 108,
      };
      updateDashboard(snap);
      if (state.viz3dActive) Viz3D.update3D(snap);
      state.latestSnap = snap;
    }, 1000);

    // Demo health report
    setTimeout(() => {
      if (!state.connected) {
        const demoReport = {
          current_health: 87.3, wear_level: 12.7, cycle_count: 412,
          internal_resistance: 85, thermal_risk_index: 8, avg_power_draw: 7.2,
          max_temperature: 35.2, min_temperature: 24.1,
          estimated_remaining_cycles: 638, estimated_remaining_life: 638,
          grade: 'Good', anomalies: [],
        };
        updateHealthView(demoReport);
        state.latestReport = demoReport;
      }
    }, 500);
  }

})();
