# Battery Monitor — Enterprise
> C++ WinAPI Core · pybind11 Bridge · Python WebSocket Server · HTML/JS Frontend

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Frontend  (HTML + CSS + JS)                            │
│  ├─ index.html      — Layout, views, markup             │
│  ├─ css/style.css   — Industrial dark aesthetic         │
│  ├─ js/charts.js    — Chart.js realtime visualizations  │
│  ├─ js/viz3d.js     — Three.js 3D battery cell model   │
│  └─ js/app.js       — WebSocket client, UI orchestration│
└────────────────┬────────────────────────────────────────┘
                 │ WebSocket ws://localhost:8765
┌────────────────▼────────────────────────────────────────┐
│  Python Server  (python/server.py)                      │
│  ├─ BatteryWebSocketServer — asyncio + websockets       │
│  ├─ AlertManager           — threshold-based alerts     │
│  ├─ FrontendHTTPHandler    — serves web/ on :8080       │
│  └─ SimulationEngine       — fallback if C++ not built  │
└────────────────┬────────────────────────────────────────┘
                 │ pybind11 (battery_core.pyd)
┌────────────────▼────────────────────────────────────────┐
│  C++ Engine  (src/battery_engine.cpp)                   │
│  ├─ WMI          — BatteryStatus, BatteryStaticData     │
│  ├─ DeviceIoControl — IOCTL_BATTERY_QUERY_STATUS/INFO  │
│  ├─ SetupAPI     — Device enumeration                   │
│  ├─ PDH          — Performance counters                 │
│  ├─ PowerSetActiveScheme — Power plan management        │
│  └─ RegisterPowerSettingNotification — Live events      │
└─────────────────────────────────────────────────────────┘
```

## Build

### Prerequisites
- Windows 10/11 (required for WinAPI battery subsystem)
- Visual Studio 2022 with C++ workload
- Python 3.9+ with pip
- CMake 3.18+

### 1. Install Python dependencies
```bat
pip install websockets pybind11
```

### 2. Build C++ module
```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
copy build\Release\battery_core.pyd python\
```

### 3. Run server
```bat
cd python
python server.py
```

### 4. Open frontend
Navigate to `http://localhost:8080` in your browser.

> **Note**: The frontend runs in demo mode with simulated data if the
> WebSocket server is not available. All charts and 3D visualization
> remain fully functional.

---

## Windows APIs Used

| API | Purpose |
|-----|---------|
| `IWbemServices::ExecQuery` | WMI BatteryStatus, BatteryStaticData |
| `DeviceIoControl` | `IOCTL_BATTERY_QUERY_STATUS`, `IOCTL_BATTERY_QUERY_INFORMATION`, `IOCTL_BATTERY_QUERY_TAG` |
| `SetupDiGetClassDevs` | Battery device enumeration |
| `GetSystemPowerStatus` | Rapid AC/battery/percent poll |
| `PdhOpenQuery / PdhCollectQueryData` | Performance counter monitoring |
| `PowerEnumerate / PowerSetActiveScheme` | Power plan management |
| `RegisterPowerSettingNotification` | `GUID_BATTERY_PERCENTAGE_REMAINING`, `GUID_ACDC_POWER_SOURCE` |

---

## Features

**Telemetry**
- Voltage (V), Current (A), Power (W), Temperature (°C)
- State of Charge (OS% and accurate mWh-based)
- Capacity now / Full / Design (mWh)
- Charge/Discharge rate, C-Rate

**Analytics**
- Battery health % and wear level
- Internal resistance estimation (Peukert model)
- Thermal runaway risk index (0–100)
- Charging efficiency, energy accumulation
- Time-to-full / time-to-empty prediction
- Cycle count

**Visualization**
- Realtime power and voltage waveforms
- V-I correlation scatter plot
- Temperature trend with warning threshold
- Capacity trend
- Discharge rate histogram
- Battery health donut chart
- Multi-dimensional radar chart
- **3D battery cell model** (Three.js) with:
  - Live charge fill animation
  - Thermal heat mapping per-cell
  - Charge ion particle field
  - Mouse orbit + zoom

**Alerts**
- 7 configurable alert rules
- Active alert banner and badge
- Alert history with timestamps
- WebSocket push delivery

**Export**
- CSV export of telemetry buffer (300 samples)
