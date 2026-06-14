#include "OpenTelemetryPlugin.h"

#include "../core/Controller.h"
#include "../core/process/BrewProcess.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <ctime>
#include <new>
#include <sys/time.h>

// mbedTLS root-cert bundle baked into the firmware (CONFIG_MBEDTLS_CERTIFICATE_
// BUNDLE_DEFAULT_CMN). Same symbol the OTA client uses for HTTPS.
extern const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");

static const char *OTEL_TAG = "OpenTelemetry";
static constexpr const char *SCOPE_NAME = "gaggimate";

static uint64_t nowUnixNanos() {
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000000ULL + static_cast<uint64_t>(tv.tv_usec) * 1000ULL;
}

// SNTP has set the wall clock (post 2020-09). Until then unix-nanos timestamps
// would be garbage, so we hold off exporting.
static bool clockValid() { return time(nullptr) > 1600000000L; }

void OpenTelemetryPlugin::setup(Controller *ctrl, PluginManager *pluginManager) {
    controller = ctrl;
    Settings &settings = controller->getSettings();
    metricsEnabled = settings.isOtelMetrics();
    tracesEnabled = settings.isOtelTraces();
    endpoint = settings.getOtelEndpoint();
    headers = settings.getOtelHeaders();
    intervalS = settings.getOtelInterval();
    if (intervalS < 1)
        intervalS = 1;

    if (!metricsEnabled && !tracesEnabled)
        return;

    mutex = xSemaphoreCreateMutex();
    // Sized for a parent shot span plus one child span per phase.
    spanQueue = xQueueCreate(16, sizeof(otel::SpanData *));
    buffer = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM));
    if (buffer == nullptr)
        buffer = static_cast<uint8_t *>(malloc(BUFFER_SIZE));
    if (mutex == nullptr || buffer == nullptr) {
        ESP_LOGE(OTEL_TAG, "Failed to allocate resources; OTel disabled");
        return;
    }

    auto floatField = [this](float Snapshot::*field) {
        return [this, field](Event &event) {
            const float v = event.getFloat("value");
            lock();
            snapshot.*field = v;
            unlock();
        };
    };

    pluginManager->on("boiler:currentTemperature:change", floatField(&Snapshot::temp));
    pluginManager->on("boiler:targetTemperature:change", floatField(&Snapshot::targetTemp));
    pluginManager->on("pump:puck-flow:change", floatField(&Snapshot::puckFlow));
    pluginManager->on("pump:puck-resistance:change", floatField(&Snapshot::puckResistance));

    pluginManager->on("boiler:pressure:change", [this](Event &event) {
        const float v = event.getFloat("value");
        lock();
        snapshot.pressure = v;
        if (shotActive && v > peakPressure)
            peakPressure = v;
        if (activePhase >= 0 && v > phases[activePhase].peakPressure)
            phases[activePhase].peakPressure = v;
        unlock();
    });
    pluginManager->on("pump:flow:change", [this](Event &event) {
        const float v = event.getFloat("value");
        lock();
        snapshot.pumpFlow = v;
        if (shotActive && v > peakFlow)
            peakFlow = v;
        if (activePhase >= 0 && v > phases[activePhase].peakFlow)
            phases[activePhase].peakFlow = v;
        unlock();
    });

    auto weightHandler = [this](Event &event) {
        const float v = event.getFloat("value");
        lock();
        snapshot.weight = v;
        snapshot.haveWeight = true;
        if (shotActive && v > maxWeight)
            maxWeight = v;
        unlock();
    };
    pluginManager->on("controller:volumetric-measurement:bluetooth:change", weightHandler);
    pluginManager->on("controller:volumetric-measurement:estimation:change", weightHandler);

    if (tracesEnabled) {
        pluginManager->on("controller:brew:start", [this](Event &) { onBrewStart(); });
        pluginManager->on("controller:brew:end", [this](Event &) { onBrewEnd(); });
        pluginManager->on("controller:brew:phase", [this](Event &event) { onBrewPhase(event.getInt("index")); });
    }

    // Pin to core 1 so the blocking TLS handshakes stay off core 0 (WiFi MAC,
    // LWIP, AsyncTCP). 12KB stack covers mbedTLS.
    xTaskCreatePinnedToCore(exportTaskFn, "OtelExport", 12288, this, 1, &taskHandle, 1);
    ESP_LOGI(OTEL_TAG, "OpenTelemetry exporter started (metrics=%d traces=%d endpoint=%s)", metricsEnabled, tracesEnabled,
             endpoint.c_str());
}

