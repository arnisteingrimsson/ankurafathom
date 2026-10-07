import {initValidation} from '/validation.js';
import {FathomClient} from '/client.mjs';
import {presets,baselineFor,sameControls,policyName,metricValue,escapeHTML as esc} from '/ui-model.mjs';

const $=id=>document.getElementById(id);
const api=new FathomClient(location.origin);
const state={year:5,chart:'revenue',view:'overview',preview:null,project:null,description:null,
  display:null,previewCompatible:true,controls:{...presets.ai},busy:false,job:null,watch:null,poll:null,playing:null,loading:false,explaining:false};
const money=v=>v==null?'—':`${v<0?'−':''}$${(Math.abs(v)/1e6).toFixed(2)}M`;
const percent=v=>v==null?'—':`${(v*100).toFixed(1)}%`;
const number=v=>v==null?'—':v.toLocaleString('en-US',{maximumFractionDigits:1,minimumFractionDigits:1});
const signed=(v,format)=>v==null?'—':`${v>0?'+':''}${format(v)}`;
const formats={revenue:money,ebitda:money,collections:money,cash_flow:money,cost:money,margin:percent,utilization:percent,headcount:number};
const policyHelp={responsive:'Adjust hiring with a two-month lag. Surplus capacity leaves through attrition.',
  hold:'Replace departures immediately to maintain workforce. This is a constant-workforce control.',
  freeze:'Stop new hiring. Natural attrition reduces payroll and can also constrain delivery.'};

function error(message){$('alert').textContent=message;$('alert').hidden=!message;}
function status(message){$('run-status').textContent=message;if(state.busy)$('evidence-label').textContent=message+' · previous results shown';}
function dirty(){return state.display && ((!state.display.live&&!state.previewCompatible)||!sameControls(state.controls,state.display.controls));}
function readControls(){return {ai:+$('ai').value/100,demand_scale:1+(+$('demand').value/100),
  erosion:+$('erosion').value/100,fixed_shift:+$('fixed').value/100,bd:+$('bd').checked,macro:+$('macro').checked,
  success_gate:+$('success').value/100,policy_hold:+($('policy').value==='hold'),policy_responsive:+($('policy').value==='responsive')};}
