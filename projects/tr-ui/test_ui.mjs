import assert from 'node:assert/strict';
import {presets,baselineFor,sameControls,metricValue,escapeHTML} from './ui-model.mjs';
for(const candidate of Object.values(presets)){
  const baseline=baselineFor(candidate);
  assert.equal(baseline.ai,0);assert.equal(baseline.bd,0);assert.equal(baseline.demand_scale,1);
  for(const key of ['policy_hold','policy_responsive','macro','success_gate'])assert.equal(candidate[key],baseline[key]);
  assert(sameControls(candidate,{...candidate}));assert(!sameControls(candidate,{...candidate,ai:candidate.ai+.01}));
}
assert(!sameControls(presets.ai,{}));
assert.equal(metricValue({values:[{metric:'margin',candidate:0,baseline:null}]},'margin'),0);
assert.equal(metricValue({values:[]},'missing'),null);
assert.equal(escapeHTML('<script>"&'), '&lt;script&gt;&quot;&amp;');
console.log('UI mapping contracts passed: matched controls, draft detection, undefined values and escaping.');
