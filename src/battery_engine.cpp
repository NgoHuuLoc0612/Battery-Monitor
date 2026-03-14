/**
 * Battery Monitor Engine - Enterprise C++ Core
 * Windows Battery Subsystem Interface via WMI, PDH, SetupAPI, DeviceIoControl
 * Real-time telemetry, health analytics, thermal modeling, predictive algorithms
 */

#include "../include/battery_engine.h"
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#include <devguid.h>
#include <setupapi.h>
#include <batclass.h>
#include <powrprof.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
#include <ntddndis.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <poclass.h>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <stdexcept>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "pdh.lib")

namespace BatteryMonitor {

// ─── Constructor / Destructor ─────────────────────────────────────────────────

BatteryEngine::BatteryEngine()
    : m_wmiService(nullptr)
    , m_wmiLocator(nullptr)
    , m_pdhQuery(nullptr)
    , m_initialized(false)
    , m_monitoringActive(false)
    , m_sampleIndex(0)
{
    m_telemetryBuffer.reserve(MAX_TELEMETRY_SAMPLES);
}

BatteryEngine::~BatteryEngine() {
    StopMonitoring();
    Shutdown();
}

// ─── Initialization ───────────────────────────────────────────────────────────

HRESULT BatteryEngine::Initialize() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;

    hr = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr
    );
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) return hr;

    hr = InitializeWMI();
    if (FAILED(hr)) return hr;

    hr = InitializePDH();
    if (FAILED(hr)) {
        // PDH non-critical, continue
        m_pdhQuery = nullptr;
    }

    // Open raw battery device handle
    m_batteryHandle = OpenBatteryDevice();
    if (m_batteryHandle == INVALID_HANDLE_VALUE) {
        m_batteryHandle = nullptr;
    }

    // Seed history ring buffer
    for (auto& v : m_voltageHistory) v = 0.0f;
    for (auto& c : m_currentHistory) c = 0.0f;
    for (auto& t : m_tempHistory)    t = 0.0f;
    for (auto& p : m_powerHistory)   p = 0.0f;

    m_initialized = true;
    return S_OK;
}

HRESULT BatteryEngine::InitializeWMI() {
    HRESULT hr = CoCreateInstance(
        CLSID_WbemLocator, nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        reinterpret_cast<void**>(&m_wmiLocator)
    );
    if (FAILED(hr)) return hr;

    hr = m_wmiLocator->ConnectServer(
        _bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &m_wmiService
    );
    if (FAILED(hr)) return hr;

    hr = CoSetProxyBlanket(
        m_wmiService,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE
    );
    return hr;
}

HRESULT BatteryEngine::InitializePDH() {
    PDH_STATUS status = PdhOpenQuery(nullptr, 0, &m_pdhQuery);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    // Add battery counters
    const wchar_t* counters[] = {
        L"\\Battery(*)\\ Capacity",
        L"\\Battery(*)\\ Charge Rate",
        L"\\Battery(*)\\ Discharge Rate"
    };

    for (auto* counter : counters) {
        PDH_HCOUNTER hCounter;
        PdhAddCounter(m_pdhQuery, counter, 0, &hCounter);
        m_pdhCounters.push_back(hCounter);
    }

    PdhCollectQueryData(m_pdhQuery);
    return S_OK;
}

void BatteryEngine::Shutdown() {
    if (m_pdhQuery) {
        PdhCloseQuery(m_pdhQuery);
        m_pdhQuery = nullptr;
    }
    if (m_batteryHandle && m_batteryHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_batteryHandle);
        m_batteryHandle = nullptr;
    }
    if (m_wmiService) { m_wmiService->Release(); m_wmiService = nullptr; }
    if (m_wmiLocator) { m_wmiLocator->Release(); m_wmiLocator = nullptr; }
    m_initialized = false;
}

// ─── Device Access ────────────────────────────────────────────────────────────

