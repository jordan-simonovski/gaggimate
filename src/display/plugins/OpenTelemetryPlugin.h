#pragma once
#ifndef OPENTELEMETRYPLUGIN_H
#define OPENTELEMETRYPLUGIN_H

#include "../core/Plugin.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <OtlpEncoder.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <vector>

class Controller;
class PluginManager;
class Event;

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
        // millis() of the last scale reading; 0 until one arrives. Exported as
        // coffee.scale.sample_age_ms so a laggy/stalled BLE link is visible in
        // telemetry instead of only being felt on the screen.
        unsigned long weightMillis = 0;
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

    // One captured instant of the live gauges plus the shot identity at sample
    // time. The export task samples these into a ring at ~500ms and ships the
    // batch every intervalS, so a fast-moving signal (scale weight especially)
    // keeps its shape without extra HTTP round-trips.
    struct GaugeSample {
        uint64_t timeNanos = 0;
        unsigned long captureMillis = 0; // for age-of-reading style gauges
        Snapshot snap;
        bool inShot = false;
        uint8_t traceId[16] = {};
        uint8_t spanId[8] = {};
        float grindLevel = 0.0f;
        char phase[24] = {}; // phase name at sample time, empty when none
    };

    void onBrewStart();
    void onBrewEnd();
    void onBrewPhase(int index);
    // A rating saved against a stored shot, emitted as a span on that shot's
    // trace (parented to the shot span) so taste notes join the shot's data.
    void onShotRated(Event &event);

    static void exportTaskFn(void *arg);
    void exportLoop();
    void captureSample();
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
    PluginManager *plugins = nullptr;

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

    // Max timestamped points batched per gauge per export. This may exceed
    // otlp.Gauge.data_points (8) because encodeMetrics splits a batch across
    // several ResourceMetrics messages; the per-message cap is derived from the
    // generated struct, not from this constant. The sampler derives its cadence
    // from this so the ring is never exceeded for any export interval.
    static constexpr size_t MAX_GAUGE_POINTS = 20;
    // Floor for the internal sample cadence. Finer than this buys little for the
    // signals we track and just inflates payloads.
    static constexpr unsigned long SAMPLE_INTERVAL_FLOOR_MS = 500;

    bool metricsEnabled = false;
    bool tracesEnabled = false;
    String endpoint;
    String headers;
    int intervalS = 10;
    // Live gauges are sampled every sampleIntervalMs into sampleRing and flushed
    // every intervalS. The ring is owned by the export task (sampled + flushed
    // there), so it needs no lock; only the shared snapshot read is guarded.
    unsigned long sampleIntervalMs = SAMPLE_INTERVAL_FLOOR_MS;
    GaugeSample sampleRing[MAX_GAUGE_POINTS];
    size_t sampleCount = 0;

    // Cached identity, refreshed on the main thread; copied under lock by the
    // export task so it never reads controller-owned Strings cross-thread.
    std::vector<otel::Attribute> resourceAttrs;
    String scopeVersion;

    // Owned by the export task only (created on first POST, never touched from
    // another thread). Kept alive across exports so a steady-state export is a
    // POST on an already-open socket: no DNS, no TCP connect, no TLS handshake.
    // That matters far more than it looks - resolving a .local endpoint runs
    // inside lwIP's tcpip thread, which is the single thread servicing all
    // networking, so a slow lookup every interval froze the web server too.
    HTTPClient *http = nullptr;
    WiFiClient *plainClient = nullptr;
    WiFiClientSecure *tlsClient = nullptr;
    String clientBase; // endpoint the persistent client was built for
    // Consecutive failures -> back off, so an unreachable collector costs one
    // connect attempt per backoff window instead of one per export interval.
    unsigned int exportFailures = 0;
    unsigned long nextAttemptMillis = 0;
    static constexpr unsigned long MAX_BACKOFF_MS = 300000; // 5 minutes
    void releaseHttpClient();
    // Drops the connection, advances the backoff. Every failure path must go
    // through this, or that path retries at the full export cadence.
    void recordExportFailure(const char *reason);
    bool exportBackedOff() const { return nextAttemptMillis != 0 && static_cast<long>(millis() - nextAttemptMillis) < 0; }

    TaskHandle_t taskHandle = nullptr;
    uint8_t *buffer = nullptr;

    // Sized for the worst case: MAX_GAUGE_POINTS in-shot points across every
    // gauge, each carrying 3 attributes + an exemplar (~200B/point on the wire).
    // PSRAM-backed; falls back to internal heap if PSRAM is unavailable.
    static constexpr size_t BUFFER_SIZE = 40960;
};

#endif // OPENTELEMETRYPLUGIN_H
