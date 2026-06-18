#pragma once
#ifndef GRINDERS_H
#define GRINDERS_H

// Grinder catalogue shared by Settings (default/clamping), Controller (stepping)
// and the OpenTelemetry plugin (resource attribute name). The Web UI carries its
// own copy of the same table in web/src/config/grinders.js - keep them in sync.
//
// name must stay <= 48 chars (OTLP AnyValue.string_value cap). Values are sane
// defaults per grinder; tune as needed.
struct GrinderDef {
    const char *name;
    double min;
    double max;
    double step;
    const char *unit; // optional suffix shown in UI (e.g. "clicks"); "" for none
};

inline constexpr GrinderDef GRINDERS[] = {
    {"Custom / Generic", 0.0, 100.0, 1.0, ""},
    {"Varia VS3", 0.0, 20.0, 0.1, ""},          // V2: stepless 0-20 dial
    {"Niche Zero", 0.0, 50.0, 1.0, ""},         // numbered 0-50 dial
    {"Eureka Mignon", 0.0, 100.0, 1.0, ""},     // stepless, no printed scale
    {"DF64 / DF54", 0.0, 100.0, 1.0, ""},       // stepless collar, no printed scale
    {"Baratza Encore / Encore ESP", 1.0, 40.0, 1.0, ""}, // 40 settings, starts at 1
    {"Fellow Ode Gen 2", 1.0, 11.0, 1.0, ""},   // 11 numbered settings
    {"1Zpresso (J/JX/K)", 0.0, 100.0, 1.0, "clicks"},    // click count varies by model
    {"Comandante C40", 0.0, 50.0, 1.0, "clicks"},
    {"Timemore C2/C3", 0.0, 36.0, 1.0, "clicks"},        // ~36 clicks/rotation
    {"Mazzer Mini / Super Jolly", 0.0, 100.0, 1.0, ""},  // stepless collar, no printed scale
};

inline constexpr int GRINDER_COUNT = sizeof(GRINDERS) / sizeof(GRINDERS[0]);

inline const GrinderDef &getGrinderDef(int id) {
    if (id < 0 || id >= GRINDER_COUNT)
        id = 0;
    return GRINDERS[id];
}

#endif // GRINDERS_H