void OpenTelemetryPlugin::onBrewStart() {
    Process *process = controller->getProcess();
    if (process != nullptr && process->getType() == MODE_BREW) {
        if (static_cast<BrewProcess *>(process)->isUtility())
            return; // flush / utility shots are not real espresso shots
    }
    lock();
    shotActive = true;
    esp_fill_random(traceId, sizeof(traceId));
    esp_fill_random(spanId, sizeof(spanId));
    shotStartNanos = nowUnixNanos();
    shotStartMillis = millis();
    peakPressure = 0.0f;
    peakFlow = 0.0f;
    maxWeight = 0.0f;
    phases.clear();
    activePhase = -1;
    Profile &profile = controller->getProfileManager()->getSelectedProfile();
    shotProfileLabel = profile.label;
    shotProfileType = profile.type;
    shotVolumetric = profile.isVolumetric();
    shotTargetTemp = controller->getTargetTemp();
    unlock();
}

void OpenTelemetryPlugin::onBrewPhase(int index) {
    const uint64_t now = nowUnixNanos();
    lock();
    if (!shotActive) {
        unlock();
        return;
    }
    String name;
    String type;
    Process *process = controller->getProcess();
    if (process != nullptr && process->getType() == MODE_BREW) {
        const Phase &phase = static_cast<BrewProcess *>(process)->currentPhase;
        name = phase.name;
        type = phase.phase == PhaseType::PHASE_TYPE_PREINFUSION ? "preinfusion" : "brew";
    }
    PhaseSpan ps;
    ps.name = name.isEmpty() ? String("phase") : name;
    ps.type = type;
    ps.index = index;
    ps.startNanos = now;
    phases.push_back(ps);
    activePhase = static_cast<int>(phases.size()) - 1;
    unlock();
}

