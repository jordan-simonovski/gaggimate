// The grinder catalogue lives in the firmware (src/display/core/Grinders.h) and
// arrives with GET /api/settings as `grinders`, indexed by `grinderModel`. The
// Web UI deliberately keeps no copy of the table: one source, nothing to drift.
// Adding a grinder means editing Grinders.h and nothing here.

// Only used before settings have loaded, or when running the UI without a
// device attached. Matches index 0 of the firmware table.
const FALLBACK = [{ name: 'Custom / Generic', min: 0, max: 100, step: 1, unit: '' }];

export function getGrinders(settings) {
  const list = settings?.grinders;
  return Array.isArray(list) && list.length > 0 ? list : FALLBACK;
}

export function getGrinder(settings, id) {
  const list = getGrinders(settings);
  const idx = Number(id);
  if (!Number.isFinite(idx) || idx < 0 || idx >= list.length) {
    return list[0];
  }
  return list[idx];
}

// Format a grind level for display, e.g. "12.5" or "8 clicks". Trailing ".0" is
// stripped so whole numbers stay clean. Takes a resolved grinder (see
// getGrinder) so this stays a pure formatter.
export function formatGrindLevel(value, grinder) {
  const num = Number(value) || 0;
  const text = Number.isInteger(num) ? String(num) : num.toFixed(1);
  return grinder?.unit ? `${text} ${grinder.unit}` : text;
}

// Brew ratio as "1:X" from a yield (or target) weight and the dose. Returns
// null when it can't be computed (no dose or no yield), so callers can show "—".
export function formatBrewRatio(yieldWeight, dose) {
  const y = Number(yieldWeight) || 0;
  const d = Number(dose) || 0;
  if (d <= 0 || y <= 0) return null;
  return `1:${(y / d).toFixed(1)}`;
}
