#include "OtlpEncoder.h"

#include "otlp.pb.h"
#include <cstring>
#include <esp_heap_caps.h>
#include <pb_encode.h>

namespace otel {

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

size_t OtlpEncoder::encodeMetrics(const std::vector<Attribute> &resourceAttrs, const String &scopeName,
                                  const String &scopeVersion, const std::vector<MetricPoint> &metrics, uint64_t timeNanos,
                                  uint64_t startTimeNanos, uint8_t *buf, size_t bufSize) {
    auto *req = static_cast<otlp_ExportMetricsServiceRequest *>(otlpAlloc(sizeof(otlp_ExportMetricsServiceRequest)));
    if (req == nullptr)
        return 0;
    // init_zero is all-zeros for these messages; memset avoids the brace-init
    // assignment that C++ rejects on the generated aggregate types.
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
    pb_size_t nm = metrics.size() < metricCap ? static_cast<pb_size_t>(metrics.size()) : metricCap;
    sm.metrics_count = nm;
    for (pb_size_t j = 0; j < nm; j++) {
        otlp_Metric &m = sm.metrics[j];
        strlcpy(m.name, metrics[j].name.c_str(), sizeof(m.name));
        strlcpy(m.unit, metrics[j].unit.c_str(), sizeof(m.unit));
        otlp_NumberDataPoint *dp = nullptr;
        if (metrics[j].kind == MetricPoint::SUM) {
            m.which_data = otlp_Metric_sum_tag;
            m.data.sum.aggregation_temporality = otlp_AggregationTemporality_AGGREGATION_TEMPORALITY_CUMULATIVE;
            m.data.sum.is_monotonic = metrics[j].monotonic;
            m.data.sum.data_points_count = 1;
            dp = &m.data.sum.data_points[0];
            dp->start_time_unix_nano = startTimeNanos;
        } else {
            m.which_data = otlp_Metric_gauge_tag;
            m.data.gauge.data_points_count = 1;
            dp = &m.data.gauge.data_points[0];
        }
        dp->time_unix_nano = timeNanos;
        dp->which_value = otlp_NumberDataPoint_as_double_tag;
        dp->value.as_double = metrics[j].value;
        dp->attributes_count =
            fillAttributes(dp->attributes, sizeof(dp->attributes) / sizeof(dp->attributes[0]), metrics[j].attributes);
        if (metrics[j].hasExemplar) {
            dp->exemplars_count = 1;
            otlp_Exemplar &ex = dp->exemplars[0];
            ex.time_unix_nano = timeNanos;
            ex.as_double = metrics[j].value;
            ex.span_id.size = sizeof(ex.span_id.bytes);
            memcpy(ex.span_id.bytes, metrics[j].spanId, sizeof(ex.span_id.bytes));
            ex.trace_id.size = sizeof(ex.trace_id.bytes);
            memcpy(ex.trace_id.bytes, metrics[j].traceId, sizeof(ex.trace_id.bytes));
        }
    }

    pb_ostream_t os = pb_ostream_from_buffer(buf, bufSize);
    bool ok = pb_encode(&os, &otlp_ExportMetricsServiceRequest_msg, req);
    size_t len = ok ? os.bytes_written : 0;
    free(req);
    return len;
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
