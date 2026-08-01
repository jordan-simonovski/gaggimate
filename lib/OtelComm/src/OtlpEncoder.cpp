#include "OtlpEncoder.h"

#include "otlp.pb.h"
#include <cstring>
#include <esp_heap_caps.h>
#include <pb_encode.h>

// Compile-time guard against nanopb's hard limit: field offsets are encoded in
// 16 bits, so a single generated message struct must stay under 64KB (see
// PB_FITS in pb.h). Raising otlp.Gauge.data_points or adding metrics inflates
// these structs; if one approaches the cap the build fails here with a readable
// reason instead of nanopb's cryptic FIELDINFO_DOES_NOT_FIT assert. The margin
// means we notice before hitting the wall. The full sample batch is split into
// chunks at encode time precisely so these stay small (see encodeMetrics).
static_assert(sizeof(otlp_ScopeMetrics) <= 60u * 1024u,
              "otlp_ScopeMetrics approaching nanopb's 64KB message cap: reduce ScopeMetrics.metrics or "
              "otlp.Gauge.data_points in otlp.options");
static_assert(sizeof(otlp_ResourceMetrics) <= 60u * 1024u,
              "otlp_ResourceMetrics approaching nanopb's 64KB message cap");
static_assert(sizeof(otlp_ExportMetricsServiceRequest) <= 60u * 1024u,
              "otlp_ExportMetricsServiceRequest approaching nanopb's 64KB message cap");

namespace otel {

bool hexToBytes(const char *hex, size_t hexLen, uint8_t *out, size_t len) {
    if (hex == nullptr || hexLen != len * 2)
        return false;
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hexLen; i++) { // validate before writing anything
        if (value(hex[i]) < 0)
            return false;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = static_cast<uint8_t>((value(hex[i * 2]) << 4) | value(hex[i * 2 + 1]));
    }
    return true;
}

namespace {

// Large OTLP request structs live in PSRAM; the export task stack stays small.
void *otlpAlloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == nullptr)
        p = malloc(n);
    return p;
}

void fillKeyValue(otlp_KeyValue &kv, const Attribute &a) {
    strlcpy(kv.key, a.key.c_str(), sizeof(kv.key));
    kv.has_value = true;
    switch (a.type) {
    case Attribute::STRING:
        kv.value.which_value = otlp_AnyValue_string_value_tag;
        strlcpy(kv.value.value.string_value, a.s.c_str(), sizeof(kv.value.value.string_value));
        break;
    case Attribute::DOUBLE:
        kv.value.which_value = otlp_AnyValue_double_value_tag;
        kv.value.value.double_value = a.d;
        break;
    case Attribute::INT:
        kv.value.which_value = otlp_AnyValue_int_value_tag;
        kv.value.value.int_value = a.i;
        break;
    case Attribute::BOOL:
        kv.value.which_value = otlp_AnyValue_bool_value_tag;
        kv.value.value.bool_value = a.b;
        break;
    }
}

// Copy up to `cap` attributes into a fixed nanopb array; returns the count used.
pb_size_t fillAttributes(otlp_KeyValue *dst, pb_size_t cap, const std::vector<Attribute> &src) {
    pb_size_t n = src.size() < cap ? static_cast<pb_size_t>(src.size()) : cap;
    for (pb_size_t i = 0; i < n; i++)
        fillKeyValue(dst[i], src[i]);
    return n;
}

} // namespace