HANDLE BatteryEngine::OpenBatteryDevice() {
    HDEVINFO devInfo = SetupDiGetClassDevs(
        &GUID_DEVCLASS_BATTERY,
        nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVCLASS_BATTERY, 0, &ifData)) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, nullptr, 0, &required, nullptr);

    auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(new BYTE[required]);
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    HANDLE hDevice = INVALID_HANDLE_VALUE;
    if (SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detail, required, nullptr, nullptr)) {
        hDevice = CreateFile(
            detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
        );
    }

    delete[] reinterpret_cast<BYTE*>(detail);
    SetupDiDestroyDeviceInfoList(devInfo);
    return hDevice;
}

// ─── Core Data Acquisition ────────────────────────────────────────────────────

BatterySnapshot BatteryEngine::AcquireSnapshot() {
    BatterySnapshot snap = {};
    snap.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // --- Windows SYSTEM_POWER_STATUS (fastest path) ---
    SYSTEM_POWER_STATUS sps = {};
    if (GetSystemPowerStatus(&sps)) {
        snap.acOnline        = (sps.ACLineStatus == 1);
        snap.batteryPresent  = (sps.BatteryFlag != 128); // 128 = no battery
        snap.charging        = (sps.BatteryFlag & 8) != 0;
        snap.discharging     = !(snap.charging) && !snap.acOnline;
        snap.percentRemaining = (sps.BatteryLifePercent == 255) ? -1.0f
                                : static_cast<float>(sps.BatteryLifePercent);
        snap.remainingSeconds = sps.BatteryLifeTime;
        snap.fullLifeSeconds  = sps.BatteryFullLifeTime;
    }

    // --- Raw IOCTL path for voltage/current/capacity ---
    if (m_batteryHandle && m_batteryHandle != INVALID_HANDLE_VALUE) {
        AcquireIOCTLData(snap);
    }

    // --- WMI BatteryStatus for thermal + extended ---
    AcquireWMIStatus(snap);

    // --- WMI BatteryStaticData for design capacity, chemistry ---
    // m_staticDataCached is set to true inside AcquireWMIStaticData only when
    // valid capacity data was obtained, preventing stale zero-value caching.
    if (m_staticDataCached == false) {
        AcquireWMIStaticData(snap);
    } else {
        snap.designCapacity   = m_cachedStatic.designCapacity;
        snap.fullChargeCapacity = m_cachedStatic.fullChargeCapacity;
        snap.chemistry        = m_cachedStatic.chemistry;
        snap.deviceName       = m_cachedStatic.deviceName;
        snap.manufacturer     = m_cachedStatic.manufacturer;
        snap.serialNumber     = m_cachedStatic.serialNumber;
    }

    // --- Derived metrics ---
    ComputeDerivedMetrics(snap);

    // --- Push into ring buffers ---
    UpdateHistoryBuffers(snap);

    return snap;
}

void BatteryEngine::AcquireIOCTLData(BatterySnapshot& snap) {
    BATTERY_QUERY_INFORMATION bqi = {};
    bqi.BatteryTag = GetBatteryTag();
    if (bqi.BatteryTag == 0) return;

    // Query status
    BATTERY_WAIT_STATUS bws = {};
    bws.BatteryTag = bqi.BatteryTag;
    BATTERY_STATUS bst = {};
    DWORD bytesReturned = 0;

    if (DeviceIoControl(
            m_batteryHandle,
            IOCTL_BATTERY_QUERY_STATUS,
            &bws, sizeof(bws),
            &bst, sizeof(bst),
            &bytesReturned, nullptr))
    {
        snap.voltage        = bst.Voltage / 1000.0f;       // mV → V
        snap.currentNow     = bst.Rate / 1000.0f;          // mW → W (signed)
        snap.capacityNow    = static_cast<float>(bst.Capacity); // mWh
        snap.powerNow       = std::abs(snap.voltage * (snap.currentNow / snap.voltage));
    }

    // Query information (design + full capacity)
    bqi.InformationLevel = BatteryInformation;
    BATTERY_INFORMATION bi = {};
    if (DeviceIoControl(
            m_batteryHandle,
            IOCTL_BATTERY_QUERY_INFORMATION,
            &bqi, sizeof(bqi),
            &bi, sizeof(bi),
            &bytesReturned, nullptr))
    {
        snap.designCapacity     = static_cast<float>(bi.DesignedCapacity);
        snap.fullChargeCapacity = static_cast<float>(bi.FullChargedCapacity);
        snap.cycleCount         = bi.CycleCount;
        // Chemistry string
        char chem[5] = {};
        memcpy(chem, bi.Chemistry, 4);
        snap.chemistry = std::string(chem);
    }

    // Query estimated time
    bqi.InformationLevel = BatteryEstimatedTime;
    ULONG estimatedTime = 0;
    if (DeviceIoControl(
            m_batteryHandle,
            IOCTL_BATTERY_QUERY_INFORMATION,
            &bqi, sizeof(bqi),
            &estimatedTime, sizeof(estimatedTime),
            &bytesReturned, nullptr))
    {
        if (estimatedTime != BATTERY_UNKNOWN_TIME) {
            snap.remainingSeconds = static_cast<int>(estimatedTime);
        }
    }
}

