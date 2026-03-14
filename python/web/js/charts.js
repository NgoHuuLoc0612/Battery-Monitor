/**
 * Battery Monitor — Charts Module
 * Real-time Chart.js visualizations: power, voltage, temperature,
 * capacity trend, V-I scatter, histogram, health donut, radar
 */

'use strict';

const ChartDefaults = {
  responsive: true,
  maintainAspectRatio: false,
  animation: { duration: 0 },
  plugins: {
    legend: {
      labels: {
        color: '#5a7080',
        font: { family: "'Share Tech Mono', monospace", size: 10 },
        boxWidth: 8,
      }
    },
    tooltip: {
      backgroundColor: '#0d1219',
      borderColor: '#1e2d3e',
      borderWidth: 1,
      titleColor: '#00e5ff',
      bodyColor: '#c8d8e8',
      titleFont: { family: "'Share Tech Mono', monospace", size: 10 },
      bodyFont: { family: "'Share Tech Mono', monospace", size: 10 },
    }
  },
  scales: {
    x: {
      ticks: { color: '#5a7080', font: { family: "'Share Tech Mono', monospace", size: 8 }, maxTicksLimit: 8 },
      grid: { color: 'rgba(30,45,62,0.6)' },
      border: { color: '#1e2d3e' },
    },
    y: {
      ticks: { color: '#5a7080', font: { family: "'Share Tech Mono', monospace", size: 8 }, maxTicksLimit: 6 },
      grid: { color: 'rgba(30,45,62,0.6)' },
      border: { color: '#1e2d3e' },
    }
  }
};

// ─── Rolling buffer helper ─────────────────────────────────────────────────────
class RollingBuffer {
  constructor(maxLen) {
    this.maxLen = maxLen;
    this.data = [];
  }
  push(val) {
    this.data.push(val);
    if (this.data.length > this.maxLen) this.data.shift();
  }
  get() { return [...this.data]; }
  clear() { this.data = []; }
}

// ─── Charts registry ───────────────────────────────────────────────────────────
const Charts = {};
const BUF_SIZE = 120;

const powerBuf    = new RollingBuffer(BUF_SIZE);
const voltageBuf  = new RollingBuffer(BUF_SIZE);
const tempBuf     = new RollingBuffer(BUF_SIZE);
const capBuf      = new RollingBuffer(BUF_SIZE);
const labelBuf    = new RollingBuffer(BUF_SIZE);
const currentBuf  = new RollingBuffer(BUF_SIZE);

function makeTimeLabel() {
  const d = new Date();
  return `${d.getHours().toString().padStart(2,'0')}:${d.getMinutes().toString().padStart(2,'0')}:${d.getSeconds().toString().padStart(2,'0')}`;
}

function deepMerge(target, ...sources) {
  for (const src of sources) {
    for (const key in src) {
      if (src[key] && typeof src[key] === 'object' && !Array.isArray(src[key])) {
        target[key] = deepMerge(target[key] || {}, src[key]);
      } else {
        target[key] = src[key];
      }
    }
  }
  return target;
}

function cfg(overrides) {
  return deepMerge(JSON.parse(JSON.stringify(ChartDefaults)), overrides);
}