function writeControls(c){
  $('ai').value=c.ai*100;$('demand').value=Math.round((c.demand_scale-1)*100);$('erosion').value=c.erosion*100;
  $('fixed').value=c.fixed_shift*100;$('bd').checked=!!c.bd;$('macro').checked=!!c.macro;$('success').value=c.success_gate*100;
  $('policy').value=c.policy_hold?'hold':c.policy_responsive?'responsive':'freeze';state.controls={...c};controlLabels();
}
function controlLabels(){
  const c=state.controls;
  $('ai-value').textContent=`${Math.round(c.ai*100)}%`;
  $('demand-value').textContent=`${c.demand_scale>1?'+':''}${Math.round((c.demand_scale-1)*100)}%`;
  $('erosion-value').textContent=`${Math.round(c.erosion*100)}%`;
  $('fixed-value').textContent=`${Math.round(((state.project?.fixed_share??.15)+c.fixed_shift)*100)}%`;
  $('success-value').textContent=`${Math.round(c.success_gate*100)}%`;
  $('policy-help').textContent=policyHelp[$('policy').value];
  for(const el of document.querySelectorAll('input[type=range]')){
    const progress=(Number(el.value)-Number(el.min))/(Number(el.max)-Number(el.min))*100;
    el.style.background=`linear-gradient(to right, #177c72 ${progress}%, #dce4dd ${progress}%)`;
  }
}
function setBusy(value){
  state.busy=value;
  for(const el of $('scenario-form').querySelectorAll('input,select'))el.disabled=value;
  $('run').disabled=value||!state.description;for(const id of ['cancel','cancel-top']){$(id).hidden=!value;$(id).disabled=value&&!state.job;}
  $('run-status').classList.toggle('busy',value);
  $('run').firstElementChild.textContent=value?'Running comparison…':'Run comparison';
}
function evidence(){
  const d=state.display;if(!d)return;
  document.querySelector('.evidence-bar').classList.toggle('draft',!!dirty());
  const source=d.live?`Verified ${d.cacheHit?'reused':'live'} run`:'Verified saved example · 30 Sep 2026';
  $('evidence-label').innerHTML=`<span class="status-dot"></span>${dirty()?'Draft changed · results below are from the previous comparison':source+' · synthetic assumptions'}`;
  $('export').disabled=false;$('explain').disabled=state.busy;
  $('explain-note').textContent='Fee recognition and approval adjustments from verified results.';
}
function render(){
  if(!state.display)return;
  const comparison=state.display.annual[state.year-1];
  $('year').value=String(state.year);$('scrub').value=String(state.year);$('playback-label').textContent=`Year ${state.year} of 5`;
  const cards=[['revenue','Annual revenue','↗'],['ebitda','Practice EBITDA','◈'],['margin','EBITDA margin','%'],['headcount','Ending workforce','↔']];
  $('kpis').innerHTML=cards.map(([key,label,symbol])=>{
    const row=comparison.values.find(v=>v.metric===key),value=row?.candidate;
    const delta=key==='margin'?`${signed(row?.delta==null?null:row.delta*100,v=>v.toFixed(1))} pp`:
      key==='headcount'?`${signed(row?.delta,number)} FTE`:row?.percent_change==null?'N/A':`${signed(row.percent_change,v=>v.toFixed(1))}%`;
    const tone=key==='headcount'?'neutral':row?.delta>0?'positive':row?.delta<0?'negative':'neutral';
    return `<article class="kpi"><div class="kpi-label">${label}<span class="symbol" aria-hidden="true">${symbol}</span></div><div class="kpi-value">${formats[key](value)}${key==='headcount'?'<small>FTE</small>':''}</div><div class="kpi-change"><span class="change ${tone}">${delta}</span><span>vs matched baseline</span></div></article>`;
  }).join('');
  drawChart();drawWorkforce(comparison);drawAssumptions();
  const fields=[['revenue','Revenue'],['ebitda','EBITDA'],['margin','EBITDA margin'],['utilization','Delivery utilization'],['headcount','Ending expected FTE'],['cash_flow','Operating cash']];
  $('comparison-table').innerHTML=`<table><caption class="sr-only">Year ${state.year} financial comparison</caption><thead><tr><th>Year ${state.year}</th><th>Baseline</th><th>Your scenario</th></tr></thead><tbody>${fields.map(([key,label])=>`<tr><td>${label}</td><td>${formats[key](metricValue(comparison,key,'baseline'))}</td><td>${formats[key](metricValue(comparison,key))}</td></tr>`).join('')}</tbody></table>`;
  const rev=comparison.values.find(v=>v.metric==='revenue'),profit=comparison.values.find(v=>v.metric==='ebitda');
  const title=rev.delta<-.01&&profit.delta<-.01?'Efficiency needs a path to growth.':rev.delta>.01&&profit.delta>.01?'More work changes the equation.':Math.abs(rev.delta)<.01&&Math.abs(profit.delta)<.01?'A clear starting point.':'The trade-off is in the mix.';
  $('decision-title').textContent=title;
  const c=state.display.controls;
  $('decision-text').textContent=`In year ${state.year}, this scenario ${rev.delta<0?'reduces':'adds'} ${money(Math.abs(rev.delta))} in revenue and ${profit.delta<0?'reduces':'adds'} ${money(Math.abs(profit.delta))} in EBITDA. Both cases use “${policyName(c).toLowerCase()}” and the same market assumptions.`;
  const valuation=state.display.valuation;
  $('decision-metrics').innerHTML=valuation?`<div><span>5-YEAR INCREMENTAL NPV</span><strong>${money(valuation.incremental_npv)}</strong></div><div><span>PAYBACK</span><strong>${valuation.payback_status==='recovered'?`Month ${valuation.undiscounted_payback_time}`:valuation.payback_status==='no_deficit'?'No deficit':'Not reached'}</strong></div>`:'';
  evidence();controlLabels();
}
function drawChart(){
  const metric=state.chart,annual=state.display.annual;
  $('chart-title').textContent={revenue:'Revenue trajectory',ebitda:'EBITDA trajectory',cash_flow:'Operating cash trajectory'}[metric];
  const b=annual.map(c=>metricValue(c,metric,'baseline')/1e6),c=annual.map(c=>metricValue(c,metric)/1e6);
  const min=Math.min(0,...b,...c),peak=Math.max(...b,...c,1),range=peak-min;
  const step=Math.max(1,Math.ceil(range/4/5)*5),lo=Math.floor(min/step)*step,hi=Math.ceil(peak/step)*step;
  const width=Math.max(280,$('chart').clientWidth),left=42,right=width-25;
  const x=i=>left+i*(right-left)/4,y=v=>195-(v-lo)/(hi-lo)*169;
  const line=values=>values.map((v,i)=>`${i?'L':'M'}${x(i)},${y(v)}`).join(' ');
  const ticks=Array.from({length:Math.round((hi-lo)/step)+1},(_,i)=>lo+i*step);
  const selected=state.year-1;
  $('chart').innerHTML=`<svg viewBox="0 0 ${width} 235" role="img" aria-label="Annual ${esc(metric.replace('_',' '))} in millions of dollars, baseline and scenario, years 1 to 5"><defs><linearGradient id="area" x1="0" y1="0" x2="0" y2="1"><stop stop-color="#bddbc8" stop-opacity=".4"/><stop offset="1" stop-color="#e5f1e5" stop-opacity=".06"/></linearGradient></defs>${ticks.map(v=>`<line x1="${left}" x2="${right}" y1="${y(v)}" y2="${y(v)}" stroke="#edf0e9"/><text x="${left-13}" y="${y(v)+4}" text-anchor="end">${v}</text>`).join('')}<path d="${line(c)} L${x(4)},${y(0)} L${x(0)},${y(0)} Z" fill="url(#area)"/><line x1="${x(selected)}" x2="${x(selected)}" y1="17" y2="198" stroke="#d5ded5" stroke-dasharray="3 4"/><path d="${line(b)}" fill="none" stroke="#a6b0a8" stroke-width="2" stroke-dasharray="5 5"/><path d="${line(c)}" fill="none" stroke="#177c72" stroke-width="2.5"/>${c.map((v,i)=>`<circle cx="${x(i)}" cy="${y(v)}" r="${i===selected?5:3}" fill="${i===selected?'#177c72':'#fff'}" stroke="#177c72" stroke-width="1.5"><title>Year ${i+1}: scenario ${money(v*1e6)}, baseline ${money(b[i]*1e6)}</title></circle><text x="${x(i)}" y="221" text-anchor="middle">Year ${i+1}</text>`).join('')}<text class="value-label" x="${Math.min(right-72,x(selected)+8)}" y="${Math.max(14,y(c[selected])-12)}">$${c[selected].toFixed(2)}M</text></svg>`;
}
function drawWorkforce(comparison){
  const levels=(state.display.live?state.project:state.preview.project).levels;
  const max=Math.max(1,...levels.flatMap(l=>['baseline','candidate'].map(s=>metricValue(comparison,'headcount_'+l.id,s)??0)));
  $('workforce-chart').innerHTML='<div class="workforce-legend">Grey: matched baseline · Green: your scenario</div>'+levels.map(l=>{
    const b=metricValue(comparison,'headcount_'+l.id,'baseline'),c=metricValue(comparison,'headcount_'+l.id);
    return `<div class="workforce-row"><div class="workforce-name">${esc(l.id.replaceAll('_',' ').replace(/^./,s=>s.toUpperCase()))}<small>${l.count} initial employees</small></div><div class="workforce-bars" aria-hidden="true"><span style="width:${(b??0)/max*100}%"></span><span style="width:${(c??0)/max*100}%"></span></div><div class="workforce-numbers">${number(c)} FTE<small>${number(b)} baseline</small></div></div>`;
  }).join('');
}
function drawAssumptions(){
  const c=state.display.controls,project=state.display.live?state.project:state.preview.project;
  const rows=[['Displayed comparison','Year '+state.year],['Initial employees',project.initial_fte],['Hiring policy',policyName(c)],['AI eligible-task savings',percent(c.ai)],['Pipeline growth',`${Math.round((c.demand_scale-1)*100)}%`],['Fixed-fee share',percent(project.fixed_share+c.fixed_shift)],['Price pass-through',percent(c.erosion)],['Senior BD',c.bd?'Enabled':'Disabled'],['Synthetic market stress',c.macro?'Enabled':'Disabled'],['Success-fee multiplier',percent(c.success_gate)],['Annual attrition',percent(project.assumptions.attrition??project.levels[0].attrition)],['Discount rate for NPV','10% annually']];
  $('assumption-list').innerHTML=rows.map(([k,v])=>`<div class="assumption-row"><span>${esc(k)}</span><strong>${esc(v)}</strong></div>`).join('')+'<p class="footnote">The baseline uses the same hiring, market and success-fee assumptions, with no AI, no additional BD, the original fee mix and unchanged opportunity flow.</p>';
  $('provenance').innerHTML=`<p>${state.display.live?'Verified execution through the local application API.':'Saved-example evidence comes from the previously verified 34-scenario pilot.'} The chart shows annual results. NPV uses monthly incremental operating cash at 10%, with no added initial cost. Payback is undiscounted first sampled recovery and can reverse later. Operating cash excludes financing, taxes and capex.</p><pre>${esc(JSON.stringify(state.display.provenance,null,2))}</pre>`;
}
const validationWorkspace=initValidation(()=>state.display);
function setView(view){
  state.view=view;
  if(view==='validation'){stopPlayback();validationWorkspace.open();}else validationWorkspace.close();
  document.querySelector('.year-control').hidden=view==='validation';
  for(const name of ['overview','workforce','assumptions','validation']){$(name).hidden=name!==view;const tab=$('tab-'+name);tab.classList.toggle('active',name===view);if(name===view)tab.setAttribute('aria-current','page');else tab.removeAttribute('aria-current');}
}
function choosePreset(key){
  writeControls(presets[key]);$('preset').value=key;
  if(state.preview?.examples[key])state.display={...state.preview.examples[key],live:false};
  error('');status('Verified saved example. Run to execute this comparison through the local engine.');render();
}
function stopWatch(){state.watch?.();state.watch=null;clearInterval(state.poll);state.poll=null;}
async function loadRun(job){
  if(state.loading)return;state.loading=true;$('cancel').hidden=true;$('cancel-top').hidden=true;
  try{
    status('Verified execution complete. Loading full-resolution comparisons…');
    const metrics=state.description.metrics.map(m=>m.id);
    const base={baseline_run:job.id,candidate_run:job.id,baseline_scenario:0,candidate_scenario:1,replication:0,metrics};
    const annual=[];
    for(let year=1;year<=5;year++){
      annual.push(await api.compare({...base,from:(year-1)*12,to:year*12}));
      status(`Preparing verified annual results · ${year} of 5 years`);
    }
    const valued=await api.compare({...base,metrics:['cash_flow'],from:0,to:60,
      valuation:{metric:'cash_flow',annual_discount_rate:.1,time_units_per_year:12,initial_incremental_cash_flow:0}});
    const controls=job.effective_parameters.find(s=>s.id===1).parameters;
    state.display={controls,annual,valuation:valued.valuation,live:true,cacheHit:!!job.cache_hit,
      provenance:{...annual[0].candidate_provenance,baseline_scenario:0,candidate_scenario:1,seed:job.request.experiment.seed,replications:1,calculation:'Shared application metrics v1.0; full resolution',synthetic:true}};
    state.job=job;
    sessionStorage.removeItem('tr-active-run');sessionStorage.setItem('tr-last-run',job.id);
    status(job.cache_hit?'Verified completed result reused. No new simulation was needed.':'Comparison complete. Native checks and saved-result verification passed.');
    setBusy(false);render();
  }catch(e){error(`Results could not be loaded: ${e.message}. Previous results remain displayed.`);status('Result loading failed. Run again to retry verified result access.');setBusy(false);}
  finally{state.loading=false;}
}
async function receive(job){
  state.job=job;
  if(job.status==='completed'){stopWatch();
    if(state.display?.live && state.display.provenance.run_id===job.id && sameControls(state.controls,state.display.controls)){
      state.display.cacheHit=!!job.cache_hit;setBusy(false);status('Verified completed result reused. No new simulation was needed.');evidence();return;
    }
    await loadRun(job);return;}
  if(['failed','cancelled','interrupted'].includes(job.status)){
    stopWatch();sessionStorage.removeItem('tr-active-run');setBusy(false);
    status(job.status==='cancelled'?'Execution cancelled. Previous results remain displayed.':`Execution ${job.status}. Previous results remain displayed.`);
    if(job.error)error(typeof job.error==='string'?job.error:job.error.message??JSON.stringify(job.error));evidence();return;
  }
  status({queued:'Queued for the local engine…',cancelling:'Cancelling execution…',executing:'Executing baseline and scenario in the native engine…',verifying:'Verifying saved results and accounting checks…'}[job.stage]??`Engine stage: ${job.stage}`);
}
function watch(job){
  stopWatch();state.watch=api.watch(job.id,receive,()=>status('Connection interrupted. Polling the execution status…'));
  state.poll=setInterval(async()=>{try{await receive(await api.status(job.id));}catch(e){status(`Status unavailable: ${e.message}. Retrying…`);}},4000);
}
async function run(event){
  event.preventDefault();if(state.busy||!state.description)return;
  error('');stopPlayback();state.job=null;setBusy(true);status('Submitting a matched baseline and scenario…');
  try{
    const job=await api.run({model_id:state.description.id,model_version:state.description.model_version,
      scenarios:[{id:0,overrides:baselineFor(state.controls)},{id:1,overrides:state.controls}],seed:20260930,replications:1,threads:2,require_check:true});
    state.job=job;for(const id of ['cancel','cancel-top'])$(id).disabled=false;sessionStorage.setItem('tr-active-run',job.id);
    if(job.status==='completed')await receive(job);else{watch(job);await receive(job);}
  }catch(e){setBusy(false);error(`The comparison could not start: ${e.message}`);status('No new results were produced. Your draft has been preserved.');}
}
function stopPlayback(){clearInterval(state.playing);state.playing=null;$('play').textContent='▶';$('play').setAttribute('aria-label','Play annual results');}
function changeYear(value){state.year=Number(value);render();}
function download(){
  const payload={project:'T&R Decision Lab',synthetic:true,source:state.display.live?'verified_api_run':'verified_saved_example',
    controls:state.display.controls,annual:state.display.annual,valuation:state.display.valuation,provenance:state.display.provenance,
    current_draft_differs:!!dirty(),note:'Export describes displayed results. Draft changes are not simulated.'};
  const link=document.createElement('a');const url=URL.createObjectURL(new Blob([JSON.stringify(payload,null,2)],{type:'application/json'}));
  link.href=url;link.download='tr-comparison.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
function explain(){
  if(!state.display)return;
  const comparison=state.display.annual[state.year-1];
  const keys=[['earned_tm','Hourly fees'],['earned_fixed','Fixed fees'],['earned_retainer','Earned retainer services'],['earned_success','Expected success fees'],['disallowed','Less: approval disallowance'],['revenue','Recognized revenue']];
  $('explanation-content').innerHTML=`<div class="explanation-summary"><div class="eyebrow">YEAR ${state.year} · ANNUAL RECOGNITION</div><h3>${money(metricValue(comparison,'revenue'))} in recognized revenue</h3><p>Hourly fees follow delivered hours. Fixed fees preserve contracted value, with price pass-through on new work. Success fees are probability-weighted expectations.</p><table><thead><tr><th>Fee basis</th><th>Baseline</th><th>Your scenario</th></tr></thead><tbody>${keys.map(([key,label])=>`<tr><td>${label}</td><td>${money(metricValue(comparison,key,'baseline'))}</td><td>${money(metricValue(comparison,key))}</td></tr>`).join('')}</tbody></table><p>Each amount uses the shared API’s full-resolution aggregation rules. Holdbacks and collection delays change cash timing; approval disallowance reduces revenue.</p><p>This is an accounting breakdown, not causal attribution of the difference between scenarios.</p></div><details><summary>View the calculation evidence</summary><pre>${esc(JSON.stringify({interval:comparison.interval,calculation_version:comparison.calculation_version,values:comparison.values.filter(v=>keys.some(([key])=>key===v.metric)),provenance:state.display.provenance},null,2))}</pre></details>`;
  $('explanation-dialog').showModal();
}
async function boot(){
  try{const response=await fetch('/preview.json');if(!response.ok)throw new Error('Saved examples unavailable');state.preview=await response.json();state.project=state.preview.project;choosePreset('ai');}
  catch(e){error(e.message);status('Connect to the API to create the first comparison.');}
  try{
    const [description,projectResponse]=await Promise.all([api.describe('ankura_tr'),fetch('/project.json')]);
    state.description=description;state.project=await projectResponse.json();
    state.previewCompatible=!!state.preview?.input_sha256&&Object.entries(state.preview.input_sha256).every(([name,hash])=>description.input_sha256[name]===hash);
    const fixed=description.parameters.find(p=>p.id==='fixed_shift');$('fixed').max=String(fixed.maximum*100);
    $('fixed-help').textContent=`${percent(state.project.fixed_share)} today · ${percent(state.project.fixed_ceiling)} eligibility ceiling. Existing contracts keep their terms.`;
    $('connection').textContent='Local engine connected';$('run').disabled=false;render();
    const id=sessionStorage.getItem('tr-active-run')??sessionStorage.getItem('tr-last-run');
    if(id){
      try{const job=await api.status(id);
        if(job.model_version===description.model_version&&job.effective_parameters.some(s=>s.id===1)){
          writeControls(job.effective_parameters.find(s=>s.id===1).parameters);$('preset').value='custom';
          if(!['failed','cancelled','interrupted'].includes(job.status)){setBusy(true);if(job.status!=='completed')watch(job);await receive(job);}
        }
      }catch{sessionStorage.removeItem('tr-active-run');sessionStorage.removeItem('tr-last-run');}
    }
  }catch(e){$('connection').textContent='Saved examples only';$('connection').classList.add('offline');status(`Local engine unavailable: ${e.message}. Saved examples are still available.`);}
}
$('scenario-form').addEventListener('submit',run);
$('preset').addEventListener('change',e=>choosePreset(e.target.value));
for(const id of ['ai','demand','policy','erosion','fixed','bd','macro','success'])$(id).addEventListener('input',()=>{
  state.controls=readControls();$('preset').value='custom';controlLabels();evidence();
  if(!state.busy)status(dirty()?'Draft updated. Run comparison to calculate these assumptions.':'Controls match the displayed result.');
});
for(const id of ['cancel','cancel-top'])$(id).addEventListener('click',async()=>{if(!state.job)return;try{await receive(await api.cancel(state.job.id));}catch(e){error(`Cancellation failed: ${e.message}`);}});
for(const button of document.querySelectorAll('[data-view]'))button.addEventListener('click',()=>setView(button.dataset.view));
for(const button of document.querySelectorAll('[data-chart]'))button.addEventListener('click',()=>{state.chart=button.dataset.chart;document.querySelectorAll('[data-chart]').forEach(b=>b.classList.toggle('active',b===button));drawChart();});
$('year').addEventListener('change',e=>{stopPlayback();changeYear(e.target.value);});$('scrub').addEventListener('input',e=>{stopPlayback();changeYear(e.target.value);});
$('play').addEventListener('click',()=>{if(state.playing){stopPlayback();return;}if(state.year===5)changeYear(1);$('play').textContent='Ⅱ';$('play').setAttribute('aria-label','Pause annual results');state.playing=setInterval(()=>{if(state.year>=5){stopPlayback();return;}changeYear(state.year+1);},1200);});
$('provenance-link').addEventListener('click',()=>setView('validation'));$('export').addEventListener('click',download);$('explain').addEventListener('click',explain);$('close-explanation').addEventListener('click',()=>$('explanation-dialog').close());
new ResizeObserver(entries=>{if(entries[0].contentRect.width>0&&state.display)drawChart();}).observe($('chart'));
if(location.pathname==='/analysis')setView('validation');
boot();