namespace {

// Fills one NumberDataPoint from a DataPoint. start is written only for SUM
// points (proto3 zero is omitted for gauges, which is what we want).
void fillDataPoint(otlp_NumberDataPoint &dp, const DataPoint &src, uint64_t start) {
    dp.start_time_unix_nano = start;
    dp.time_unix_nano = src.timeNanos;
    dp.which_value = otlp_NumberDataPoint_as_double_tag;
    dp.value.as_double = src.value;
    dp.attributes_count =
        fillAttributes(dp.attributes, sizeof(dp.attributes) / sizeof(dp.attributes[0]), src.attributes);
    if (src.hasExemplar) {
        dp.exemplars_count = 1;
        otlp_Exemplar &ex = dp.exemplars[0];
        ex.time_unix_nano = src.timeNanos;
        ex.as_double = src.value;
        ex.span_id.size = sizeof(ex.span_id.bytes);
        memcpy(ex.span_id.bytes, src.spanId, sizeof(ex.span_id.bytes));
        ex.trace_id.size = sizeof(ex.trace_id.bytes);
        memcpy(ex.trace_id.bytes, src.traceId, sizeof(ex.trace_id.bytes));
    }
}

// Encodes one ExportMetricsServiceRequest (one ResourceMetrics) covering gauge
// points [pointOffset, pointOffset + chunkCap) of each series. SUM series are
// emitted only when includeSums is set (so cumulative counters appear exactly
// once across the chunked output). Returns bytes written, or 0 on failure.
size_t encodeChunk(const std::vector<Attribute> &resourceAttrs, const String &scopeName, const String &scopeVersion,
                   const std::vector<MetricSeries> &metrics, uint64_t startTimeNanos, size_t pointOffset,
                   bool includeSums, uint8_t *buf, size_t bufSize) {
    auto *req = static_cast<otlp_ExportMetricsServiceRequest *>(otlpAlloc(sizeof(otlp_ExportMetricsServiceRequest)));
    if (req == nullptr)
        return 0;
    memset(req, 0, sizeof(*req));

    req->resource_metrics_count = 1;
    otlp_ResourceMetrics &rm = req->resource_metrics[0];
    rm.has_resource = true;
    rm.resource.attributes_count = fillAttributes(
        rm.resource.attributes, sizeof(rm.resource.attributes) / sizeof(rm.resource.attributes[0]), resourceAttrs);

    rm.scope_metrics_count = 1;
    otlp_ScopeMetrics &sm = rm.scope_metrics[0];
    sm.has_scope = true;
    strlcpy(sm.scope.name, scopeName.c_str(), sizeof(sm.scope.name));
    strlcpy(sm.scope.version, scopeVersion.c_str(), sizeof(sm.scope.version));

    const pb_size_t metricCap = sizeof(sm.metrics) / sizeof(sm.metrics[0]);
    pb_size_t nm = 0;
    for (size_t j = 0; j < metrics.size() && nm < metricCap; j++) {
        const MetricSeries &series = metrics[j];
        if (series.kind == MetricSeries::SUM) {
            if (!includeSums || series.points.empty())
                continue;
            otlp_Metric &m = sm.metrics[nm];
            strlcpy(m.name, series.name.c_str(), sizeof(m.name));
            strlcpy(m.unit, series.unit.c_str(), sizeof(m.unit));
            m.which_data = otlp_Metric_sum_tag;
            m.data.sum.aggregation_temporality = otlp_AggregationTemporality_AGGREGATION_TEMPORALITY_CUMULATIVE;
            m.data.sum.is_monotonic = series.monotonic;
            m.data.sum.data_points_count = 1;
            fillDataPoint(m.data.sum.data_points[0], series.points[0], startTimeNanos);
            nm++;
            continue;
        }
        // GAUGE: take this chunk's window of points, skip the metric if empty.
        const pb_size_t cap =
            sizeof(sm.metrics[nm].data.gauge.data_points) / sizeof(sm.metrics[nm].data.gauge.data_points[0]);
        if (pointOffset >= series.points.size())
            continue;
        const size_t remaining = series.points.size() - pointOffset;
        const pb_size_t np = remaining < cap ? static_cast<pb_size_t>(remaining) : cap;
        if (np == 0)
            continue;
        otlp_Metric &m = sm.metrics[nm];
        strlcpy(m.name, series.name.c_str(), sizeof(m.name));
        strlcpy(m.unit, series.unit.c_str(), sizeof(m.unit));
        m.which_data = otlp_Metric_gauge_tag;
        for (pb_size_t i = 0; i < np; i++)
            fillDataPoint(m.data.gauge.data_points[i], series.points[pointOffset + i], 0);
        m.data.gauge.data_points_count = np;
        nm++;
    }
    sm.metrics_count = nm;

    pb_ostream_t os = pb_ostream_from_buffer(buf, bufSize);
    bool ok = pb_encode(&os, &otlp_ExportMetricsServiceRequest_msg, req);
    size_t len = ok ? os.bytes_written : 0;
    free(req);
    return len;
}

} // namespace

