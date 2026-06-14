#pragma once

#include <Arduino.h>
#include <cstdint>
#include <vector>

// OTLP/HTTP payload builder. Fills the statically-sized nanopb structs (see
// otlp.proto / otlp.options) for ExportMetricsServiceRequest and
// ExportTraceServiceRequest and encodes them into a caller-provided buffer.
//
// The data model below is intentionally decoupled from the generated nanopb
// types so callers (the plugin) never include otlp.pb.h.
namespace otel {

// A single OTLP attribute (KeyValue). Only the variant matching `type` is read.
struct Attribute {
    enum Type { STRING, DOUBLE, INT, BOOL };
    String key;
    Type type = STRING;
    String s;
    double d = 0.0;
    int64_t i = 0;
    bool b = false;

    static Attribute str(const String &k, const String &v) {
        Attribute a;
        a.key = k;
        a.type = STRING;
        a.s = v;
        return a;
    }
    static Attribute dbl(const String &k, double v) {
        Attribute a;
        a.key = k;
        a.type = DOUBLE;
        a.d = v;
        return a;
    }
    static Attribute integer(const String &k, int64_t v) {
        Attribute a;
        a.key = k;
        a.type = INT;
        a.i = v;
        return a;
    }
    static Attribute boolean(const String &k, bool v) {
        Attribute a;
        a.key = k;
        a.type = BOOL;
        a.b = v;
        return a;
    }
};

// One gauge metric (one data point) for the current export tick.
struct MetricPoint {
    String name;
    String unit;
    double value = 0.0;
};

struct SpanEventData {
    String name;
    uint64_t timeNanos = 0;
    std::vector<Attribute> attributes;
};

struct SpanData {
    uint8_t traceId[16]{};
    uint8_t spanId[8]{};
    uint8_t parentSpanId[8]{};
    bool hasParent = false; // root span when false (parent_span_id omitted)
    String name;
    uint64_t startNanos = 0;
    uint64_t endNanos = 0;
    int statusCode = 0; // 0 = UNSET, 1 = OK, 2 = ERROR
    std::vector<Attribute> attributes;
    std::vector<SpanEventData> events;
};

class OtlpEncoder {
  public:
    // Encode an ExportMetricsServiceRequest. Returns the encoded byte count, or
    // 0 on failure (buffer too small / out of memory).
    static size_t encodeMetrics(const std::vector<Attribute> &resourceAttrs, const String &scopeName,
                                const String &scopeVersion, const std::vector<MetricPoint> &metrics, uint64_t timeNanos,
                                uint8_t *buf, size_t bufSize);

    // Encode an ExportTraceServiceRequest carrying a single span.
    static size_t encodeTrace(const std::vector<Attribute> &resourceAttrs, const String &scopeName,
                              const String &scopeVersion, const SpanData &span, uint8_t *buf, size_t bufSize);
};

} // namespace otel
