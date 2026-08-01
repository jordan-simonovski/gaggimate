#ifndef BLESCALEPLUGIN_H
#define BLESCALEPLUGIN_H
#include "../core/Plugin.h"
#include "remote_scales.h"
#include "remote_scales_plugin_registry.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

void on_ble_measurement(float value);

constexpr unsigned long UPDATE_INTERVAL_MS = 1000;
constexpr unsigned int RECONNECTION_TRIES = 15;
// How often the dedicated scale task wakes. The scale itself pushes weight via
// notifications; this cadence only bounds how quickly a connect/reconnect and
// the driver heartbeat are serviced.
constexpr unsigned long SERVICE_INTERVAL_MS = 50;

class BLEScalePlugin : public Plugin {
  public:
    BLEScalePlugin();
    ~BLEScalePlugin();

    void setup(Controller *controller, PluginManager *pluginManager) override;
    // No-op: the scale runs on its own task (see serviceTask). Servicing it from
    // the shared Arduino loop meant a blocking OTA check, an OTA flash or a slow
    // web request stalled the driver heartbeat for seconds, which stalls the
    // weight stream the volumetric targets depend on.
    void loop() override {}

    void connect(const std::string &uuid);
    void scan() const;
    void disconnect();
    void onMeasurement(float value) const;
    bool isConnected() { return scale != nullptr && scale->isConnected(); };
    std::string getName() {
        if (scale != nullptr && scale->isConnected()) {
            return scale->getDeviceName();
        }
        return "";
    };
    std::string getUUID() {
        if (scale != nullptr && scale->isConnected()) {
            return scale->getDeviceAddress();
        }
        return "";
    };
    int getRSSI() {
        if (scale != nullptr && scale->isConnected()) {
            return scale->getRSSI();
        }
        return 0;
    };

    std::vector<DiscoveredDevice> getDiscoveredScales() const;
    void tare() const;

    // Accessors for the native scale fields that drivers optionally expose
    // (see RemoteScales). Each returns a sentinel value if not supported.
    float getFlowRate() const { return scale != nullptr && scale->hasFlowRate() ? scale->getFlowRate() : 0.0f; }
    bool hasFlowRate() const { return scale != nullptr && scale->hasFlowRate(); }
    uint8_t getBatteryLevel() const {
        return scale != nullptr && scale->hasBatteryLevel() ? scale->getBatteryLevel() : REMOTE_SCALES_BATTERY_UNKNOWN;
    }
    bool hasBatteryLevel() const { return scale != nullptr && scale->hasBatteryLevel(); }
    ScaleWeightUnit getWeightUnit() const {
        return scale != nullptr && scale->hasWeightUnit() ? scale->getWeightUnit() : ScaleWeightUnit::UNKNOWN;
    }
    bool hasWeightUnit() const { return scale != nullptr && scale->hasWeightUnit(); }
    uint32_t getScaleTimerMs() const { return scale != nullptr && scale->hasScaleTimer() ? scale->getScaleTimerMs() : 0; }
    bool hasScaleTimer() const { return scale != nullptr && scale->hasScaleTimer(); }

  private:
    void update();
    void onProcessStart() const;
    void pollScaleMetadata();

    void establishConnection();

    // The `scale` pointer is created/destroyed by the service task but
    // disconnect()/tare() are also driven from event handlers on other tasks.
    // scaleMutex serialises those lifecycle transitions; the *Locked variants
    // assume the caller already holds it. Plain getters (isConnected, getName,
    // ...) stay unguarded - they were never guarded and only ever read.
    SemaphoreHandle_t scaleMutex = nullptr;
    // Bounded by default: the service task can hold the mutex across a blocking
    // NimBLE connect, and the callers here run on the brew-logic and UI tasks,
    // which must never stall behind it.
    static constexpr TickType_t SCALE_LOCK_WAIT = pdMS_TO_TICKS(250);
    bool lockScale(TickType_t wait = SCALE_LOCK_WAIT) const {
        return scaleMutex == nullptr || xSemaphoreTake(scaleMutex, wait) == pdTRUE;
    }
    void unlockScale() const {
        if (scaleMutex)
            xSemaphoreGive(scaleMutex);
    }
    // Set when disconnect() couldn't take the mutex; the service task performs
    // the disconnect on its next tick instead of the caller blocking for it.
    volatile bool disconnectPending = false;
    void disconnectLocked();
    void connectLocked(const std::string &uuid);
    void onProcessStartLocked() const;
    void serviceLoop();
    static void serviceTask(void *arg);
    TaskHandle_t taskHandle = nullptr;

    bool active = false;
    bool doConnect = false;
    std::string uuid;

    unsigned long lastUpdate = 0;
    unsigned int reconnectionTries = 0;

    // Cached scale-metadata values used to avoid firing an event for each
    // unchanged poll tick. Reset when the scale disconnects.
    uint8_t lastBatteryLevel = REMOTE_SCALES_BATTERY_UNKNOWN;
    ScaleWeightUnit lastWeightUnit = ScaleWeightUnit::UNKNOWN;

    // Latch so the mid-brew oz warning + volumetric abort fires once per
    // transition into ounces, not once per sample at ~10 Hz. Reset on
    // disconnect and when the unit returns to grams.
    mutable bool warnedOunceMidBrew = false;

    // Rate limiting for callbacks
    mutable unsigned long lastMeasurementTime = 0;
    static constexpr unsigned long MIN_MEASUREMENT_INTERVAL_MS = 10; // Max 100 measurements per second

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    RemoteScalesPluginRegistry *pluginRegistry = nullptr;
    RemoteScalesScanner *scanner = nullptr;
    std::unique_ptr<RemoteScales> scale = nullptr;
};

extern BLEScalePlugin BLEScales;

#endif // BLESCALEPLUGIN_H