size_t OtlpEncoder::encodeMetrics(const std::vector<Attribute> &resourceAttrs, const String &scopeName,
                                  const String &scopeVersion, const std::vector<MetricSeries> &metrics,
                                  uint64_t startTimeNanos, uint8_t *buf, size_t bufSize) {
    // Per-chunk gauge capacity is the generated data_points array size (kept
    // small so each ScopeMetrics struct stays under nanopb's 64KB cap). The full
    // batch is emitted as back-to-back ExportMetricsServiceRequest messages;
    // concatenating serialized protobufs of the same type is itself a valid
    // message (repeated resource_metrics concatenate), so the collector sees one
    // request with several ResourceMetrics.
    constexpr size_t chunkCap = sizeof(otlp_Gauge::data_points) / sizeof(otlp_NumberDataPoint);

    size_t maxGaugePoints = 0;
    for (const MetricSeries &s : metrics)
        if (s.kind == MetricSeries::GAUGE && s.points.size() > maxGaugePoints)
            maxGaugePoints = s.points.size();

    size_t total = 0;
    bool first = true;
    for (size_t offset = 0; first || offset < maxGaugePoints; offset += chunkCap) {
        const size_t len = encodeChunk(resourceAttrs, scopeName, scopeVersion, metrics, startTimeNanos, offset,
                                       /*includeSums=*/first, buf + total, bufSize - total);
        if (len == 0)
            return first ? 0 : total; // first chunk failing means nothing usable
        total += len;
        first = false;
        if (offset + chunkCap >= maxGaugePoints)
            break;
    }
    return total;
}

size_t OtlpEncoder::encodeTrace(const std::vector<Attribute> &resourceAttrs, const String &scopeName,
                                const String &scopeVersion, const SpanData &span, uint8_t *buf, size_t bufSize) {
    auto *req = static_cast<otlp_ExportTraceServiceRequest *>(otlpAlloc(sizeof(otlp_ExportTraceServiceRequest)));
    if (req == nullptr)
        return 0;
    memset(req, 0, sizeof(*req));

    req->resource_spans_count = 1;
    otlp_ResourceSpans &rs = req->resource_spans[0];
    rs.has_resource = true;
    rs.resource.attributes_count = fillAttributes(
        rs.resource.attributes, sizeof(rs.resource.attributes) / sizeof(rs.resource.attributes[0]), resourceAttrs);

    rs.scope_spans_count = 1;
    otlp_ScopeSpans &ss = rs.scope_spans[0];
    ss.has_scope = true;
    strlcpy(ss.scope.name, scopeName.c_str(), sizeof(ss.scope.name));
    strlcpy(ss.scope.version, scopeVersion.c_str(), sizeof(ss.scope.version));

    ss.spans_count = 1;
    otlp_Span &s = ss.spans[0];
    s.trace_id.size = sizeof(s.trace_id.bytes);
    memcpy(s.trace_id.bytes, span.traceId, sizeof(s.trace_id.bytes));
    s.span_id.size = sizeof(s.span_id.bytes);
    memcpy(s.span_id.bytes, span.spanId, sizeof(s.span_id.bytes));
    // Leave parent_span_id at size 0 (omitted) for root spans; nanopb skips
    // zero-length proto3 bytes, which collectors read as "no parent".
    if (span.hasParent) {
        s.parent_span_id.size = sizeof(s.parent_span_id.bytes);
        memcpy(s.parent_span_id.bytes, span.parentSpanId, sizeof(s.parent_span_id.bytes));
    }
    strlcpy(s.name, span.name.c_str(), sizeof(s.name));
    s.kind = otlp_SpanKind_SPAN_KIND_INTERNAL;
    s.start_time_unix_nano = span.startNanos;
    s.end_time_unix_nano = span.endNanos;
    s.attributes_count = fillAttributes(s.attributes, sizeof(s.attributes) / sizeof(s.attributes[0]), span.attributes);

    const pb_size_t eventCap = sizeof(s.events) / sizeof(s.events[0]);
    pb_size_t ne = span.events.size() < eventCap ? static_cast<pb_size_t>(span.events.size()) : eventCap;
    s.events_count = ne;
    for (pb_size_t i = 0; i < ne; i++) {
        otlp_SpanEvent &ev = s.events[i];
        ev.time_unix_nano = span.events[i].timeNanos;
        strlcpy(ev.name, span.events[i].name.c_str(), sizeof(ev.name));
        ev.attributes_count =
            fillAttributes(ev.attributes, sizeof(ev.attributes) / sizeof(ev.attributes[0]), span.events[i].attributes);
    }

    s.has_status = true;
    s.status.code = span.statusCode;

    pb_ostream_t os = pb_ostream_from_buffer(buf, bufSize);
    bool ok = pb_encode(&os, &otlp_ExportTraceServiceRequest_msg, req);
    size_t len = ok ? os.bytes_written : 0;
    free(req);
    return len;
}

} // namespace otel
