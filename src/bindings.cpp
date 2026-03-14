/**
 * Battery Monitor - pybind11 Bindings
 * Exposes the C++ BatteryEngine to Python with full feature parity
 * Thread-safe GIL-aware callback integration
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/chrono.h>
#include "../include/battery_engine.h"

namespace py = pybind11;
using namespace BatteryMonitor;

// ─── Python-friendly snapshot dict helper ─────────────────────────────────────

py::dict SnapshotToDict(const BatterySnapshot& s) {
    py::dict d;
    d["timestamp"]              = s.timestamp;
    d["ac_online"]              = s.acOnline;
    d["charging"]               = s.charging;
    d["discharging"]            = s.discharging;
    d["battery_present"]        = s.batteryPresent;
    d["percent_remaining"]      = s.percentRemaining;
    d["soc_accurate"]           = s.socAccurate;
    d["voltage"]                = s.voltage;
    d["current_now"]            = s.currentNow;
    d["power_now"]              = s.powerNow;
    d["temperature_celsius"]    = s.temperatureCelsius;
    d["capacity_now"]           = s.capacityNow;
    d["full_charge_capacity"]   = s.fullChargeCapacity;
    d["design_capacity"]        = s.designCapacity;
    d["health_percent"]         = s.healthPercent;
    d["wear_level"]             = s.wearLevel;
    d["cycle_count"]            = s.cycleCount;
    d["internal_resistance"]    = s.internalResistance;
    d["c_rate"]                 = s.cRate;
    d["thermal_risk_index"]     = s.thermalRiskIndex;
    d["minutes_to_full"]        = s.estimatedMinutesToFull;
    d["minutes_to_empty"]       = s.estimatedMinutesToEmpty;
    d["charging_efficiency"]    = s.chargingEfficiency;
    d["energy_accumulated"]     = s.energyAccumulated;
    d["charge_rate"]            = s.chargeRate;
    d["discharge_rate"]         = s.dischargeRate;
    d["chemistry"]              = s.chemistry;
    d["device_name"]            = s.deviceName;
    d["manufacturer"]           = s.manufacturer;
    d["serial_number"]          = s.serialNumber;
    d["remaining_seconds"]      = s.remainingSeconds;
    return d;
}

py::dict HealthReportToDict(const BatteryHealthReport& r) {
    py::dict d;
    d["current_health"]             = r.currentHealth;
    d["wear_level"]                 = r.wearLevel;
    d["cycle_count"]                = r.cycleCount;
    d["internal_resistance"]        = r.internalResistance;
    d["thermal_risk_index"]         = r.thermalRiskIndex;
    d["avg_power_draw"]             = r.avgPowerDraw;
    d["max_temperature"]            = r.maxTemperature;
    d["min_temperature"]            = r.minTemperature;
    d["estimated_remaining_cycles"] = r.estimatedRemainingCycles;
    d["estimated_remaining_life"]   = r.estimatedRemainingLife;
    d["grade"]                      = r.grade;
    d["anomalies"]                  = r.anomalies;
    return d;
}

// ─── GIL-safe callback wrapper ─────────────────────────────────────────────────

class PyBatteryEngine {
public:
    PyBatteryEngine() {
        m_engine = std::make_unique<BatteryEngine>();
    }

    bool initialize() {
        HRESULT hr = m_engine->Initialize();
        return SUCCEEDED(hr);
    }

    void shutdown() { m_engine->Shutdown(); }

    py::dict acquire_snapshot() {
        auto snap = m_engine->AcquireSnapshot();
        return SnapshotToDict(snap);
    }

    py::dict get_latest_snapshot() {
        auto snap = m_engine->GetLatestSnapshot();
        return SnapshotToDict(snap);
    }

    void start_monitoring(int interval_ms = 1000) {
        m_engine->StartMonitoring(interval_ms);
    }

    void stop_monitoring() {
        m_engine->StopMonitoring();
    }

    // GIL-aware Python callback registration
    void register_callback(py::object callback) {
        m_engine->RegisterSnapshotCallback([callback](const BatterySnapshot& snap) {
            py::gil_scoped_acquire acquire;
            try {
                callback(SnapshotToDict(snap));
            } catch (const py::error_already_set& e) {
                // Swallow callback errors to prevent thread crash
            }
        });
    }

    py::dict generate_health_report() {
        auto report = m_engine->GenerateHealthReport();
        return HealthReportToDict(report);
    }

    std::vector<float> get_voltage_history() {
        return m_engine->GetVoltageHistory();
    }

    std::vector<float> get_power_history() {
        return m_engine->GetPowerHistory();
    }

    std::vector<float> get_temperature_history() {
        return m_engine->GetTemperatureHistory();
    }

    py::list get_telemetry_buffer(int max_samples = 300) {
        const auto& buf = m_engine->GetTelemetryBuffer();
        py::list result;
        int start = std::max(0, (int)buf.size() - max_samples);
        for (int i = start; i < (int)buf.size(); ++i)
            result.append(SnapshotToDict(buf[i]));
        return result;
    }

    std::string snapshot_to_json() {
        auto snap = m_engine->AcquireSnapshot();
        return BatteryEngine::SnapshotToJSON(snap);
    }

    std::string health_report_to_json() {
        auto report = m_engine->GenerateHealthReport();
        return BatteryEngine::HealthReportToJSON(report);
    }

    bool is_initialized() const { return m_engine->IsInitialized(); }

private:
    std::unique_ptr<BatteryEngine> m_engine;
};

// ─── Module Definition ─────────────────────────────────────────────────────────

PYBIND11_MODULE(battery_core, m) {
    m.doc() = "Battery Monitor Core - Enterprise C++ Engine via pybind11";

    py::class_<PyBatteryEngine>(m, "BatteryEngine")
        .def(py::init<>())
        .def("initialize",            &PyBatteryEngine::initialize,
             "Initialize WMI, PDH, and device handles. Returns True on success.")
        .def("shutdown",              &PyBatteryEngine::shutdown,
             "Release all resources.")
        .def("acquire_snapshot",      &PyBatteryEngine::acquire_snapshot,
             "Acquire a full battery snapshot synchronously.")
        .def("get_latest_snapshot",   &PyBatteryEngine::get_latest_snapshot,
             "Get most recent snapshot from monitoring thread.")
        .def("start_monitoring",      &PyBatteryEngine::start_monitoring,
             py::arg("interval_ms") = 1000,
             "Start background monitoring thread.")
        .def("stop_monitoring",       &PyBatteryEngine::stop_monitoring,
             "Stop background monitoring thread.")
        .def("register_callback",     &PyBatteryEngine::register_callback,
             py::arg("callback"),
             "Register Python callable invoked on each new snapshot (GIL-safe).")
        .def("generate_health_report",&PyBatteryEngine::generate_health_report,
             "Generate comprehensive battery health report.")
        .def("get_voltage_history",   &PyBatteryEngine::get_voltage_history,
             "Get voltage ring buffer (300 samples).")
        .def("get_power_history",     &PyBatteryEngine::get_power_history,
             "Get power ring buffer (300 samples).")
        .def("get_temperature_history",&PyBatteryEngine::get_temperature_history,
             "Get temperature ring buffer (300 samples).")
        .def("get_telemetry_buffer",  &PyBatteryEngine::get_telemetry_buffer,
             py::arg("max_samples") = 300,
             "Get list of snapshot dicts from telemetry buffer.")
        .def("snapshot_to_json",      &PyBatteryEngine::snapshot_to_json,
             "Acquire snapshot and return as JSON string.")
        .def("health_report_to_json", &PyBatteryEngine::health_report_to_json,
             "Generate health report and return as JSON string.")
        .def("is_initialized",        &PyBatteryEngine::is_initialized);

    m.def("version", []() { return "1.0.0-enterprise"; });
}