ULONG BatteryEngine::GetBatteryTag() {
    ULONG tag = 0;
    ULONG timeout = 0;
    DWORD bytesReturned = 0;
    DeviceIoControl(
        m_batteryHandle,
        IOCTL_BATTERY_QUERY_TAG,
        &timeout, sizeof(timeout),
        &tag, sizeof(tag),
        &bytesReturned, nullptr
    );
    return tag;
}

void BatteryEngine::AcquireWMIStatus(BatterySnapshot& snap) {
    if (!m_wmiService) return;

    // ── Helper: query a single WMI class and return one object ───────────────
    auto queryFirst = [&](const wchar_t* wql) -> IWbemClassObject* {
        IEnumWbemClassObject* en = nullptr;
        HRESULT hr = m_wmiService->ExecQuery(
            _bstr_t(L"WQL"), _bstr_t(wql),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &en);
        if (FAILED(hr) || !en) return nullptr;
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        if (en->Next(WBEM_INFINITE, 1, &obj, &ret) != WBEM_S_NO_ERROR) obj = nullptr;
        en->Release();
        return obj;
    };

    // ── Generic typed variant reader ─────────────────────────────────────────
    auto getVariantFloat = [](IWbemClassObject* obj, const wchar_t* prop) -> float {
        VARIANT vt; VariantInit(&vt);
        if (FAILED(obj->Get(prop, 0, &vt, nullptr, nullptr))) return 0.0f;
        float val = 0.0f;
        switch (vt.vt) {
            case VT_I4:  val = static_cast<float>(vt.lVal);   break;
            case VT_UI4: val = static_cast<float>(vt.ulVal);  break;
            case VT_I2:  val = static_cast<float>(vt.iVal);   break;
            case VT_R4:  val = vt.fltVal;                     break;
            case VT_R8:  val = static_cast<float>(vt.dblVal); break;
            default:     val = 0.0f;                          break;
        }
        VariantClear(&vt);
        return val;
    };

    // ── 1. BatteryStatus: voltage, current, capacity, charge/discharge rate ──
    if (IWbemClassObject* obj = queryFirst(L"SELECT * FROM BatteryStatus")) {
        float wmiVoltage       = getVariantFloat(obj, L"Voltage");
        float wmiCurrent       = getVariantFloat(obj, L"Current");
        float wmiTemp          = getVariantFloat(obj, L"Temperature");
        float wmiCapacity      = getVariantFloat(obj, L"RemainingCapacity");
        float wmiChargeRate    = getVariantFloat(obj, L"ChargeRate");
        float wmiDischargeRate = getVariantFloat(obj, L"DischargeRate");
        obj->Release();

        // Voltage: WMI reports mV
        if (wmiVoltage > 0.0f && snap.voltage == 0.0f)
            snap.voltage = wmiVoltage / 1000.0f;

        // Temperature: WMI reports in units of 0.1 Kelvin
        if (wmiTemp > 0.0f)
            snap.temperatureCelsius = (wmiTemp / 10.0f) - 273.15f;

        // Current: WMI reports mA; sign convention: positive = charging
        if (wmiCurrent != 0.0f && snap.currentNow == 0.0f)
            snap.currentNow = wmiCurrent / 1000.0f;

        // Remaining capacity: WMI reports mWh
        if (wmiCapacity > 0.0f && snap.capacityNow == 0.0f)
            snap.capacityNow = wmiCapacity;

        snap.chargeRate    = wmiChargeRate;
        snap.dischargeRate = wmiDischargeRate;
    }

    // ── 2. BatteryTemperature: HP-specific class, more reliable than BatteryStatus.Temperature
    if (snap.temperatureCelsius == 0.0f) {
        if (IWbemClassObject* obj = queryFirst(L"SELECT Temperature FROM BatteryTemperature")) {
            float t = getVariantFloat(obj, L"Temperature");
            obj->Release();
            // Same 0.1K encoding
            if (t > 0.0f)
                snap.temperatureCelsius = (t / 10.0f) - 273.15f;
        }
    }

    // ── 3. BatteryCycleCount: some HP drivers expose this separately
    if (snap.cycleCount == 0) {
        if (IWbemClassObject* obj = queryFirst(L"SELECT CycleCount FROM BatteryCycleCount")) {
            float cc = getVariantFloat(obj, L"CycleCount");
            obj->Release();
            if (cc > 0.0f)
                snap.cycleCount = static_cast<int>(cc);
        }
    }
}

