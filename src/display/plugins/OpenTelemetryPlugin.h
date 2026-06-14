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
    bool shotVolumetric = false;
    String shotProfileLabel;
    String shotProfileType;

    // Per-phase slices for the active shot (guarded by mutex). activePhase is an
    // index into phases for the currently-open phase, or -1 if none.
    std::vector<PhaseSpan> phases;
    int activePhase = -1;

    QueueHandle_t spanQueue = nullptr; // holds otel::SpanData*

    bool metricsEnabled = false;
    bool tracesEnabled = false;
    String endpoint;
    String headers;
    int intervalS = 10;

    TaskHandle_t taskHandle = nullptr;
    uint8_t *buffer = nullptr;

    static constexpr size_t BUFFER_SIZE = 8192;
};

#endif // OPENTELEMETRYPLUGIN_H
