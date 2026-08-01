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

// One timestamped sample within a metric series. Each carries its own
// time_unix_nano so a gauge can ship a dense batch (e.g. 500ms samples) in a
// single export. Attributes/exemplar are per-point because OTLP series identity
// is per data point (phase.name can change mid-batch).
struct DataPoint {
    uint64_t timeNanos = 0;
    double value = 0.0;
    // Optional per-point attributes (e.g. coffee.shot.id, coffee.phase.name).
    // Capped by otlp.NumberDataPoint.attributes in otlp.options.
    std::vector<Attribute> attributes;
    // Optional exemplar linking this point to a trace (metrics -> traces).
    bool hasExemplar = false;
    uint8_t traceId[16] = {};
    uint8_t spanId[8] = {};
};

// One metric series for the current export tick. GAUGE carries a batch of
// instantaneous readings (points); SUM is a cumulative monotonic counter (e.g.
// shots total) carrying a start time so backends can compute rates, and uses a
// single point. Point counts are capped by otlp.{Gauge,Sum}.data_points.
struct MetricSeries {
    enum Kind { GAUGE, SUM };
    String name;
    String unit;
    Kind kind = GAUGE;
    bool monotonic = false; // only meaningful for SUM
    std::vector<DataPoint> points;
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

// Parse a W3C-style hex id (trace id: 32 chars, span id: 16) into `len` bytes.
// Returns false — leaving `out` untouched — unless `hex` is exactly len*2
// characters and all of them are hex digits, so a malformed or absent id can
// never be turned into a span pointing at an unrelated trace.
bool hexToBytes(const char *hex, size_t hexLen, uint8_t *out, size_t len);

class OtlpEncoder {
  public:
    // Encode an ExportMetricsServiceRequest. Returns the encoded byte count, or
    // 0 on failure (buffer too small / out of memory).
    // Each point carries its own time_unix_nano. startTimeNanos is the
    // cumulative-counter start (process/collector start); it is only written for
    // SUM points. Gauges ignore it.
    static size_t encodeMetrics(const std::vector<Attribute> &resourceAttrs, const String &scopeName,
                                const String &scopeVersion, const std::vector<MetricSeries> &metrics,
                                uint64_t startTimeNanos, uint8_t *buf, size_t bufSize);

    // Encode an ExportTraceServiceRequest carrying a single span.
    static size_t encodeTrace(const std::vector<Attribute> &resourceAttrs, const String &scopeName,
                              const String &scopeVersion, const SpanData &span, uint8_t *buf, size_t bufSize);
};

} // namespace otel