void BatteryEngine::AcquireWMIStaticData(BatterySnapshot& snap) {
    if (!m_wmiService) return;

    // ── Generic variant helpers (local) ──────────────────────────────────────
    auto queryFirst = [&](const wchar_t* wql) -> IWbemClassObject* {
        IEnumWbemClassObject* en = nullptr;
        HRESULT hr = m_wmiService->ExecQuery(
            _bstr_t(L"WQL"), _bstr_t(wql),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &en);
        if (FAILED(hr) || !en) return nullptr;
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        if (en->Next(WBEM_INFINITE, 1, &obj, &ret) != WBEM_S_NO_ERROR) obj = nullptr;
        en->Release();
        return obj;
    };

    auto getStr = [](IWbemClassObject* obj, const wchar_t* prop) -> std::string {
        VARIANT vt; VariantInit(&vt);
        if (SUCCEEDED(obj->Get(prop, 0, &vt, nullptr, nullptr)) && vt.vt == VT_BSTR) {
            std::wstring ws(vt.bstrVal);
            VariantClear(&vt);
            return std::string(ws.begin(), ws.end());
        }
        VariantClear(&vt);
        return "";
    };

    auto getUL = [](IWbemClassObject* obj, const wchar_t* prop) -> ULONG {
        VARIANT vt; VariantInit(&vt);
        if (FAILED(obj->Get(prop, 0, &vt, nullptr, nullptr))) return 0;
        ULONG val = 0;
        switch (vt.vt) {
            case VT_I4:  val = static_cast<ULONG>(vt.lVal);  break;
            case VT_UI4: val = vt.ulVal;                     break;
            case VT_I2:  val = static_cast<ULONG>(vt.iVal);  break;
            default:     val = 0;                            break;
        }
        VariantClear(&vt);
        return val;
    };

    // ── 1. BatteryStaticData: device name, manufacturer, serial, design cap ──
    if (IWbemClassObject* obj = queryFirst(L"SELECT * FROM BatteryStaticData")) {
        snap.deviceName       = getStr(obj, L"DeviceName");
        snap.manufacturer     = getStr(obj, L"ManufactureName");
        snap.serialNumber     = getStr(obj, L"SerialNumber");
        snap.designCapacity   = static_cast<float>(getUL(obj, L"DesignedCapacity"));
        // HP leaves FullChargedCapacity = 0 here; we fill it from a dedicated class below
        ULONG fcc = getUL(obj, L"FullChargedCapacity");
        if (fcc > 0) snap.fullChargeCapacity = static_cast<float>(fcc);
        obj->Release();
    }

    // ── 2. BatteryFullChargedCapacity: HP-specific; authoritative FCC value ──
    if (snap.fullChargeCapacity == 0.0f) {
        if (IWbemClassObject* obj = queryFirst(L"SELECT FullChargedCapacity FROM BatteryFullChargedCapacity")) {
            ULONG fcc = getUL(obj, L"FullChargedCapacity");
            obj->Release();
            if (fcc > 0) snap.fullChargeCapacity = static_cast<float>(fcc);
        }
    }

    // ── 3. BatteryStaticData.ManufactureDate (optional enrichment) ───────────
    // (already captured above; extend here if more fields needed)

    // ── Persist to cache only when we have valid capacity data ───────────────
    // A cache with fullChargeCapacity=0 is worse than no cache at all.
    bool capacityValid = (snap.fullChargeCapacity > 0.0f && snap.designCapacity > 0.0f);
    if (capacityValid) {
        m_cachedStatic.designCapacity     = snap.designCapacity;
        m_cachedStatic.fullChargeCapacity = snap.fullChargeCapacity;
        m_cachedStatic.chemistry          = snap.chemistry;
        m_cachedStatic.deviceName         = snap.deviceName;
        m_cachedStatic.manufacturer       = snap.manufacturer;
        m_cachedStatic.serialNumber       = snap.serialNumber;
        m_staticDataCached = true;
    }
    // If capacity is still missing, leave m_staticDataCached = false so next
    // snapshot retries all WMI queries instead of serving stale zeros.
}