void OpenTelemetryPlugin::onBrewEnd() {
    const uint64_t endNanos = nowUnixNanos();
    lock();
    if (!shotActive) {
        unlock();
        return;
    }
    shotActive = false;
    activePhase = -1;
    const uint64_t startNanos = shotStartNanos;
    const unsigned long durationMs = millis() - shotStartMillis;
    const float pp = peakPressure;
    const float pf = peakFlow;
    const float mw = maxWeight;
    const float tt = shotTargetTemp;
    const bool vol = shotVolumetric;
    const String label = shotProfileLabel;
    const String type = shotProfileType;
    uint8_t tid[16];
    uint8_t sid[8];
    memcpy(tid, traceId, sizeof(tid));
    memcpy(sid, spanId, sizeof(sid));
    std::vector<PhaseSpan> phaseCopy = phases;
    phases.clear();
    unlock();

    if (!clockValid() || startNanos == 0)
        return;

    auto *span = new (std::nothrow) otel::SpanData();
    if (span == nullptr)
        return;
    memcpy(span->traceId, tid, sizeof(tid));
    memcpy(span->spanId, sid, sizeof(sid));
    span->name = "shot";
    span->startNanos = startNanos;
    span->endNanos = endNanos;
    span->statusCode = 1; // OK
    span->attributes.push_back(otel::Attribute::str("coffee.profile.name", label));
    span->attributes.push_back(otel::Attribute::str("coffee.profile.type", type));
    span->attributes.push_back(otel::Attribute::integer("coffee.shot.duration_ms", static_cast<int64_t>(durationMs)));
    span->attributes.push_back(otel::Attribute::dbl("coffee.shot.peak_pressure_bar", pp));
    span->attributes.push_back(otel::Attribute::dbl("coffee.shot.peak_flow_mls", pf));
    span->attributes.push_back(otel::Attribute::dbl("coffee.shot.final_weight_g", mw));
    span->attributes.push_back(otel::Attribute::boolean("coffee.shot.volumetric", vol));
    span->attributes.push_back(otel::Attribute::dbl("coffee.shot.target_temperature_c", tt));

    if (xQueueSend(spanQueue, &span, 0) != pdTRUE) {
        delete span; // queue full; drop rather than block the brew thread
        return;
    }

    // Child span per phase. A phase ends where the next one starts; the last
    // phase ends with the shot. Same trace_id, parent = the shot span.
    for (size_t i = 0; i < phaseCopy.size(); i++) {
        const PhaseSpan &ph = phaseCopy[i];
        const uint64_t phaseEnd = (i + 1 < phaseCopy.size()) ? phaseCopy[i + 1].startNanos : endNanos;
        if (phaseEnd < ph.startNanos)
            continue;

        auto *child = new (std::nothrow) otel::SpanData();
        if (child == nullptr)
            return;
        memcpy(child->traceId, tid, sizeof(tid));
        esp_fill_random(child->spanId, sizeof(child->spanId));
        memcpy(child->parentSpanId, sid, sizeof(sid));
        child->hasParent = true;
        child->name = ph.name;
        child->startNanos = ph.startNanos;
        child->endNanos = phaseEnd;
        child->statusCode = 1; // OK
        child->attributes.push_back(otel::Attribute::integer("coffee.phase.index", static_cast<int64_t>(ph.index)));
        if (!ph.type.isEmpty())
            child->attributes.push_back(otel::Attribute::str("coffee.phase.type", ph.type));
        child->attributes.push_back(otel::Attribute::dbl("coffee.phase.peak_pressure_bar", ph.peakPressure));
        child->attributes.push_back(otel::Attribute::dbl("coffee.phase.peak_flow_mls", ph.peakFlow));
        child->attributes.push_back(otel::Attribute::integer(
            "coffee.phase.duration_ms", static_cast<int64_t>((phaseEnd - ph.startNanos) / 1000000ULL)));

        if (xQueueSend(spanQueue, &child, 0) != pdTRUE) {
            delete child; // queue full; drop the rest
            return;
        }
    }
}

void OpenTelemetryPlugin::exportTaskFn(void *arg) { static_cast<OpenTelemetryPlugin *>(arg)->exportLoop(); }