// ─── Initialize all charts ─────────────────────────────────────────────────────
function initCharts() {

  // Power chart
  Charts.power = new Chart(document.getElementById('chartPower'), cfg({
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: 'Power (W)',
        data: [],
        borderColor: '#00e5ff',
        backgroundColor: 'rgba(0,229,255,0.06)',
        fill: true,
        borderWidth: 1.5,
        pointRadius: 0,
        tension: 0.3,
      }]
    },
    options: {
      scales: {
        y: { title: { display: true, text: 'W', color: '#5a7080', font: { size: 9 } } }
      }
    }
  }));

  // Voltage chart
  Charts.voltage = new Chart(document.getElementById('chartVoltage'), cfg({
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: 'Voltage (V)',
        data: [],
        borderColor: '#ffb300',
        backgroundColor: 'rgba(255,179,0,0.05)',
        fill: true,
        borderWidth: 1.5,
        pointRadius: 0,
        tension: 0.3,
      }]
    },
    options: {
      scales: {
        y: {
          min: 3.0, max: 4.4,
          title: { display: true, text: 'V', color: '#5a7080', font: { size: 9 } }
        }
      }
    }
  }));

  // V-I Correlation scatter
  Charts.vic = new Chart(document.getElementById('chartVIC'), cfg({
    type: 'scatter',
    data: {
      datasets: [{
        label: 'V-I Correlation',
        data: [],
        backgroundColor: 'rgba(0,229,255,0.5)',
        pointRadius: 3,
        pointHoverRadius: 5,
      }]
    },
    options: {
      scales: {
        x: { title: { display: true, text: 'Voltage (V)', color: '#5a7080', font: { size: 9 } } },
        y: { title: { display: true, text: 'Current (A)', color: '#5a7080', font: { size: 9 } } },
      }
    }
  }));

  // Temperature chart
  Charts.temp = new Chart(document.getElementById('chartTemp'), cfg({
    type: 'line',
    data: {
      labels: [],
      datasets: [
        {
          label: 'Temperature (°C)',
          data: [],
          borderColor: '#ff6b35',
          backgroundColor: 'rgba(255,107,53,0.06)',
          fill: true,
          borderWidth: 1.5,
          pointRadius: 0,
          tension: 0.3,
        },
        {
          label: 'Warning (45°C)',
          data: [],
          borderColor: 'rgba(255,179,0,0.4)',
          borderWidth: 1,
          borderDash: [4, 4],
          pointRadius: 0,
          fill: false,
        }
      ]
    },
    options: {
      scales: {
        y: {
          title: { display: true, text: '°C', color: '#5a7080', font: { size: 9 } }
        }
      }
    }
  }));

  // Capacity chart
  Charts.capacity = new Chart(document.getElementById('chartCapacity'), cfg({
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: 'Capacity (mWh)',
        data: [],
        borderColor: '#a855f7',
        backgroundColor: 'rgba(168,85,247,0.06)',
        fill: true,
        borderWidth: 1.5,
        pointRadius: 0,
        tension: 0.3,
      }]
    },
    options: {
      scales: {
        y: { title: { display: true, text: 'mWh', color: '#5a7080', font: { size: 9 } } }
      }
    }
  }));

  // Discharge rate histogram
  Charts.hist = new Chart(document.getElementById('chartHist'), cfg({
    type: 'bar',
    data: {
      labels: [],
      datasets: [{
        label: 'Frequency',
        data: [],
        backgroundColor: 'rgba(0,214,143,0.5)',
        borderColor: '#00d68f',
        borderWidth: 1,
      }]
    },
    options: {
      scales: {
        x: { title: { display: true, text: 'Power (W) bin', color: '#5a7080', font: { size: 9 } } },
        y: { title: { display: true, text: 'Count', color: '#5a7080', font: { size: 9 } } },
      }
    }
  }));

  // Health donut
  Charts.healthDonut = new Chart(document.getElementById('chartHealthDonut'), {
    type: 'doughnut',
    data: {
      datasets: [{
        data: [87, 13],
        backgroundColor: ['#00d68f', '#1e2d3e'],
        borderWidth: 0,
        cutout: '78%',
      }]
    },
    options: {
      responsive: false,
      animation: { duration: 800 },
      plugins: { legend: { display: false }, tooltip: { enabled: false } }
    }
  });

  // Radar
  Charts.radar = new Chart(document.getElementById('chartRadar'), {
    type: 'radar',
    data: {
      labels: ['Capacity', 'Voltage', 'Thermal', 'Resistance', 'Efficiency', 'Cycles'],
      datasets: [{
        label: 'Health Dimensions',
        data: [87, 90, 75, 80, 92, 70],
        backgroundColor: 'rgba(0,229,255,0.1)',
        borderColor: '#00e5ff',
        borderWidth: 1.5,
        pointBackgroundColor: '#00e5ff',
        pointRadius: 4,
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 400 },
      scales: {
        r: {
          min: 0, max: 100,
          ticks: { display: false },
          grid: { color: 'rgba(30,45,62,0.8)' },
          angleLines: { color: 'rgba(30,45,62,0.8)' },
          pointLabels: {
            color: '#5a7080',
            font: { family: "'Share Tech Mono', monospace", size: 9 }
          }
        }
      },
      plugins: {
        legend: {
          labels: {
            color: '#5a7080',
            font: { family: "'Share Tech Mono', monospace", size: 10 }
          }
        }
      }
    }
  });
}