// ─── Derived Metrics & Analytics ─────────────────────────────────────────────

void BatteryEngine::ComputeDerivedMetrics(BatterySnapshot& snap) {
    // Health percentage
    if (snap.designCapacity > 0 && snap.fullChargeCapacity > 0) {
        snap.healthPercent = (snap.fullChargeCapacity / snap.designCapacity) * 100.0f;
        snap.wearLevel     = 100.0f - snap.healthPercent;
    }

    // Power (W)
    if (snap.voltage > 0 && snap.currentNow != 0)
        snap.powerNow = std::abs(snap.voltage * snap.currentNow);

    // State of Charge (true SoC from capacity)
    if (snap.fullChargeCapacity > 0 && snap.capacityNow >= 0)
        snap.socAccurate = (snap.capacityNow / snap.fullChargeCapacity) * 100.0f;

    // Internal resistance estimate (DC-IR method)
    // nominalVoltage from EstimateNominalVoltage() is per-cell.
    // Pack voltage is N cells in series; estimate N by rounding pack/cell_nominal.
    // Then: R_pack = |(V_nominal_pack - V_actual)| / |I|
    if (snap.currentNow != 0 && snap.voltage > 0) {
        float cellNominal = EstimateNominalVoltage(snap.chemistry);
        if (cellNominal > 0) {
            int cellCount = static_cast<int>(std::round(snap.voltage / cellNominal));
            if (cellCount < 1) cellCount = 1;
            float packNominal = cellNominal * static_cast<float>(cellCount);
            // Use signed current: positive=charging, negative=discharging
            // IR drop sign cancels in abs(), result in mΩ
            snap.internalResistance = std::abs((packNominal - snap.voltage) / snap.currentNow) * 1000.0f;
            // Sanity clamp: healthy Li-Ion pack < 500mΩ, degraded < 1500mΩ
            if (snap.internalResistance > 2000.0f) snap.internalResistance = 0.0f; // measurement artifact
        }
    }

    // Time-to-full / time-to-empty predictions
    if (snap.charging && snap.chargeRate > 0 && snap.fullChargeCapacity > snap.capacityNow) {
        float remaining = snap.fullChargeCapacity - snap.capacityNow;
        snap.estimatedMinutesToFull = (remaining / snap.chargeRate) * 60.0f;
    }
    if (!snap.charging && snap.dischargeRate > 0 && snap.capacityNow > 0) {
        snap.estimatedMinutesToEmpty = (snap.capacityNow / snap.dischargeRate) * 60.0f;
    }

    // Efficiency (charging efficiency)
    if (snap.charging && snap.chargeRate > 0 && snap.powerNow > 0)
        snap.chargingEfficiency = (snap.chargeRate / (snap.powerNow * 1000.0f)) * 100.0f;

    // C-Rate
    if (snap.designCapacity > 0 && snap.powerNow > 0)
        snap.cRate = (snap.powerNow * 1000.0f) / snap.designCapacity;

    // Thermal runaway risk index (0–100)
    snap.thermalRiskIndex = ComputeThermalRisk(snap);

    // Energy throughput accumulation
    if (m_telemetryBuffer.size() > 1) {
        const auto& prev = m_telemetryBuffer.back();
        double dtHours = (snap.timestamp - prev.timestamp) / 3600000.0;
        snap.energyAccumulated = prev.energyAccumulated + snap.powerNow * dtHours;
    }
}