void OpenTelemetryPlugin::exportLoop() {
    unsigned long lastMetric = 0;
    for (;;) {
        if (tracesEnabled) {
            otel::SpanData *span = nullptr;
            while (xQueueReceive(spanQueue, &span, 0) == pdTRUE) {
                if (span != nullptr) {
                    exportSpan(span);
                    delete span;
                }
            }
        }
        if (metricsEnabled) {
            const unsigned long now = millis();
            if (now - lastMetric >= static_cast<unsigned long>(intervalS) * 1000UL) {
                lastMetric = now;
                exportMetrics();
            }
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

std::vector<otel::Attribute> OpenTelemetryPlugin::buildResourceAttributes() const {
    std::vector<otel::Attribute> attrs;
    attrs.push_back(otel::Attribute::str("service.name", SCOPE_NAME));
    attrs.push_back(otel::Attribute::str("service.instance.id", WiFi.macAddress()));
    const SystemInfo info = controller->getSystemInfo();
    if (info.version.length() > 0)
        attrs.push_back(otel::Attribute::str("service.version", info.version));
    if (info.hardware.length() > 0)
        attrs.push_back(otel::Attribute::str("host.type", info.hardware));
    return attrs;
}

void OpenTelemetryPlugin::exportMetrics() {
    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGW(OTEL_TAG, "metrics skipped: WiFi not connected");
        return;
    }
    if (!clockValid()) {
        ESP_LOGW(OTEL_TAG, "metrics skipped: clock not NTP-synced yet");
        return;
    }

    Snapshot s;
    lock();
    s = snapshot;
    unlock();

    std::vector<otel::MetricPoint> metrics;
    metrics.push_back({"coffee.boiler.temperature", "Cel", s.temp});
    metrics.push_back({"coffee.boiler.target_temperature", "Cel", s.targetTemp});
    metrics.push_back({"coffee.boiler.pressure", "bar", s.pressure});
    metrics.push_back({"coffee.pump.flow", "ml/s", s.pumpFlow});
    metrics.push_back({"coffee.pump.puck_flow", "ml/s", s.puckFlow});
    metrics.push_back({"coffee.pump.puck_resistance", "", s.puckResistance});
    if (s.haveWeight)
        metrics.push_back({"coffee.scale.weight", "g", s.weight});

    const String version = controller->getSystemInfo().version;
    const size_t len =
        otel::OtlpEncoder::encodeMetrics(buildResourceAttributes(), SCOPE_NAME, version, metrics, nowUnixNanos(), buffer,
                                         BUFFER_SIZE);
    if (len == 0) {
        ESP_LOGW(OTEL_TAG, "Failed to encode metrics");
        return;
    }
    postOtlp("/v1/metrics", buffer, len);
}

void OpenTelemetryPlugin::exportSpan(otel::SpanData *span) {
    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGW(OTEL_TAG, "span skipped: WiFi not connected");
        return;
    }
    const String version = controller->getSystemInfo().version;
    const size_t len =
        otel::OtlpEncoder::encodeTrace(buildResourceAttributes(), SCOPE_NAME, version, *span, buffer, BUFFER_SIZE);
    if (len == 0) {
        ESP_LOGW(OTEL_TAG, "Failed to encode span");
        return;
    }
    postOtlp("/v1/traces", buffer, len);
}

bool OpenTelemetryPlugin::sendPost(HTTPClient &http, const uint8_t *body, size_t len) const {
    http.addHeader("Content-Type", "application/x-protobuf");

    // Custom headers: one "Key: Value" per line (e.g. auth tokens).
    int start = 0;
    while (start < static_cast<int>(headers.length())) {
        int nl = headers.indexOf('\n', start);
        String line = (nl == -1) ? headers.substring(start) : headers.substring(start, nl);
        line.trim();
        if (line.length() > 0) {
            const int colon = line.indexOf(':');
            if (colon > 0) {
                String key = line.substring(0, colon);
                String value = line.substring(colon + 1);
                key.trim();
                value.trim();
                if (key.length() > 0)
                    http.addHeader(key, value);
            }
        }
        if (nl == -1)
            break;
        start = nl + 1;
    }

    const int code = http.POST(const_cast<uint8_t *>(body), len);
    if (code < 200 || code >= 300) {
        const String resp = http.getString();
        ESP_LOGW(OTEL_TAG, "OTLP POST failed: HTTP %d, body: %s", code, resp.c_str());
        return false;
    }
    ESP_LOGI(OTEL_TAG, "OTLP POST ok: HTTP %d", code);
    return true;
}

bool OpenTelemetryPlugin::postOtlp(const char *signalPath, const uint8_t *body, size_t len) {
    String base = endpoint;
    base.trim();
    while (base.endsWith("/"))
        base.remove(base.length() - 1);
    if (base.isEmpty()) {
        ESP_LOGW(OTEL_TAG, "OTLP skipped: endpoint not configured");
        return false;
    }
    const String url = base + signalPath;
    ESP_LOGI(OTEL_TAG, "OTLP POST -> %s (%u bytes)", url.c_str(), static_cast<unsigned>(len));

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(5000);
    http.setTimeout(8000);

    bool ok = false;
    if (url.startsWith("https://")) {
        WiFiClientSecure client;
        client.setCACertBundle(x509_crt_imported_bundle_bin_start);
        if (http.begin(client, url)) {
            ok = sendPost(http, body, len);
            http.end();
        } else {
            ESP_LOGW(OTEL_TAG, "OTLP begin() failed for %s", url.c_str());
        }
    } else {
        WiFiClient client;
        if (http.begin(client, url)) {
            ok = sendPost(http, body, len);
            http.end();
        } else {
            ESP_LOGW(OTEL_TAG, "OTLP begin() failed for %s", url.c_str());
        }
    }
    return ok;
}