// ─── Update charts with new snapshot ──────────────────────────────────────────
function updateCharts(snap) {
  const lbl = makeTimeLabel();
  labelBuf.push(lbl);
  powerBuf.push(snap.power_now || 0);
  voltageBuf.push(snap.voltage || 0);
  tempBuf.push(snap.temperature_celsius || 0);
  capBuf.push(snap.capacity_now || 0);
  currentBuf.push(snap.current_now || 0);

  const labels = labelBuf.get();

  // Power
  Charts.power.data.labels = labels;
  Charts.power.data.datasets[0].data = powerBuf.get();
  Charts.power.update('none');

  // Voltage
  Charts.voltage.data.labels = labels;
  Charts.voltage.data.datasets[0].data = voltageBuf.get();
  Charts.voltage.update('none');

  // Temperature
  Charts.temp.data.labels = labels;
  Charts.temp.data.datasets[0].data = tempBuf.get();
  Charts.temp.data.datasets[1].data = tempBuf.get().map(() => 45);
  Charts.temp.update('none');

  // Capacity
  Charts.capacity.data.labels = labels;
  Charts.capacity.data.datasets[0].data = capBuf.get();
  Charts.capacity.update('none');

  // V-I scatter (add point)
  if (snap.voltage && snap.current_now !== undefined) {
    const vicData = Charts.vic.data.datasets[0].data;
    vicData.push({ x: snap.voltage, y: snap.current_now });
    if (vicData.length > BUF_SIZE) vicData.shift();
    Charts.vic.update('none');
  }
}

// ─── Histogram rebuild ─────────────────────────────────────────────────────────
function rebuildHistogram(powerHistory) {
  if (!powerHistory || powerHistory.length === 0) return;
  const bins = 10;
  const min  = Math.min(...powerHistory);
  const max  = Math.max(...powerHistory) + 0.001;
  const step = (max - min) / bins;
  const counts = new Array(bins).fill(0);
  const labels = [];

  for (let i = 0; i < bins; i++)
    labels.push((min + i * step).toFixed(1));

  for (const v of powerHistory) {
    const idx = Math.min(Math.floor((v - min) / step), bins - 1);
    counts[idx]++;
  }

  Charts.hist.data.labels = labels;
  Charts.hist.data.datasets[0].data = counts;
  Charts.hist.update();
}

// ─── Health donut & radar update ──────────────────────────────────────────────
function updateHealthCharts(report) {
  const h = report.current_health || 0;
  Charts.healthDonut.data.datasets[0].data = [h, 100 - h];
  const color = h >= 80 ? '#00d68f' : h >= 60 ? '#ffb300' : '#ff3b30';
  Charts.healthDonut.data.datasets[0].backgroundColor[0] = color;
  Charts.healthDonut.update();

  // Radar: normalize each dimension to 0-100
  const thermalScore = Math.max(0, 100 - (report.thermal_risk_index || 0));
  const resistScore  = Math.max(0, 100 - Math.min(100, ((report.internal_resistance || 0) / 300) * 100));
  const cycleScore   = Math.max(0, Math.min(100, ((report.estimated_remaining_cycles || 500) / 1000) * 100));

  Charts.radar.data.datasets[0].data = [
    h,
    90,        // voltage (placeholder, could be derived)
    thermalScore,
    resistScore,
    report.avg_power_draw > 0 ? Math.min(100, 100 - report.avg_power_draw * 2) : 80,
    cycleScore,
  ];
  Charts.radar.update();
}

// ─── Export ───────────────────────────────────────────────────────────────────
window.BatteryCharts = { initCharts, updateCharts, rebuildHistogram, updateHealthCharts };