float BatteryEngine::EstimateNominalVoltage(const std::string& chemistry) {
    if (chemistry == "LION" || chemistry == "LiIo") return 3.7f;
    if (chemistry == "LIPO" || chemistry == "LiPo") return 3.85f;
    if (chemistry == "NICD") return 1.2f;
    if (chemistry == "NIMH") return 1.25f;
    if (chemistry == "VRLA") return 12.0f;
    return 3.7f; // default Li-Ion
}

float BatteryEngine::ComputeThermalRisk(const BatterySnapshot& snap) {
    float risk = 0.0f;
    if (snap.temperatureCelsius > 0) {
        if (snap.temperatureCelsius > 45.0f) risk += 30.0f;
        if (snap.temperatureCelsius > 55.0f) risk += 40.0f;
        if (snap.temperatureCelsius > 60.0f) risk += 30.0f;
    }
    if (snap.cRate > 2.0f) risk += 20.0f;
    if (snap.internalResistance > 200.0f) risk += 15.0f;
    if (snap.wearLevel > 30.0f) risk += snap.wearLevel * 0.5f;
    return std::min(100.0f, risk);
}

void BatteryEngine::UpdateHistoryBuffers(const BatterySnapshot& snap) {
    size_t idx = m_sampleIndex % HISTORY_BUFFER_SIZE;
    m_voltageHistory[idx] = snap.voltage;
    m_currentHistory[idx] = snap.currentNow;
    m_tempHistory[idx]    = snap.temperatureCelsius;
    m_powerHistory[idx]   = snap.powerNow;
    m_sampleIndex++;

    m_telemetryBuffer.push_back(snap);
    if (m_telemetryBuffer.size() > MAX_TELEMETRY_SAMPLES)
        m_telemetryBuffer.erase(m_telemetryBuffer.begin());
}

// ─── Power Plan Management ────────────────────────────────────────────────────

std::vector<PowerPlan> BatteryEngine::EnumeratePowerPlans() {
    std::vector<PowerPlan> plans;
    GUID* guidScheme = nullptr;
    ULONG idx = 0;

    while (PowerEnumerate(nullptr, nullptr, nullptr,
                          ACCESS_SCHEME, idx++, nullptr, &(ULONG&)(*guidScheme)) == ERROR_SUCCESS)
    {
        PowerPlan plan;
        plan.guid = *guidScheme;

        ULONG nameLen = 256;
        wchar_t nameW[256] = {};
        if (PowerReadFriendlyName(nullptr, guidScheme, nullptr, nullptr,
                                  reinterpret_cast<PUCHAR>(nameW), &nameLen) == ERROR_SUCCESS) {
            std::wstring ws(nameW);
            plan.name = std::string(ws.begin(), ws.end());
        }

        GUID* activePlan = nullptr;
        if (PowerGetActiveScheme(nullptr, &activePlan) == ERROR_SUCCESS) {
            plan.isActive = IsEqualGUID(*activePlan, *guidScheme);
            LocalFree(activePlan);
        }

        plans.push_back(plan);
        LocalFree(guidScheme);
    }
    return plans;
}

bool BatteryEngine::SetPowerPlan(const GUID& planGuid) {
    return PowerSetActiveScheme(nullptr, &planGuid) == ERROR_SUCCESS;
}

// ─── Advanced Analytics ───────────────────────────────────────────────────────

