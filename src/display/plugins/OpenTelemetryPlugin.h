#pragma once
#ifndef OPENTELEMETRYPLUGIN_H
#define OPENTELEMETRYPLUGIN_H

#include "../core/Plugin.h"
#include <Arduino.h>
#include <OtlpEncoder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <vector>

class Controller;
class PluginManager;
class Event;
class HTTPClient;

// Ships GaggiMate coffee telemetry to an OTLP/HTTP collector:
//   * metrics  - periodic gauges (boiler temp/pressure, pump flow, weight, ...)
//   * traces   - one span per shot (duration, peak pressure/flow, final weight)
//
// Network IO (blocking TLS POSTs) runs on a dedicated FreeRTOS task fed from the
// event thread via a cached snapshot + a span queue, so the 50ms main loop is
// never stalled. Enable/endpoint/headers come from Settings; the plugin is only
// registered when metrics or traces are enabled (see Controller::setup).
class OpenTelemetryPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override {}

  private:
    struct Snapshot {
        float temp = 0.0f;
        float targetTemp = 0.0f;
        float pressure = 0.0f;
        float pumpFlow = 0.0f;
        float puckFlow = 0.0f;
        float puckResistance = 0.0f;
        bool puckResistanceValid = false;
        float weight = 0.0f;
        bool haveWeight = false;
    };

    // One coffee-shot phase, captured live and emitted as a child span of the
    // parent "shot" span when the brew ends.
    struct PhaseSpan {
        String name;
        String type;
        int index = 0;
        uint64_t startNanos = 0;
        float peakPressure = 0.0f;
        float peakFlow = 0.0f;
        double pressureSum = 0.0; // running averages over the phase
        uint32_t pressureCount = 0;
        double flowSum = 0.0;
        uint32_t flowCount = 0;
    };

    void onBrewStart();
    void onBrewEnd();
    void onBrewPhase(int index);

    static void exportTaskFn(void *arg);
    void exportLoop();
    void exportMetrics();
    void exportSpan(otel::SpanData *span);
    bool postOtlp(const char *signalPath, const uint8_t *body, size_t len);
    bool sendPost(HTTPClient &http, const uint8_t *body, size_t len) const;
    std::vector<otel::Attribute> buildResourceAttributes() const;
    // Rebuilds the cached resource attributes / scope version on the main thread
    // (reads controller SystemInfo Strings, which the export task must not touch).
    void refreshMetadata();

    void lock() const {
        if (mutex)
            xSemaphoreTake(mutex, portMAX_DELAY);
    }
    void unlock() const {
        if (mutex)
            xSemaphoreGive(mutex);
    }

    Controller *controller = nullptr;

    SemaphoreHandle_t mutex = nullptr;
    Snapshot snapshot;

    // Shot span in progress (guarded by mutex).
    bool shotActive = false;
    uint8_t traceId[16]{};
    uint8_t spanId[8]{};
    uint64_t shotStartNanos = 0;
    unsigned long shotStartMillis = 0;
    float peakPressure = 0.0f;
    float peakFlow = 0.0f;
    float maxWeight = 0.0f;
    float shotTargetTemp = 0.0f;
    float shotGrindLevel = 0.0f;
    float shotDoseWeight = 0.0f;
    bool shotVolumetric = false;
    String shotProfileLabel;
    String shotProfileType;

    // Per-phase slices for the active shot (guarded by mutex). activePhase is an
    // index into phases for the currently-open phase, or -1 if none.
    std::vector<PhaseSpan> phases;
    int activePhase = -1;

    // Shot-level running aggregates (guarded by mutex, reset each shot). Sums use
    // double so accumulation over a few hundred samples stays accurate.
    double pressureSum = 0.0;
    uint32_t pressureCount = 0;
    double pressureErrSum = 0.0; // Σ|measured - target| for profile adherence
    uint32_t pressureErrCount = 0;
    double flowSum = 0.0;
    uint32_t flowCount = 0;
    double flowErrSum = 0.0;
    uint32_t flowErrCount = 0;
    double tempSum = 0.0;
    uint32_t tempCount = 0;
    float tempMin = 0.0f;
    float tempMax = 0.0f;
    double resistanceSum = 0.0;
    double resistanceSumSq = 0.0;
    uint32_t resistanceCount = 0;
    unsigned long firstFlowMillis = 0; // 0 until puck flow first crosses threshold

    static constexpr float FIRST_FLOW_THRESHOLD_MLS = 0.5f;
    // Puck resistance is only physical during extraction (order 1-100). The
    // firmware reports 1e7 / INFINITY as a "no estimate" sentinel; reject
    // anything non-finite or above this bound so it never reaches telemetry.
    static constexpr float RESISTANCE_SANE_MAX = 1.0e6f;

    // Cumulative monotonic counters since boot (guarded by mutex). Exported as
    // OTLP Sums alongside the gauges. metricsStartNanos is the counter epoch.
    uint64_t shotsTotal = 0;
    double brewSecondsTotal = 0.0;
    double waterTotalMl = 0.0;
    uint64_t wifiDisconnectsTotal = 0;
    uint64_t bleDisconnectsTotal = 0;
    unsigned long lastFlowMillis = 0; // for trapezoidal water integration
    uint64_t metricsStartNanos = 0;

    // Trace ids of the most recently completed shot, attached as exemplars to
    // the shot-driven counters so metrics link back to the shot trace.
    uint8_t lastShotTraceId[16]{};
    uint8_t lastShotSpanId[8]{};
    bool haveLastShot = false;

    QueueHandle_t spanQueue = nullptr; // holds otel::SpanData*

    bool metricsEnabled = false;
    bool tracesEnabled = false;
    String endpoint;
    String headers;
    int intervalS = 10;

    // Cached identity, refreshed on the main thread; copied under lock by the
    // export task so it never reads controller-owned Strings cross-thread.
    std::vector<otel::Attribute> resourceAttrs;
    String scopeVersion;

    TaskHandle_t taskHandle = nullptr;
    uint8_t *buffer = nullptr;

    static constexpr size_t BUFFER_SIZE = 8192;
};

#endif // OPENTELEMETRYPLUGIN_H
