export const presets = {
  ai: {ai:.2, demand_scale:1, erosion:.5, fixed_shift:0, bd:0, macro:0, success_gate:1, policy_hold:0, policy_responsive:1},
  growth: {ai:.2, demand_scale:1.2, erosion:.5, fixed_shift:0, bd:0, macro:0, success_gate:1, policy_hold:0, policy_responsive:1},
  bd: {ai:.2, demand_scale:1, erosion:.5, fixed_shift:0, bd:1, macro:0, success_gate:1, policy_hold:0, policy_responsive:1},
  freeze: {ai:.2, demand_scale:1, erosion:.5, fixed_shift:0, bd:0, macro:0, success_gate:1, policy_hold:0, policy_responsive:0},
  none: {ai:0, demand_scale:1, erosion:0, fixed_shift:0, bd:0, macro:0, success_gate:1, policy_hold:0, policy_responsive:1},
};
export function baselineFor(candidate) {
  return {...candidate, ai:0, erosion:0, demand_scale:1, fixed_shift:0, bd:0};
}
export function sameControls(a,b) {
  return Object.keys(presets.ai).every(k => Number.isFinite(a?.[k]) && Number.isFinite(b?.[k]) && Math.abs(a[k]-b[k])<1e-10);
}
export function policyName(p) {return p.policy_hold ? 'Replace all departures' : p.policy_responsive ? 'Hire to match demand' : 'Freeze new hiring';}
export function metricValue(comparison,metric,side='candidate') {return comparison?.values.find(v=>v.metric===metric)?.[side]??null;}
export function escapeHTML(value){return String(value).replace(/[&<>"']/g,s=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[s]));}