BatteryHealthReport BatteryEngine::GenerateHealthReport() {
    BatteryHealthReport report = {};
    if (m_telemetryBuffer.empty()) return report;

    const auto& latest = m_telemetryBuffer.back();
    report.currentHealth     = latest.healthPercent;
    report.wearLevel         = latest.wearLevel;
    report.cycleCount        = latest.cycleCount;
    report.internalResistance = latest.internalResistance;
    report.thermalRiskIndex  = latest.thermalRiskIndex;

    // Compute average power over session
    float totalPower = 0;
    float maxTemp = 0, minTemp = 999;
    for (const auto& s : m_telemetryBuffer) {
        totalPower += s.powerNow;
        if (s.temperatureCelsius > maxTemp) maxTemp = s.temperatureCelsius;
        if (s.temperatureCelsius > 0 && s.temperatureCelsius < minTemp) minTemp = s.temperatureCelsius;
    }
    report.avgPowerDraw   = totalPower / m_telemetryBuffer.size();
    report.maxTemperature = maxTemp;
    report.minTemperature = (minTemp == 999) ? 0 : minTemp;

    // Degradation projection (linear regression on health)
    if (latest.cycleCount > 0 && latest.wearLevel > 0) {
        // Simplified: assume 0.05% wear per cycle average
        float projectedCycles = (100.0f - latest.wearLevel) / 0.05f;
        report.estimatedRemainingCycles = static_cast<int>(projectedCycles);
        report.estimatedRemainingLife   = projectedCycles / 1.0f; // days if 1 cycle/day
    }

    // Health grade
    if (report.currentHealth >= 90) report.grade = "Excellent";
    else if (report.currentHealth >= 80) report.grade = "Good";
    else if (report.currentHealth >= 70) report.grade = "Fair";
    else if (report.currentHealth >= 60) report.grade = "Poor";
    else report.grade = "Critical";

    // Anomaly flags
    if (latest.thermalRiskIndex > 70) report.anomalies.push_back("HIGH_THERMAL_RISK");
    if (latest.internalResistance > 300) report.anomalies.push_back("HIGH_INTERNAL_RESISTANCE");
    if (latest.wearLevel > 40) report.anomalies.push_back("SIGNIFICANT_CAPACITY_LOSS");
    if (latest.voltage < 3.0f && !latest.charging) report.anomalies.push_back("LOW_VOLTAGE_WARNING");

    return report;
}

std::vector<float> BatteryEngine::GetVoltageHistory() const {
    std::vector<float> result(HISTORY_BUFFER_SIZE);
    size_t start = m_sampleIndex % HISTORY_BUFFER_SIZE;
    for (size_t i = 0; i < HISTORY_BUFFER_SIZE; ++i)
        result[i] = m_voltageHistory[(start + i) % HISTORY_BUFFER_SIZE];
    return result;
}

std::vector<float> BatteryEngine::GetPowerHistory() const {
    std::vector<float> result(HISTORY_BUFFER_SIZE);
    size_t start = m_sampleIndex % HISTORY_BUFFER_SIZE;
    for (size_t i = 0; i < HISTORY_BUFFER_SIZE; ++i)
        result[i] = m_powerHistory[(start + i) % HISTORY_BUFFER_SIZE];
    return result;
}

std::vector<float> BatteryEngine::GetTemperatureHistory() const {
    std::vector<float> result(HISTORY_BUFFER_SIZE);
    size_t start = m_sampleIndex % HISTORY_BUFFER_SIZE;
    for (size_t i = 0; i < HISTORY_BUFFER_SIZE; ++i)
        result[i] = m_tempHistory[(start + i) % HISTORY_BUFFER_SIZE];
    return result;
}

// ─── Monitoring Thread ────────────────────────────────────────────────────────

void BatteryEngine::StartMonitoring(int intervalMs) {
    if (m_monitoringActive) return;
    m_monitoringActive = true;
    m_monitorInterval  = intervalMs;

    m_monitorThread = std::thread([this]() {
        while (m_monitoringActive) {
            auto snap = AcquireSnapshot();
            {
                std::lock_guard<std::mutex> lock(m_snapshotMutex);
                m_latestSnapshot = snap;
            }
            if (m_snapshotCallback) m_snapshotCallback(snap);
            std::this_thread::sleep_for(std::chrono::milliseconds(m_monitorInterval));
        }
    });
}

void BatteryEngine::StopMonitoring() {
    m_monitoringActive = false;
    if (m_monitorThread.joinable()) m_monitorThread.join();
}

BatterySnapshot BatteryEngine::GetLatestSnapshot() {
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    return m_latestSnapshot;
}

void BatteryEngine::RegisterSnapshotCallback(std::function<void(const BatterySnapshot&)> cb) {
    m_snapshotCallback = std::move(cb);
}

// ─── Windows Power Notifications ─────────────────────────────────────────────

