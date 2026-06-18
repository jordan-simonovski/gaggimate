// Grinder catalogue for the Web UI. Must stay in sync with the firmware copy in
// src/display/core/Grinders.h (same order = same index/id).
export const grinders = [
  { name: 'Custom / Generic', min: 0, max: 100, step: 1, unit: '' },
  { name: 'Varia VS3', min: 0, max: 20, step: 0.1, unit: '' }, // V2: stepless 0-20 dial
  { name: 'Niche Zero', min: 0, max: 50, step: 1, unit: '' }, // numbered 0-50 dial
  { name: 'Eureka Mignon', min: 0, max: 100, step: 1, unit: '' }, // stepless, no printed scale
  { name: 'DF64 / DF54', min: 0, max: 100, step: 1, unit: '' }, // stepless collar, no printed scale
  { name: 'Baratza Encore / Encore ESP', min: 1, max: 40, step: 1, unit: '' }, // 40 settings, starts at 1
  { name: 'Fellow Ode Gen 2', min: 1, max: 11, step: 1, unit: '' }, // 11 numbered settings
  { name: '1Zpresso (J/JX/K)', min: 0, max: 100, step: 1, unit: 'clicks' }, // click count varies by model
  { name: 'Comandante C40', min: 0, max: 50, step: 1, unit: 'clicks' },
  { name: 'Timemore C2/C3', min: 0, max: 36, step: 1, unit: 'clicks' }, // ~36 clicks/rotation
  { name: 'Mazzer Mini / Super Jolly', min: 0, max: 100, step: 1, unit: '' }, // stepless collar, no printed scale
];

export function getGrinder(id) {
  const idx = Number(id);
  if (!Number.isFinite(idx) || idx < 0 || idx >= grinders.length) {
    return grinders[0];
  }
  return grinders[idx];
}

// Format a grind level for display, e.g. "12.5" or "8 clicks". Trailing ".0" is
// stripped so whole numbers stay clean.
export function formatGrindLevel(value, grinderId) {
  const g = getGrinder(grinderId);
  const num = Number(value) || 0;
  const text = Number.isInteger(num) ? String(num) : num.toFixed(1);
  return g.unit ? `${text} ${g.unit}` : text;
}

// Brew ratio as "1:X" from a yield (or target) weight and the dose. Returns
// null when it can't be computed (no dose or no yield), so callers can show "—".
export function formatBrewRatio(yieldWeight, dose) {
  const y = Number(yieldWeight) || 0;
  const d = Number(dose) || 0;
  if (d <= 0 || y <= 0) return null;
  return `1:${(y / d).toFixed(1)}`;
}
