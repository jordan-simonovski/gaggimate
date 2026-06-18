#include "OpenTelemetryPlugin.h"

#include "../core/Controller.h"
#include "../core/Grinders.h"
#include "../core/process/BrewProcess.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <cmath>
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

// Lowercase hex of a trace id, matching the value collectors store for the
// span's trace_id. Used as the coffee.shot.id metric attribute so live samples
// can be filtered/joined to the shot trace.
static String traceIdHex(const uint8_t *id, size_t len) {
    static const char digits[] = "0123456789abcdef";
    String out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out += digits[id[i] >> 4];
        out += digits[id[i] & 0x0F];
    }
    return out;
}

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

    pluginManager->on("boiler:targetTemperature:change", floatField(&Snapshot::targetTemp));

    pluginManager->on("boiler:currentTemperature:change", [this](Event &event) {
        const float v = event.getFloat("value");
        lock();
        snapshot.temp = v;
        if (shotActive) {
            if (tempCount == 0) {
                tempMin = v;
                tempMax = v;
            } else if (v < tempMin) {
                tempMin = v;
            } else if (v > tempMax) {
                tempMax = v;
            }
            tempSum += v;
            tempCount++;
        }
        unlock();
    });
    pluginManager->on("pump:puck-flow:change", [this](Event &event) {
        const float v = event.getFloat("value");
        lock();
        snapshot.puckFlow = v;
        if (shotActive && firstFlowMillis == 0 && v >= FIRST_FLOW_THRESHOLD_MLS)
            firstFlowMillis = millis() - shotStartMillis;
        unlock();
    });
    pluginManager->on("pump:puck-resistance:change", [this](Event &event) {
        const float v = event.getFloat("value");
        const bool valid = std::isfinite(v) && v > 0.0f && v < RESISTANCE_SANE_MAX;
        lock();
        snapshot.puckResistanceValid = valid;
        if (valid) {
            snapshot.puckResistance = v;
            if (shotActive) {
                resistanceSum += v;
                resistanceSumSq += static_cast<double>(v) * v;
                resistanceCount++;
            }
        }
        unlock();
    });

    pluginManager->on("boiler:pressure:change", [this](Event &event) {
        const float v = event.getFloat("value");
        const float target = controller->getTargetPressure();
        lock();
        snapshot.pressure = v;
        if (shotActive) {
            if (v > peakPressure)
                peakPressure = v;
            pressureSum += v;
            pressureCount++;
            if (target > 0.0f) { // only score adherence in pressure-targeted phases
                pressureErrSum += fabsf(v - target);
                pressureErrCount++;
            }
        }
        if (activePhase >= 0) {
            if (v > phases[activePhase].peakPressure)
                phases[activePhase].peakPressure = v;
            phases[activePhase].pressureSum += v;
            phases[activePhase].pressureCount++;
        }
        unlock();
    });
    pluginManager->on("pump:flow:change", [this](Event &event) {
        const float v = event.getFloat("value");
        const float target = controller->getTargetFlow();
        const unsigned long nowMs = millis();
        lock();
        snapshot.pumpFlow = v;
        // Integrate pump flow (ml/s) into total water dispensed (incl. flush).
        if (lastFlowMillis != 0 && v > 0.0f) {
            const double dtS = static_cast<double>(nowMs - lastFlowMillis) / 1000.0;
            if (dtS > 0.0 && dtS < 5.0) // ignore stale gaps (sleep/idle)
                waterTotalMl += static_cast<double>(v) * dtS;
        }
        lastFlowMillis = nowMs;
        if (shotActive) {
            if (v > peakFlow)
                peakFlow = v;
            flowSum += v;
            flowCount++;
            if (target > 0.0f) { // only score adherence in flow-targeted phases
                flowErrSum += fabsf(v - target);
                flowErrCount++;
            }
        }
        if (activePhase >= 0) {
            if (v > phases[activePhase].peakFlow)
                phases[activePhase].peakFlow = v;
            phases[activePhase].flowSum += v;
            phases[activePhase].flowCount++;
        }
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

    // Scale gone: clear the latched weight so the coffee.scale.weight gauge
    // stops exporting the last reading (often a negative tare offset). Without
    // this haveWeight stays true forever and the series flatlines at the stale
    // value instead of going absent.
    pluginManager->on("scale:disconnect", [this](Event &) {
        lock();
        snapshot.weight = 0.0f;
        snapshot.haveWeight = false;
        unlock();
    });

    if (tracesEnabled) {
        pluginManager->on("controller:brew:start", [this](Event &) { onBrewStart(); });
        pluginManager->on("controller:brew:end", [this](Event &) { onBrewEnd(); });
        pluginManager->on("controller:brew:phase", [this](Event &event) { onBrewPhase(event.getInt("index")); });
    }

    if (metricsEnabled) {
        pluginManager->on("controller:wifi:disconnect", [this](Event &) {
            lock();
            wifiDisconnectsTotal++;
            unlock();
        });
        pluginManager->on("controller:bluetooth:disconnect", [this](Event &) {
            lock();
            bleDisconnectsTotal++;
            unlock();
        });
    }

    // Cache identity on the main thread now and whenever the controller link
    // (re)connects, so the export task never reads SystemInfo Strings directly.
    pluginManager->on("controller:ready", [this](Event &) { refreshMetadata(); });
    // Grinder model lives in Settings; refresh the cached resource attrs when the
    // user changes it from the web UI so coffee.grinder.model stays current.
    pluginManager->on("settings:changed", [this](Event &) { refreshMetadata(); });
    refreshMetadata();

    // Pin to core 1 so the blocking TLS handshakes stay off core 0 (WiFi MAC,
    // LWIP, AsyncTCP). 16KB stack: mbedTLS handshake + cert-bundle verification
    // can spike well past 12KB; the extra 4KB is cheap insurance against an
    // overflow that would reset the board mid-shot.
    xTaskCreatePinnedToCore(exportTaskFn, "OtelExport", 16384, this, 1, &taskHandle, 1);
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
    pressureSum = flowSum = tempSum = resistanceSum = resistanceSumSq = 0.0;
    pressureErrSum = flowErrSum = 0.0;
    pressureCount = flowCount = tempCount = resistanceCount = 0;
    pressureErrCount = flowErrCount = 0;
    tempMin = tempMax = 0.0f;
    firstFlowMillis = 0;
    phases.clear();
    activePhase = -1;
    Profile &profile = controller->getProfileManager()->getSelectedProfile();
    shotProfileLabel = profile.label;
    shotProfileType = profile.type;
    shotVolumetric = profile.isVolumetric();
    shotTargetTemp = controller->getTargetTemp();
    shotGrindLevel = static_cast<float>(controller->getGrindLevel());
    shotDoseWeight = static_cast<float>(controller->getDoseWeight());
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
    shotsTotal++;
    brewSecondsTotal += static_cast<double>(durationMs) / 1000.0;
    const float pp = peakPressure;
    const float pf = peakFlow;
    const float mw = maxWeight;
    const float tt = shotTargetTemp;
    const float gl = shotGrindLevel;
    const float dose = shotDoseWeight;
    const bool vol = shotVolumetric;
    const String label = shotProfileLabel;
    const String type = shotProfileType;
    const double pSumL = pressureSum, pErrL = pressureErrSum, fSumL = flowSum, fErrL = flowErrSum;
    const uint32_t pCntL = pressureCount, pErrCntL = pressureErrCount, fCntL = flowCount, fErrCntL = flowErrCount;
    const double tSumL = tempSum, rSumL = resistanceSum, rSumSqL = resistanceSumSq;
    const uint32_t tCntL = tempCount, rCntL = resistanceCount;
    const float tMinL = tempMin, tMaxL = tempMax;
    const unsigned long ttfL = firstFlowMillis;
    uint8_t tid[16];
    uint8_t sid[8];
    memcpy(tid, traceId, sizeof(tid));
    memcpy(sid, spanId, sizeof(sid));
    memcpy(lastShotTraceId, traceId, sizeof(lastShotTraceId));
    memcpy(lastShotSpanId, spanId, sizeof(lastShotSpanId));
    haveLastShot = true;
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
    span->attributes.push_back(otel::Attribute::dbl("coffee.grind.level", gl));
    if (dose > 0.0f) {
        span->attributes.push_back(otel::Attribute::dbl("coffee.dose.weight_g", dose));
        if (mw > 0.0f) // brew ratio = yield / dose (e.g. 2.0 == 1:2)
            span->attributes.push_back(otel::Attribute::dbl("coffee.brew.ratio", mw / dose));
    }
    if (ttfL > 0)
        span->attributes.push_back(
            otel::Attribute::integer("coffee.shot.time_to_first_flow_ms", static_cast<int64_t>(ttfL)));
    if (pCntL > 0)
        span->attributes.push_back(otel::Attribute::dbl("coffee.pressure.avg_bar", pSumL / pCntL));
    if (fCntL > 0)
        span->attributes.push_back(otel::Attribute::dbl("coffee.flow.avg_mls", fSumL / fCntL));
    if (pErrCntL > 0)
        span->attributes.push_back(otel::Attribute::dbl("coffee.pressure.adherence_bar", pErrL / pErrCntL));
    if (fErrCntL > 0)
        span->attributes.push_back(otel::Attribute::dbl("coffee.flow.adherence_mls", fErrL / fErrCntL));
    if (tCntL > 0) {
        span->attributes.push_back(otel::Attribute::dbl("coffee.temp.avg_c", tSumL / tCntL));
        span->attributes.push_back(otel::Attribute::dbl("coffee.temp.stability_c", tMaxL - tMinL));
    }
    if (rCntL > 0) {
        const double rAvg = rSumL / rCntL;
        span->attributes.push_back(otel::Attribute::dbl("coffee.puck.avg_resistance", rAvg));
        if (rCntL > 1 && rAvg > 0.0) {
            double var = rSumSqL / rCntL - rAvg * rAvg;
            if (var < 0.0)
                var = 0.0;
            span->attributes.push_back(otel::Attribute::dbl("coffee.puck.resistance_cv", std::sqrt(var) / rAvg));
        }
    }

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
        if (ph.pressureCount > 0)
            child->attributes.push_back(otel::Attribute::dbl("coffee.phase.avg_pressure_bar", ph.pressureSum / ph.pressureCount));
        if (ph.flowCount > 0)
            child->attributes.push_back(otel::Attribute::dbl("coffee.phase.avg_flow_mls", ph.flowSum / ph.flowCount));
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
    attrs.push_back(otel::Attribute::str("coffee.grinder.model", getGrinderDef(controller->getGrinderModel()).name));
    return attrs;
}

void OpenTelemetryPlugin::refreshMetadata() {
    // Must run on the main thread: buildResourceAttributes() and getSystemInfo()
    // read controller-owned Strings.
    std::vector<otel::Attribute> attrs = buildResourceAttributes();
    const String version = controller->getSystemInfo().version;
    lock();
    resourceAttrs = attrs;
    scopeVersion = version;
    unlock();
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

    const uint64_t now = nowUnixNanos();
    Snapshot s;
    uint64_t shots, wifiDc, bleDc, startNanos;
    double brewSecs, waterMl;
    bool inShot, haveLast;
    uint8_t curTid[16], curSid[8], lastTid[16], lastSid[8];
    std::vector<otel::Attribute> resAttrs;
    String version;
    String phaseName;
    float shotGl = 0.0f;
    lock();
    s = snapshot;
    if (metricsStartNanos == 0)
        metricsStartNanos = now;
    startNanos = metricsStartNanos;
    shots = shotsTotal;
    brewSecs = brewSecondsTotal;
    waterMl = waterTotalMl;
    wifiDc = wifiDisconnectsTotal;
    bleDc = bleDisconnectsTotal;
    inShot = shotActive;
    shotGl = shotGrindLevel;
    if (shotActive && activePhase >= 0 && activePhase < static_cast<int>(phases.size()))
        phaseName = phases[activePhase].name;
    memcpy(curTid, traceId, sizeof(curTid));
    memcpy(curSid, spanId, sizeof(curSid));
    haveLast = haveLastShot;
    memcpy(lastTid, lastShotTraceId, sizeof(lastTid));
    memcpy(lastSid, lastShotSpanId, sizeof(lastSid));
    resAttrs = resourceAttrs;
    version = scopeVersion;
    unlock();

    auto setExemplar = [](otel::MetricPoint &m, const uint8_t *tid, const uint8_t *sid) {
        m.hasExemplar = true;
        memcpy(m.traceId, tid, sizeof(m.traceId));
        memcpy(m.spanId, sid, sizeof(m.spanId));
    };

    using MP = otel::MetricPoint;
    std::vector<MP> metrics;
    metrics.push_back({"coffee.boiler.temperature", "Cel", s.temp});
    metrics.push_back({"coffee.boiler.target_temperature", "Cel", s.targetTemp});
    metrics.push_back({"coffee.boiler.pressure", "bar", s.pressure});
    metrics.push_back({"coffee.pump.flow", "ml/s", s.pumpFlow});
    metrics.push_back({"coffee.pump.puck_flow", "ml/s", s.puckFlow});
    if (s.puckResistanceValid)
        metrics.push_back({"coffee.pump.puck_resistance", "", s.puckResistance});
    if (s.haveWeight)
        metrics.push_back({"coffee.scale.weight", "g", s.weight});
    const size_t gaugeCount = metrics.size();
    // Cumulative monotonic counters (Sums).
    metrics.push_back({"coffee.shots.total", "{shot}", static_cast<double>(shots), MP::SUM, true});
    const size_t idxShots = metrics.size() - 1;
    metrics.push_back({"coffee.brew.duration.total", "s", brewSecs, MP::SUM, true});
    const size_t idxBrew = metrics.size() - 1;
    metrics.push_back({"coffee.water.total", "ml", waterMl, MP::SUM, true});
    metrics.push_back({"coffee.wifi.disconnects.total", "{event}", static_cast<double>(wifiDc), MP::SUM, true});
    metrics.push_back({"coffee.bluetooth.disconnects.total", "{event}", static_cast<double>(bleDc), MP::SUM, true});

    // While a shot is in flight, the live gauges belong to its trace and its
    // current phase. Tag them with shot id + phase name (so samples can be
    // grouped/filtered per shot/phase) and an exemplar to the shot span. The
    // cumulative Sums stay attribute-free so their series remain mergeable.
    if (inShot) {
        const String shotId = traceIdHex(curTid, sizeof(curTid));
        for (size_t i = 0; i < gaugeCount; i++) {
            setExemplar(metrics[i], curTid, curSid);
            metrics[i].attributes.push_back(otel::Attribute::str("coffee.shot.id", shotId));
            metrics[i].attributes.push_back(otel::Attribute::dbl("coffee.grind.level", shotGl));
            if (!phaseName.isEmpty())
                metrics[i].attributes.push_back(otel::Attribute::str("coffee.phase.name", phaseName));
        }
    }
    if (haveLast) {
        setExemplar(metrics[idxShots], lastTid, lastSid);
        setExemplar(metrics[idxBrew], lastTid, lastSid);
    }

    const size_t len = otel::OtlpEncoder::encodeMetrics(resAttrs, SCOPE_NAME, version, metrics, now, startNanos, buffer,
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
    lock();
    std::vector<otel::Attribute> resAttrs = resourceAttrs;
    String version = scopeVersion;
    unlock();
    const size_t len = otel::OtlpEncoder::encodeTrace(resAttrs, SCOPE_NAME, version, *span, buffer, BUFFER_SIZE);
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