void BatteryEngine::RegisterPowerNotifications(HWND hwnd) {
    m_powerNotifyHandle = RegisterPowerSettingNotification(
        hwnd, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE
    );
    RegisterPowerSettingNotification(
        hwnd, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE
    );
    RegisterPowerSettingNotification(
        hwnd, &GUID_BATTERY_SUBGROUP, DEVICE_NOTIFY_WINDOW_HANDLE
    );
}

void BatteryEngine::UnregisterPowerNotifications() {
    if (m_powerNotifyHandle) {
        UnregisterPowerSettingNotification(m_powerNotifyHandle);
        m_powerNotifyHandle = nullptr;
    }
}

// ─── Serialization ────────────────────────────────────────────────────────────

std::string BatteryEngine::SnapshotToJSON(const BatterySnapshot& snap) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{"
        << "\"timestamp\":" << snap.timestamp << ","
        << "\"acOnline\":" << (snap.acOnline ? "true" : "false") << ","
        << "\"charging\":" << (snap.charging ? "true" : "false") << ","
        << "\"discharging\":" << (snap.discharging ? "true" : "false") << ","
        << "\"batteryPresent\":" << (snap.batteryPresent ? "true" : "false") << ","
        << "\"percentRemaining\":" << snap.percentRemaining << ","
        << "\"socAccurate\":" << snap.socAccurate << ","
        << "\"voltage\":" << snap.voltage << ","
        << "\"currentNow\":" << snap.currentNow << ","
        << "\"powerNow\":" << snap.powerNow << ","
        << "\"temperatureCelsius\":" << snap.temperatureCelsius << ","
        << "\"capacityNow\":" << snap.capacityNow << ","
        << "\"fullChargeCapacity\":" << snap.fullChargeCapacity << ","
        << "\"designCapacity\":" << snap.designCapacity << ","
        << "\"healthPercent\":" << snap.healthPercent << ","
        << "\"wearLevel\":" << snap.wearLevel << ","
        << "\"cycleCount\":" << snap.cycleCount << ","
        << "\"internalResistance\":" << snap.internalResistance << ","
        << "\"cRate\":" << snap.cRate << ","
        << "\"thermalRiskIndex\":" << snap.thermalRiskIndex << ","
        << "\"estimatedMinutesToFull\":" << snap.estimatedMinutesToFull << ","
        << "\"estimatedMinutesToEmpty\":" << snap.estimatedMinutesToEmpty << ","
        << "\"chargingEfficiency\":" << snap.chargingEfficiency << ","
        << "\"energyAccumulated\":" << snap.energyAccumulated << ","
        << "\"chargeRate\":" << snap.chargeRate << ","
        << "\"dischargeRate\":" << snap.dischargeRate << ","
        << "\"chemistry\":\"" << snap.chemistry << "\","
        << "\"deviceName\":\"" << snap.deviceName << "\","
        << "\"manufacturer\":\"" << snap.manufacturer << "\","
        << "\"serialNumber\":\"" << snap.serialNumber << "\","
        << "\"remainingSeconds\":" << snap.remainingSeconds
        << "}";
    return oss.str();
}

std::string BatteryEngine::HealthReportToJSON(const BatteryHealthReport& report) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{"
        << "\"currentHealth\":" << report.currentHealth << ","
        << "\"wearLevel\":" << report.wearLevel << ","
        << "\"cycleCount\":" << report.cycleCount << ","
        << "\"internalResistance\":" << report.internalResistance << ","
        << "\"thermalRiskIndex\":" << report.thermalRiskIndex << ","
        << "\"avgPowerDraw\":" << report.avgPowerDraw << ","
        << "\"maxTemperature\":" << report.maxTemperature << ","
        << "\"minTemperature\":" << report.minTemperature << ","
        << "\"estimatedRemainingCycles\":" << report.estimatedRemainingCycles << ","
        << "\"estimatedRemainingLife\":" << report.estimatedRemainingLife << ","
        << "\"grade\":\"" << report.grade << "\","
        << "\"anomalies\":[";
    for (size_t i = 0; i < report.anomalies.size(); ++i) {
        if (i) oss << ",";
        oss << "\"" << report.anomalies[i] << "\"";
    }
    oss << "]}";
    return oss.str();
}

} // namespace BatteryMonitor