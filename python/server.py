"""
Battery Monitor - Python Server Layer
Bridges C++ BatteryEngine to HTML frontend via WebSocket + HTTP
Handles: real-time telemetry streaming, REST API, alert management, data export
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import sys
import time
import threading
import csv
import io
from datetime import datetime
from pathlib import Path
from typing import Any

# ── Try to import C++ engine; fall back to simulation if not built ─────────────
try:
    import battery_core
    _ENGINE_AVAILABLE = True
except ImportError:
    _ENGINE_AVAILABLE = False
    logging.warning("battery_core module not found – running in simulation mode")

import websockets
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

# ─── Logging ───────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("battery_monitor.log", encoding="utf-8"),
    ]
)
log = logging.getLogger("BatteryMonitor")


# ─── Simulation Engine (fallback when C++ not compiled) ───────────────────────

class SimulationEngine:
    """Provides realistic simulated battery data for development/testing."""

    def __init__(self):
        self._start = time.time()
        self._cycle = 0
        self._health = 87.3
        self._capacity = 45200.0
        self._design = 52000.0
        self._lock = threading.Lock()
        self._telemetry: list[dict] = []
        self._running = False
        self._callback = None

    def initialize(self) -> bool:
        return True

    def shutdown(self):
        self.stop_monitoring()

    def acquire_snapshot(self) -> dict:
        t = time.time() - self._start
        # Simulate realistic discharge/charge cycle
        cycle_pos = (t % 7200) / 7200.0  # 2hr cycle
        charging = cycle_pos > 0.6

        soc = (1.0 - cycle_pos) * 100 if not charging else (cycle_pos - 0.6) / 0.4 * 100
        soc = max(5.0, min(100.0, soc))

        voltage = 3.2 + (soc / 100) * 1.0 + (0.05 * __import__('math').sin(t * 0.1))
        current = 2.1 if charging else -1.85
        temp = 28.5 + abs(current) * 2.0 + 2.0 * __import__('math').sin(t * 0.05)
        power = abs(voltage * current)
        cap_now = (soc / 100) * self._capacity

        snap = {
            "timestamp": int(time.time() * 1000),
            "ac_online": charging,
            "charging": charging,
            "discharging": not charging,
            "battery_present": True,
            "percent_remaining": round(soc, 1),
            "soc_accurate": round(soc, 2),
            "voltage": round(voltage, 4),
            "current_now": round(current, 4),
            "power_now": round(power, 3),
            "temperature_celsius": round(temp, 2),
            "capacity_now": round(cap_now, 1),
            "full_charge_capacity": self._capacity,
            "design_capacity": self._design,
            "health_percent": self._health,
            "wear_level": round(100 - self._health, 2),
            "cycle_count": 412,
            "internal_resistance": 85.4,
            "c_rate": round(power * 1000 / self._design, 3),
            "thermal_risk_index": round(max(0, (temp - 35) * 3), 1),
            "minutes_to_full": round((100 - soc) / 0.5) if charging else 0,
            "minutes_to_empty": round(soc / 0.55) if not charging else 0,
            "charging_efficiency": 92.4 if charging else 0,
            "energy_accumulated": round(t * power / 3600, 3),
            "charge_rate": round(power * 1000 if charging else 0, 1),
            "discharge_rate": round(power * 1000 if not charging else 0, 1),
            "chemistry": "LION",
            "device_name": "DELL Battery",
            "manufacturer": "SMP",
            "serial_number": "7B4F2A91",
            "remaining_seconds": int(soc * 60 / 0.55) if not charging else 0,
        }
        with self._lock:
            self._telemetry.append(snap)
            if len(self._telemetry) > 3600:
                self._telemetry.pop(0)
        return snap

    def get_latest_snapshot(self) -> dict:
        return self.acquire_snapshot()

    def start_monitoring(self, interval_ms: int = 1000):
        self._running = True
        def _loop():
            while self._running:
                snap = self.acquire_snapshot()
                if self._callback:
                    self._callback(snap)
                time.sleep(interval_ms / 1000.0)
        threading.Thread(target=_loop, daemon=True).start()

    def stop_monitoring(self):
        self._running = False

    def register_callback(self, cb):
        self._callback = cb

    def generate_health_report(self) -> dict:
        return {
            "current_health": self._health,
            "wear_level": round(100 - self._health, 2),
            "cycle_count": 412,
            "internal_resistance": 85.4,
            "thermal_risk_index": 12.5,
            "avg_power_draw": 8.2,
            "max_temperature": 38.1,
            "min_temperature": 24.3,
            "estimated_remaining_cycles": 638,
            "estimated_remaining_life": 638.0,
            "grade": "Good",
            "anomalies": [],
        }

    def get_voltage_history(self) -> list[float]:
        return [s["voltage"] for s in self._telemetry[-300:]]

    def get_power_history(self) -> list[float]:
        return [s["power_now"] for s in self._telemetry[-300:]]

    def get_temperature_history(self) -> list[float]:
        return [s["temperature_celsius"] for s in self._telemetry[-300:]]

    def get_telemetry_buffer(self, max_samples: int = 300) -> list[dict]:
        return self._telemetry[-max_samples:]

    def is_initialized(self) -> bool:
        return True


# ─── Alert Manager ────────────────────────────────────────────────────────────

class AlertManager:
    THRESHOLDS = {
        "low_battery":       {"field": "percent_remaining", "op": "lt", "value": 20,   "severity": "warning"},
        "critical_battery":  {"field": "percent_remaining", "op": "lt", "value": 5,    "severity": "critical"},
        "high_temperature":  {"field": "temperature_celsius", "op": "gt", "value": 45, "severity": "warning"},
        "critical_temp":     {"field": "temperature_celsius", "op": "gt", "value": 55, "severity": "critical"},
        "thermal_risk":      {"field": "thermal_risk_index", "op": "gt", "value": 70,  "severity": "critical"},
        "high_resistance":   {"field": "internal_resistance", "op": "gt", "value": 250,"severity": "warning"},
        "low_health":        {"field": "health_percent",      "op": "lt", "value": 60, "severity": "warning"},
    }

    def __init__(self):
        self._active_alerts: dict[str, dict] = {}
        self._history: list[dict] = []

    def evaluate(self, snap: dict) -> list[dict]:
        new_alerts = []
        for name, rule in self.THRESHOLDS.items():
            val = snap.get(rule["field"], 0)
            triggered = (
                (rule["op"] == "lt" and val < rule["value"]) or
                (rule["op"] == "gt" and val > rule["value"])
            )
            if triggered and name not in self._active_alerts:
                alert = {
                    "id": name,
                    "severity": rule["severity"],
                    "message": self._format_message(name, val, rule),
                    "timestamp": snap["timestamp"],
                    "value": val,
                }
                self._active_alerts[name] = alert
                self._history.append(alert)
                new_alerts.append(alert)
            elif not triggered and name in self._active_alerts:
                del self._active_alerts[name]
        return new_alerts

    @staticmethod
    def _format_message(name: str, val: float, rule: dict) -> str:
        msgs = {
            "low_battery":      f"Battery low: {val:.1f}%",
            "critical_battery": f"Battery critical: {val:.1f}% – connect charger",
            "high_temperature": f"Temperature elevated: {val:.1f}°C",
            "critical_temp":    f"CRITICAL: Temperature {val:.1f}°C – check cooling",
            "thermal_risk":     f"Thermal risk index high: {val:.1f}/100",
            "high_resistance":  f"Internal resistance high: {val:.1f}mΩ – battery aging",
            "low_health":       f"Battery health degraded: {val:.1f}%",
        }
        return msgs.get(name, f"{name}: {val}")

    def get_active(self) -> list[dict]:
        return list(self._active_alerts.values())

    def get_history(self, limit: int = 50) -> list[dict]:
        return self._history[-limit:]


# ─── WebSocket Server ─────────────────────────────────────────────────────────

class BatteryWebSocketServer:
    def __init__(self, host: str = "localhost", port: int = 8765):
        self._host = host
        self._port = port
        self._clients: set = set()
        self._clients_lock = asyncio.Lock()
        self._engine = (
            battery_core.BatteryEngine() if _ENGINE_AVAILABLE
            else SimulationEngine()
        )
        self._alerts = AlertManager()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._last_snapshot: dict = {}

    async def _register(self, ws):
        async with self._clients_lock:
            self._clients.add(ws)
        log.info(f"Client connected: {ws.remote_address}")

    async def _unregister(self, ws):
        async with self._clients_lock:
            self._clients.discard(ws)
        log.info(f"Client disconnected: {ws.remote_address}")

    async def _broadcast(self, message: str):
        async with self._clients_lock:
            dead = set()
            for ws in self._clients:
                try:
                    await ws.send(message)
                except websockets.exceptions.ConnectionClosed:
                    dead.add(ws)
            self._clients -= dead

    async def _handle_client(self, ws):
        await self._register(ws)
        try:
            # Send initial snapshot immediately
            snap = self._engine.acquire_snapshot()
            await ws.send(json.dumps({"type": "snapshot", "data": snap}))

            # Send health report
            report = self._engine.generate_health_report()
            await ws.send(json.dumps({"type": "health_report", "data": report}))

            async for message in ws:
                await self._handle_message(ws, message)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            await self._unregister(ws)

    async def _handle_message(self, ws, raw: str):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            await ws.send(json.dumps({"type": "error", "message": "Invalid JSON"}))
            return

        cmd = msg.get("cmd", "")
        log.debug(f"Command received: {cmd}")

        if cmd == "get_snapshot":
            snap = self._engine.acquire_snapshot()
            await ws.send(json.dumps({"type": "snapshot", "data": snap}))

        elif cmd == "get_health_report":
            report = self._engine.generate_health_report()
            await ws.send(json.dumps({"type": "health_report", "data": report}))

        elif cmd == "get_telemetry":
            samples = msg.get("samples", 300)
            buf = self._engine.get_telemetry_buffer(samples)
            await ws.send(json.dumps({"type": "telemetry", "data": list(buf)}))

        elif cmd == "get_history":
            kind = msg.get("kind", "voltage")
            if kind == "voltage":
                hist = self._engine.get_voltage_history()
            elif kind == "power":
                hist = self._engine.get_power_history()
            elif kind == "temperature":
                hist = self._engine.get_temperature_history()
            else:
                hist = []
            await ws.send(json.dumps({"type": "history", "kind": kind, "data": list(hist)}))

        elif cmd == "get_alerts":
            await ws.send(json.dumps({
                "type": "alerts",
                "active": self._alerts.get_active(),
                "history": self._alerts.get_history()
            }))

        elif cmd == "export_csv":
            samples = msg.get("samples", 300)
            buf = list(self._engine.get_telemetry_buffer(samples))
            csv_data = self._telemetry_to_csv(buf)
            await ws.send(json.dumps({"type": "export_csv", "data": csv_data}))

        elif cmd == "ping":
            await ws.send(json.dumps({"type": "pong", "ts": int(time.time() * 1000)}))

    def _telemetry_to_csv(self, telemetry: list[dict]) -> str:
        if not telemetry:
            return ""
        out = io.StringIO()
        writer = csv.DictWriter(out, fieldnames=list(telemetry[0].keys()))
        writer.writeheader()
        writer.writerows(telemetry)
        return out.getvalue()

    def _on_snapshot(self, snap: dict):
        """Called from C++ monitoring thread – schedule broadcast on event loop."""
        self._last_snapshot = snap
        new_alerts = self._alerts.evaluate(snap)
        payload = {"type": "snapshot", "data": snap}
        if new_alerts:
            payload["alerts"] = new_alerts

        if self._loop and not self._loop.is_closed():
            asyncio.run_coroutine_threadsafe(
                self._broadcast(json.dumps(payload)),
                self._loop
            )

    async def start(self):
        self._loop = asyncio.get_running_loop()

        ok = self._engine.initialize()
        if not ok:
            log.error("Engine initialization failed – running in degraded mode")

        self._engine.register_callback(self._on_snapshot)
        self._engine.start_monitoring(1000)
        log.info("Engine monitoring started (1Hz)")

        log.info(f"WebSocket server listening on ws://{self._host}:{self._port}")
        async with websockets.serve(self._handle_client, self._host, self._port):
            await asyncio.Future()  # run forever

    def stop(self):
        self._engine.stop_monitoring()
        self._engine.shutdown()


# ─── HTTP Server (serves frontend) ───────────────────────────────────────────

class FrontendHTTPHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(Path(__file__).parent / "web"), **kwargs)

    def log_message(self, fmt, *args):
        pass  # suppress per-request logging

    def do_GET(self):
        parsed = urlparse(self.path)
        # API passthrough
        if parsed.path.startswith("/api/"):
            self._handle_api(parsed)
            return
        super().do_GET()

    def _handle_api(self, parsed):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"status": "ok"}).encode())


def start_http_server(port: int = 8080):
    web_dir = Path(__file__).parent / "web"
    os.chdir(web_dir)
    server = HTTPServer(("localhost", port), FrontendHTTPHandler)
    log.info(f"HTTP server: http://localhost:{port}")
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


# ─── Entry Point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    http_server = start_http_server(8080)
    ws_server   = BatteryWebSocketServer("localhost", 8765)
    try:
        asyncio.run(ws_server.start())
    except KeyboardInterrupt:
        log.info("Shutting down...")
        ws_server.stop()
        http_server.shutdown()