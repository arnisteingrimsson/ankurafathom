import {presets,escapeHTML as esc,policyName} from '/ui-model.mjs';
const $=id=>document.getElementById(id);
const state={description:null,project:null,id:null,revision:0,frame:null,metrics:null,history:[],running:false,pending:false,timer:null,selected:'delivery',duration:null,failed:false};
const money=v=>v==null?'—':`${v<0?'−':''}$${(Math.abs(v)/1e6).toFixed(2)}M`;
const count=v=>v==null?'—':v.toLocaleString('en-US',{maximumFractionDigits:1});
const percent=v=>v==null?'—':(v*100).toFixed(1)+'%';
const metricFormats={revenue:money,utilization:percent,headcount:count};
async function request(path,body){const response=await fetch(path,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const value=await response.json();if(!response.ok)throw Error(value.error?.message??'Live request failed');return value;}
const controls=()=>({ai:+$('savings').value/100,demand_scale:1+(+$('growth').value/100),policy_hold:+($('hiring').value==='hold'),policy_responsive:+($('hiring').value==='responsive'),erosion:+$('erosion').value/100,fixed_shift:+$('fixed').value/100,bd:+$('bd').checked,macro:+$('macro').checked,success_gate:+$('success').value/100});
function setControls(c){$('savings').value=c.ai*100;$('growth').value=Math.round((c.demand_scale-1)*100);$('hiring').value=c.policy_hold?'hold':c.policy_responsive?'responsive':'freeze';$('erosion').value=c.erosion*100;$('fixed').value=c.fixed_shift*100;$('bd').checked=!!c.bd;$('macro').checked=!!c.macro;$('success').value=c.success_gate*100;labels();}
function labels(){for(const [input,label]of [['savings','savings'],['growth','growth'],['erosion','erosion'],['success','success']])$(label+'-label').textContent=$(input).value+'%';$('fixed-label').textContent=((state.project?.fixed_share??.15)*100+(+$('fixed').value)).toFixed(0)+'%';}
function status(text){$('live-status').textContent=text;}
function error(text){$('live-error').textContent=text;$('live-error').hidden=!text;}
function buttons(){const done=state.failed||state.frame?.complete||state.frame?.checks_passed===false;
 $('live-play').textContent=state.running?'Ⅱ Pause':(state.failed||state.frame?.checks_passed===false)?'Stopped':done?'Completed':'▶ Play';$('live-play').disabled=!state.description||done||(!state.running&&state.pending);
 $('live-step').disabled=!state.description||state.pending||state.running||done;
 $('live-reset').disabled=(!state.id&&!state.pending)||state.pending;
 $('live-export').disabled=!state.id||state.pending;
 for(const e of $('live-inputs').querySelectorAll('input,select'))e.disabled=!!state.id||state.pending;
 $('live-input-note').textContent=state.id?'Conditions are fixed for this session. Reset to edit them.':'Conditions are applied when you press Play or Step.';
 $('flow-scene').classList.toggle('advancing',state.running||state.pending);
}
function pause(){state.running=false;clearTimeout(state.timer);state.timer=null;status(state.pending?'Pausing at the next step boundary…':state.id?`Paused · month ${state.revision}`:'Ready · press Play');buttons();}
function accept(data){state.id=data.id;state.revision=data.revision;state.frame=data.frame;state.metrics=data.metrics;
 if(data.revision>0&&!state.history.some(p=>p.month===data.revision))state.history.push({month:data.revision,...data.metrics});
 if(data.status==='failed'){state.failed=true;state.running=false;error('A native check failed. Execution has stopped at this month; inspect the checks below.');}
 if(data.frame.complete)state.running=false;
 $('live-identity').textContent=JSON.stringify({session:data.id,model_version:data.model_version,native_runner_sha256:data.runner_sha256,computed_through:data.revision,latest_frame_sha256:data.frame.sha256},null,2);
 render();
}
async function initialize(){state.pending=true;buttons();status('Initializing the native model…');try{accept(await request('/project/live',{model_version:state.description.model_version,overrides:controls()}));}finally{state.pending=false;buttons();}}
async function step(){if(state.pending||state.frame?.complete)return;const began=performance.now();state.pending=true;buttons();status(`Computing month ${state.revision+1}…`);
 try{const value=await request(`/project/live/${state.id}/step`,{expected_revision:state.revision});state.duration=performance.now()-began;accept(value);status(value.status==='completed'?'Complete · 60 months computed':value.status==='failed'?'Stopped · native check failed':state.running?'Running · advancing the native engine':`Paused · month ${state.revision}`);}
 catch(e){state.failed=true;state.running=false;error(e.message);status('Execution stopped · inspect error');}
 finally{state.pending=false;buttons();}
 if(state.running){const delay=Math.max(0,(+$('live-pace').value)-(performance.now()-began));state.timer=setTimeout(step,delay);}
}
$('live-play').addEventListener('click',async()=>{if(state.running){pause();return;}if(state.pending)return;error('');state.running=true;buttons();try{if(!state.id)await initialize();if(state.running)await step();}catch(e){state.running=false;state.pending=false;error(e.message);status('Unable to start');buttons();}});
$('live-step').addEventListener('click',async()=>{if(state.pending)return;error('');try{if(!state.id)await initialize();await step();}catch(e){state.pending=false;error(e.message);buttons();}});
$('live-reset').addEventListener('click',async()=>{pause();if(state.pending)return;state.pending=true;buttons();try{if(state.id)await request(`/project/live/${state.id}/stop`,{});state.id=null;state.frame=null;state.metrics=null;state.history=[];state.revision=0;state.duration=null;state.failed=false;$('live-identity').textContent='Not started';error('');render();status('Ready · press Play');}catch(e){error(e.message);}finally{state.pending=false;buttons();}});
$('live-preset').addEventListener('change',()=>setControls(presets[$('live-preset').value]));
for(const input of $('live-inputs').querySelectorAll('input,select:not(#live-preset)'))input.addEventListener('input',()=>{$('live-preset').value='custom';labels();});
$('live-inputs').addEventListener('submit',e=>e.preventDefault());
$('live-check-toggle').addEventListener('click',()=>{$('live-check-details').hidden=!$('live-check-details').hidden;$('live-check-toggle').textContent=$('live-check-details').hidden?'Inspect checks':'Hide checks';});
const nodeSpecs={
 market:{name:'Opportunities',description:'New opportunities generated during the computed month. Market sensitivity and BD conversion are synthetic assumptions.',fields:[['Prospects','m_prospects','jobs'],['Won engagements','m_won','jobs'],['BD-generated prospects','m_bd_prospects','jobs']]},
 pipeline:{name:'Won work',description:'Expected wins enter the fee-specific workload. The model represents aggregate expected engagements, not individual deals.',fields:[['Won engagements','m_won','jobs'],['Cumulative wins','cum_won','jobs'],['New prospects','m_prospects','jobs']]},
 delivery:{name:'Engagement delivery',description:'Staffing by level constrains completed engagements. Undelivered work remains in backlog.',fields:[['Completed this month','m_delivery','jobs'],['Delivery hours','m_delivery_hours','hours'],['Hourly-billed hours','m_hourly_hours','hours'],['Cumulative completions','cum_delivery','jobs']]},
 revenue:{name:'Fee recognition',description:'Hourly work follows delivered hours; fixed fees, retainers and expected success fees follow their contracted terms.',fields:[['Monthly revenue','m_revenue','USD'],['Hourly fees','m_earned_tm','USD'],['Fixed fees','m_earned_fixed','USD'],['Retainers','m_earned_retainer','USD'],['Expected success fees','m_earned_success','USD'],['Approval disallowance','m_disallowed','USD'],['Monthly EBITDA','m_ebitda','USD']]},
 cash:{name:'Cash & collections',description:'Approval, holdback and collection queues separate recognized revenue from cash. Operating cash is collections less modeled costs.',fields:[['Operating cash this month','m_cash_flow','USD'],['Cash collected','m_collections','USD'],['Modeled costs','m_cost','USD'],['Cumulative operating cash','cum_cash_flow','USD'],['Deposit liability','deposit_liability','USD']]},
 backlog:{name:'Accumulated backlog',description:'Work that remains after this month’s delivery. Each fee basis retains its own workload and commercial terms.',fields:[['Hourly engagements','backlog_tm','jobs'],['Fixed-fee engagements','backlog_fixed','jobs'],['Retainer engagements','backlog_retainer','jobs'],['Success-fee engagements','backlog_success','jobs']]},
 people:{name:'Workforce & capacity',description:'Ending expected FTE by level, after attrition, hiring and promotions. Fractional values represent aggregate expectations.',fields:[]},
 bd:{name:'Business development',description:'Eligible freed senior capacity creates additional prospects with a modeled lag. The conversion is an assumption, not calibrated behavior.',fields:[['Business development hours','m_bd_hours','hours'],['Generated prospects','m_bd_prospects','jobs'],['Cumulative generated prospects','cum_bd_prospects','jobs']]}
};
for(const button of document.querySelectorAll('[data-node]'))button.addEventListener('click',()=>{state.selected=button.dataset.node;inspect();});
function inspect(){const spec=nodeSpecs[state.selected];$('inspector-title').textContent=spec.name;$('inspector-description').textContent=spec.description;document.querySelectorAll('[data-node]').forEach(b=>b.classList.toggle('active',b.dataset.node===state.selected));
 if(!state.frame){$('inspector-values').innerHTML='<div class="empty-inspector">No computed state.<br>Press Play or Step to start.</div>';return;}
 const fields=state.selected==='people'?(state.project?.levels??[]).map((l,i)=>[l.label??l.level??l.id,`l${i}_fte`,'FTE']):spec.fields;
 $('inspector-values').innerHTML=fields.map(([label,key,unit])=>`<div class="inspector-row"><span>${esc(label)}</span><strong>${unit==='USD'?money(state.frame.outputs[key]):count(state.frame.outputs[key])}${unit==='USD'?'':` <small>${esc(unit)}</small>`}</strong><code>${esc(key)} · month ${state.revision}</code></div>`).join('');
}
function chart(metric){const rows=state.history,el=$('chart-'+metric);$('chart-'+metric+'-value').textContent=rows.length?metricFormats[metric](rows.at(-1)[metric]):'—';if(!rows.length){el.innerHTML='<p>No simulated observations.<br>Press Play or Step.</p>';return;}
 const w=310,h=140,left=27,right=9,top=12,bottom=23;const values=rows.map(r=>r[metric]).filter(v=>v!=null),max=metric==='utilization'?1:Math.max(1,...values)*1.1,min=Math.min(0,...values)*1.1;
 const x=m=>left+(w-left-right)*m/60,y=v=>h-bottom-(h-top-bottom)*(v-min)/(max-min);const points=rows.filter(r=>r[metric]!=null);let path=points.map((r,i)=>`${i?'L':'M'}${x(r.month)},${y(r[metric])}`).join(' ');const last=points.at(-1);
 el.innerHTML=`<svg viewBox="0 0 ${w} ${h}" role="img" aria-label="Computed ${metric} through month ${state.revision}"><line x1="${left}" y1="${h-bottom}" x2="${w-right}" y2="${h-bottom}" stroke="#e7ece4"/><line x1="${left}" y1="${top}" x2="${w-right}" y2="${top}" stroke="#f1f4ed"/><text x="0" y="${top+4}">${metric==='revenue'?(max/1e6).toFixed(0)+'M':metric==='utilization'?'100%':Math.round(max)}</text><text x="0" y="${h-bottom+3}">${min<0?(min/1e6).toFixed(1)+'M':'0'}</text><path d="${path}" fill="none" stroke="#328170" stroke-width="2"/>${last?`<circle cx="${x(last.month)}" cy="${y(last[metric])}" r="3.2" fill="#247665"/>`:''}<line x1="${x(state.revision)}" y1="${top}" x2="${x(state.revision)}" y2="${h-bottom}" stroke="#b5cfc0" stroke-dasharray="3 4"/><text x="${left}" y="${h-3}">0</text><text x="${w-right}" y="${h-3}" text-anchor="end">60 months</text></svg>`;
}
function render(){const o=state.frame?.outputs;
 const values=o?{market:count(o.m_prospects),pipeline:count(o.m_won),delivery:count(o.m_delivery),revenue:money(state.metrics.revenue),people:count(state.metrics.headcount),backlog:count(['tm','fixed','retainer','success'].reduce((sum,fee)=>sum+o['backlog_'+fee],0)),bd:count(o.m_bd_prospects),cash:money(state.metrics.cash_flow)}:{};
 for(const key of Object.keys(nodeSpecs))$('node-'+key).textContent=values[key]??'—';
 $('live-clock').textContent=state.id?`Month ${state.revision} / 60`:'Not started';
 $('canvas-note').textContent=state.id?(state.revision?`Computed state · end of month ${state.revision} · ${policyName(controls())}`:'Native initial state · month 1 has not been computed'):'Ready to initialize · no simulation has run';
 $('native-timing').textContent=state.duration?`Last step ${(state.duration/1000).toFixed(2)}s · 1 month computed`:'Step: 1 month · no future state computed';
 $('observation-count').textContent=`${state.history.length} computed month${state.history.length===1?'':'s'}`;
 for(const metric of Object.keys(metricFormats))chart(metric);
 const checks=state.frame?.checks??[];const failed=checks.filter(c=>!c.passed);
 $('live-check-summary').textContent=checks.length?`${checks.length-failed.length} / ${checks.length} native checks passed at month ${state.revision}`:'No checks executed yet';$('live-check-summary').classList.toggle('failed',!!failed.length);
 $('live-check-details').innerHTML=checks.length?`<table><thead><tr><th>Native rule</th><th>Left</th><th>Comparison</th><th>Right</th><th>Absolute gap</th><th>Abs / rel tolerance</th><th>Result</th></tr></thead><tbody>${checks.map(c=>`<tr><td>${esc(c.id)}</td><td>${c.left.toPrecision(8)}</td><td>${esc(c.comparison)}</td><td>${c.right.toPrecision(8)}</td><td>${c.absolute_gap.toPrecision(3)}</td><td>${c.absolute_tolerance} / ${c.relative_tolerance}</td><td>${c.passed?'Pass':'FAIL'}</td></tr>`).join('')}</tbody></table>`:'No checks have run.';
 inspect();buttons();
}
$('live-export').addEventListener('click',async()=>{try{const response=await fetch(`/project/live/${state.id}/record`);if(!response.ok)throw Error('Session export failed');const recordText=await response.text();const url=URL.createObjectURL(new Blob([recordText],{type:'application/json'}));const a=document.createElement('a');a.href=url;a.download='tr-live-session.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}catch(e){error(e.message);}});
document.addEventListener('visibilitychange',()=>{if(document.hidden&&state.running)pause();});
window.addEventListener('pagehide',()=>{clearTimeout(state.timer);if(state.id)fetch(`/project/live/${state.id}/stop`,{method:'POST',headers:{'Content-Type':'application/json'},body:'{}',keepalive:true}).catch(()=>{});});
async function boot(){render();try{const [description,project]=await Promise.all([request('/v1/models/ankura_tr/describe'),request('/project.json')]);state.description=description;state.project=project;$('fixed').max=description.parameters.find(p=>p.id==='fixed_shift').maximum*100;labels();status('Ready · press Play');buttons();}catch(e){error(e.message);status('Native engine unavailable');}}
boot();
