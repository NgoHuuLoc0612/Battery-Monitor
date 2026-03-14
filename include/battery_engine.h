/**
 * Battery Monitor Engine - Header
 * Enterprise-grade Windows Battery Subsystem Interface
 */

#pragma once

#include <windows.h>
#include <wbemidl.h>
#include <pdh.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <array>

namespace BatteryMonitor {

constexpr size_t HISTORY_BUFFER_SIZE    = 300;   // 5 minutes @ 1Hz
constexpr size_t MAX_TELEMETRY_SAMPLES  = 3600;  // 1 hour @ 1Hz

// ─── Data Structures ──────────────────────────────────────────────────────────

struct BatterySnapshot {
    long long  timestamp             = 0;      // ms since epoch
    bool       acOnline              = false;
    bool       charging              = false;
    bool       discharging           = false;
    bool       batteryPresent        = false;

    float      percentRemaining      = 0.0f;   // OS reported %
    float      socAccurate           = 0.0f;   // Calculated from capacity
    float      voltage               = 0.0f;   // V
    float      currentNow            = 0.0f;   // A (negative = discharging)
    float      powerNow              = 0.0f;   // W
    float      temperatureCelsius    = 0.0f;   // °C
    float      capacityNow           = 0.0f;   // mWh
    float      fullChargeCapacity    = 0.0f;   // mWh
    float      designCapacity        = 0.0f;   // mWh

    float      healthPercent         = 0.0f;   // %
    float      wearLevel             = 0.0f;   // %
    int        cycleCount            = 0;
    float      internalResistance    = 0.0f;   // mΩ
    float      cRate                 = 0.0f;   // C
    float      thermalRiskIndex      = 0.0f;   // 0–100

    float      estimatedMinutesToFull  = 0.0f;
    float      estimatedMinutesToEmpty = 0.0f;
    float      chargingEfficiency    = 0.0f;   // %
    float      energyAccumulated     = 0.0f;   // Wh (session total)
    float      chargeRate            = 0.0f;   // mW
    float      dischargeRate         = 0.0f;   // mW

    std::string chemistry;
    std::string deviceName;
    std::string manufacturer;
    std::string serialNumber;
    int        remainingSeconds      = 0;
    int        fullLifeSeconds       = 0;
};

struct BatteryHealthReport {
    float       currentHealth            = 0.0f;
    float       wearLevel                = 0.0f;
    int         cycleCount               = 0;
    float       internalResistance       = 0.0f;
    float       thermalRiskIndex         = 0.0f;
    float       avgPowerDraw             = 0.0f;
    float       maxTemperature           = 0.0f;
    float       minTemperature           = 0.0f;
    int         estimatedRemainingCycles = 0;
    float       estimatedRemainingLife   = 0.0f;  // days
    std::string grade;
    std::vector<std::string> anomalies;
};

struct PowerPlan {
    GUID        guid;
    std::string name;
    bool        isActive = false;
};

struct StaticBatteryData {
    float       designCapacity     = 0.0f;
    float       fullChargeCapacity = 0.0f;
    std::string chemistry;
    std::string deviceName;
    std::string manufacturer;
    std::string serialNumber;
};

// ─── Engine Class ─────────────────────────────────────────────────────────────

class BatteryEngine {
public:
    BatteryEngine();
    ~BatteryEngine();

    // Lifecycle
    HRESULT Initialize();
    void    Shutdown();

    // Core acquisition
    BatterySnapshot AcquireSnapshot();
    BatterySnapshot GetLatestSnapshot();

    // Monitoring
    void StartMonitoring(int intervalMs = 1000);
    void StopMonitoring();
    void RegisterSnapshotCallback(std::function<void(const BatterySnapshot&)> cb);

    // Analytics
    BatteryHealthReport GenerateHealthReport();
    std::vector<float>  GetVoltageHistory() const;
    std::vector<float>  GetPowerHistory() const;
    std::vector<float>  GetTemperatureHistory() const;
    const std::vector<BatterySnapshot>& GetTelemetryBuffer() const { return m_telemetryBuffer; }

    // Power management
    std::vector<PowerPlan> EnumeratePowerPlans();
    bool SetPowerPlan(const GUID& planGuid);

    // Windows notifications
    void RegisterPowerNotifications(HWND hwnd);
    void UnregisterPowerNotifications();

    // Serialization
    static std::string SnapshotToJSON(const BatterySnapshot& snap);
    static std::string HealthReportToJSON(const BatteryHealthReport& report);

    bool IsInitialized() const { return m_initialized; }

private:
    // WMI
    IWbemLocator*   m_wmiLocator  = nullptr;
    IWbemServices*  m_wmiService  = nullptr;
    HRESULT InitializeWMI();

    // PDH
    PDH_HQUERY               m_pdhQuery = nullptr;
    std::vector<PDH_HCOUNTER> m_pdhCounters;
    HRESULT InitializePDH();

    // Device handle (raw IOCTL)
    HANDLE m_batteryHandle = INVALID_HANDLE_VALUE;
    HANDLE OpenBatteryDevice();
    ULONG  GetBatteryTag();

    // Data acquisition paths
    void AcquireIOCTLData(BatterySnapshot& snap);
    void AcquireWMIStatus(BatterySnapshot& snap);
    void AcquireWMIStaticData(BatterySnapshot& snap);

    // Computation
    void  ComputeDerivedMetrics(BatterySnapshot& snap);
    void  UpdateHistoryBuffers(const BatterySnapshot& snap);
    float EstimateNominalVoltage(const std::string& chemistry);
    float ComputeThermalRisk(const BatterySnapshot& snap);

    // Ring buffers
    std::array<float, HISTORY_BUFFER_SIZE> m_voltageHistory{};
    std::array<float, HISTORY_BUFFER_SIZE> m_currentHistory{};
    std::array<float, HISTORY_BUFFER_SIZE> m_tempHistory{};
    std::array<float, HISTORY_BUFFER_SIZE> m_powerHistory{};
    size_t m_sampleIndex = 0;

    // Telemetry
    std::vector<BatterySnapshot> m_telemetryBuffer;

    // Static cache
    StaticBatteryData m_cachedStatic;
    bool m_staticDataCached = false;

    // Monitoring thread
    std::thread  m_monitorThread;
    std::mutex   m_snapshotMutex;
    std::atomic<bool> m_monitoringActive{false};
    int          m_monitorInterval = 1000;
    BatterySnapshot m_latestSnapshot;
    std::function<void(const BatterySnapshot&)> m_snapshotCallback;

    // Power notifications
    HPOWERNOTIFY m_powerNotifyHandle = nullptr;

    // Flags
    bool m_initialized = false;
};

} // namespace BatteryMonitor
