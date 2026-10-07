import {mountWorkspace, unmountWorkspace} from '/workspace.js';
const $ = id => document.getElementById(id);
const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const pretty = value => escape(JSON.stringify(value, null, 2));
const number = value => value == null ? '—' : Number(value).toLocaleString(undefined, {maximumFractionDigits:3});
const date = value => value == null ? 'Not recorded' : new Date(typeof value === 'number' ? value * 1000 : value).toLocaleString();
const pill = (text, cls='') => `<span class="pill ${cls}">${escape(text)}</span>`;
const briefHash = value => value ? value.slice(0,12) : 'Not recorded';
const heading = (label, title, description) => `<div class="heading"><div><div class="eyebrow">${escape(label)}</div><h1>${escape(title)}</h1><p class="lede">${escape(description)}</p></div></div>`;
const button = (text, action, extra='') => `<button class="button" data-action="${action}" ${extra}>${text}</button>`;
const stat = (value,label,detail) => `<div class="card stat"><div class="label">${escape(label)}</div><strong>${escape(value)}</strong><span class="small">${escape(detail)}</span></div>`;
let projects=[], projectId='', project=null, view='overview', sequence=0, runData=null, frameData=null, offset=0;
let configurationMode='parameters', modelMode='register', runMode='summary', traceFilter='';
const views = ['overview','workspace','model','configuration','inputs','runs','validation','runtime','delivery'];

async function api(path, body) {
  const response=await fetch(path, body===undefined ? {} : {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const value=await response.json();
  if(!response.ok) throw new Error(value.error?.message || 'Request failed');
  return value;
}
const base = () => `/api/projects/${encodeURIComponent(projectId)}`;
function notice(message='') { $('notice').textContent=message; }
async function handle(fn) { notice(); try { await fn(); } catch(error) { notice(error.message); } }
function download(name,value) {
  const url=URL.createObjectURL(new Blob([JSON.stringify(value,null,2)],{type:'application/json'}));
  const link=document.createElement('a'); link.href=url; link.download=name; link.click(); setTimeout(()=>URL.revokeObjectURL(url),1000);
}
function route() {
  const hash=location.hash.slice(1).split('/');
  view=views.includes(hash[0]) ? hash[0] : 'overview';
  return view==='runs' && /^[a-z]+-[0-9a-f]{32}$/.test(hash[1]||'') ? hash[1] : null;
}
function chrome() {
  document.querySelectorAll('[data-view]').forEach(link=>link.classList.toggle('active',link.dataset.view===view));
  const r=project.runtime;
  $('connection').className='pill '+(r.status==='online'?'ok':r.status==='port_conflict'?'bad':'');
  $('connection').textContent=r.status==='online'?'Customer UI online':r.status==='starting'?'Service starting':'Customer UI '+r.status;
  $('customer-top').setAttribute('aria-disabled',String(r.status!=='online'));
  if(r.status==='online') $('customer-top').href=r.url; else $('customer-top').removeAttribute('href');
}
async function selectProject(id) {
  unmountWorkspace();
  const ticket=++sequence;
  projectId=id; project=null; runData=null; frameData=null; offset=0;
  $('content').innerHTML='<p class="empty">Reading project configuration and recorded evidence…</p>';
  $('customer-top').removeAttribute('href'); $('customer-top').setAttribute('aria-disabled','true');
  const data=await api(base());
  if(ticket!==sequence) return;
  project=data; $('project').value=id;
  const url=new URL(location.href); url.searchParams.set('project',id); history.replaceState(null,'',url);
  await render();
}
function navigation(viewName) { location.hash=viewName; }
function overview() {
  const c=project.catalog, v=project.verification;
  return heading('WORKSPACE / '+project.client,project.name,project.description)+
    `<div class="grid four">${stat(c.methods.length,'Model components','ABM · DES · SD · hybrid')}${stat(c.parameters.length,'Exposed parameters','Read the rules and declared bounds')}${stat(project.run_count,'Saved runs','Customer and acceptance records')}${stat(v.status==='current'?'Current':v.available?'Review':'No receipt','Technical evidence','Business assumptions remain uncalibrated')}</div>`+
    `<div class="grid two"><div><section class="card accent"><div class="eyebrow">THE PROJECT QUESTION</div><p class="question">${escape(c.default.problem)}</p><p class="small">${escape(c.default.notes)}</p><div class="row space"><a class="button" href="#configuration">Inspect configuration →</a><a class="button primary" href="#workspace">Open visual workspace →</a><a class="button" href="#delivery">Customer application →</a></div></section><section class="card"><div class="row between"><h2>What executes</h2>${pill(c.version,'blue')}</div><div class="model-chain">${c.methods.map(m=>`<button class="model-node" data-action="model"><b>${escape(m.method)}</b><strong>${escape(m.name)}</strong></button>`).join('')}</div><p class="small space">Agent-built configurations describe the assumptions. Native model code supplies the behavior. Both are inspectable.</p></section></div><div><section class="card"><h2>Project readiness</h2><dl class="kv"><dt>Native build</dt><dd>${pill(project.build.ready?'Sources match':'Needs attention',project.build.ready?'ok':'warn')}</dd><dt>Customer application</dt><dd>${pill(project.runtime.status,project.runtime.status==='online'?'ok':'')}</dd><dt>Technical evidence</dt><dd>${pill(v.status,v.status==='current'?'ok':'warn')}</dd><dt>Operational calibration</dt><dd>${pill('Not established','warn')}</dd></dl><p class="small">${escape(v.scope||'No technical verification receipt has been recorded.')}</p><a href="#validation" class="button">Review evidence →</a></section><section class="card"><h2>Latest saved execution</h2>${project.latest_run?`<p>${pill(project.latest_run.origin)} ${pill('Saved: '+project.latest_run.saved_status)}</p><dl class="kv"><dt>Recorded time</dt><dd>${number(project.latest_run.time)} ${escape(project.time_unit)}</dd><dt>Seed</dt><dd>${number(project.latest_run.seed)}</dd><dt>Observation frames</dt><dd>${number(project.latest_run.frames)}</dd></dl><a class="button" href="#runs/${project.latest_run.id}">Inspect run & trace →</a>`:'<p class="small">No saved executions. Open the customer application and run a scenario.</p>'}</section></div></div>`+
    `<section class="card"><div class="row between"><h2>Operator notes</h2><span class="small">Separate from simulation inputs</span></div><textarea id="operator-notes" class="note-editor" aria-label="Operator notes" placeholder="Record questions, next checks and decisions for the project.">${escape(project.notes.text)}</textarea><div class="row between space"><span class="small">${project.notes.updated_at?'Saved '+date(project.notes.updated_at):'No notes recorded.'} Notes do not change the executable model.</span>${button('Save notes','save-notes')}</div></section>`;
}
function tabs(items,current,attribute) { return `<div class="tabs">${items.map(([id,label])=>`<button class="${current===id?'active':''}" data-${attribute}="${id}">${label}</button>`).join('')}</div>`; }
function model() {
  const c=project.catalog;
  return heading('MODEL & INPUTS','Model register','See the algorithms, their responsibilities and how they interact.')+
    tabs([['register','Readable model'],['contract','Model contract JSON'],['source','Native implementation'],['spec','Configuration adapter']],modelMode,'model-mode')+
    (modelMode==='register'?`<div class="grid two"><div>${c.methods.map(m=>`<section class="card"><div class="row between"><h2>${escape(m.name)}</h2>${pill(m.method,'blue')}</div><code>${escape(m.implementation)}</code><p class="space">${escape(m.rule)}</p>${m.evidence?`<div class="callout">${escape(m.evidence)}</div>`:''}</section>`).join('')}</div><aside><section class="card"><h2>Implementation boundary</h2><p class="small">This project uses a native hybrid model with a JSON configuration contract. The JSON is not a complete declarative representation of the algorithms.</p><p class="small">The native source and adapter tabs expose the current implementation. Each saved run retains the source and binary identities used at execution.</p></section><section class="card"><h2>Model boundaries</h2><ul class="list">${c.limitations.map(s=>`<li>${escape(s)}</li>`).join('')}</ul></section><section class="card"><h2>Agent construction traces</h2><p class="small">No AI-agent authoring or tool-call trace has been registered for this project. The execution traces available here are native simulation observations.</p></section></aside></div>`:modelMode==='contract'?`<section class="card"><div class="row between"><h2>Self-describing project contract</h2>${button('Export contract JSON','export-contract')}</div><pre class="json">${pretty(c)}</pre></section>`:'<section id="source-view" class="card"><p class="empty">Reading registered source…</p></section>');
}
function tree(value,label='definition',depth=0) {
  if(value===null || typeof value!=='object') return `<div class="tree-leaf"><span class="tree-key">${escape(label)}:</span> <span class="tree-value">${escape(JSON.stringify(value))}</span></div>`;
  const entries=Object.entries(value);
  return `<details ${depth<2?'open':''}><summary>${escape(label)} <span class="muted">${Array.isArray(value)?'[':'{'}${entries.length}${Array.isArray(value)?']':'}'}</span></summary>${entries.map(([k,v])=>tree(v,k,depth+1)).join('')}</details>`;
}
function parameterRows(filter='') {
  const q=filter.toLowerCase();
  return project.catalog.parameters.filter(p=>[p.id,p.label,p.group,p.description].join(' ').toLowerCase().includes(q)).map(p=>`<tr><td><b>${escape(p.label)}</b><div class="small mono">${escape(p.id)}</div></td><td class="metric">${number(project.catalog.default.parameters[p.id])}</td><td>${escape(p.unit)}</td><td class="metric">${number(p.minimum)} … ${number(p.maximum)}</td><td>${pill(p.status||'Assumed',p.status==='assumed'?'warn':'')}<div class="small">${escape(p.group)}</div></td><td>${escape(p.description)}${p.validation?`<p class="small">Validate with: ${escape(p.validation)}</p>`:''}</td></tr>`).join('');
}
function configuration() {
  const d=project.catalog.default;
  return heading('MODEL & INPUTS','Configuration','Inspect the current project defaults. A saved run’s configuration is captured separately and remains immutable.')+
    `<div class="row between"><span class="small">Current default · ${escape(project.catalog.version)}</span>${button('Export definition JSON','export-definition')}</div>`+
    tabs([['parameters','Parameters & bounds'],['tree','Structured definition'],['json','Raw JSON']],configurationMode,'configuration-mode')+
    (configurationMode==='parameters'?`<section class="card"><div class="toolbar"><input id="parameter-search" type="search" placeholder="Filter by name, group or rule…" aria-label="Filter parameters"><span class="small">${project.catalog.parameters.length} exposed parameters</span></div><div class="table-scroll"><table><thead><tr><th>Parameter</th><th>Default</th><th>Unit</th><th>Bounds</th><th>Evidence / group</th><th>Rule</th></tr></thead><tbody id="parameter-rows">${parameterRows()}</tbody></table></div></section>`:configurationMode==='tree'?`<section class="card"><div class="tree">${tree(d)}</div></section>`:`<section class="card"><pre class="json">${pretty(d)}</pre></section>`)+
    `<p class="footer-note">Customer applications own scenario editing and execution. Viewing or exporting this definition does not run the model or change a scenario.</p>`;
}
function inputs() {
  return heading('MODEL & INPUTS','Data & assumptions','Make the origin and validation target of each assumption visible before using outcomes.')+
    `<div class="grid two"><div>${project.inputs.map(i=>`<section class="card data-card"><div class="row between"><h2>${escape(i.name)}</h2>${pill(i.status,'warn')}</div><p>${escape(i.description)}</p></section>`).join('')}</div><aside><section class="card"><h2>Evidence boundary</h2><p>These are assumption-first models.</p><p class="small">No empirical calibration dataset has been registered in this control center. Customer applications can compare observations to computed points; a successful technical check is not historical validation.</p><div class="callout">Use actual data to challenge the inputs, distributions, rules and outcomes—not simply to fill a dashboard.</div><a href="#configuration" class="button">Inspect parameter assumptions →</a></section><section class="card"><h2>Current definition notes</h2><p class="small">${escape(project.catalog.default.notes)}</p></section></aside></div>`;
}
function runList(data) {
  return heading('EXECUTION & EVIDENCE','Runs & traces','Inspect recorded executions, their exact configurations and the evidence captured while they ran.')+
    `<div class="callout">Saved status describes the artifact, not whether a process is currently running. Acceptance runs and customer runs are labeled separately.</div>`+
    `<section class="card"><div class="row between"><h2>${data.total} saved runs</h2><span class="small">${data.total?data.offset+1:0}–${Math.min(data.total,data.offset+data.limit)} of ${data.total}</span></div>${data.runs.length?`<div class="table-scroll"><table><thead><tr><th>Run / created</th><th>Source</th><th>Saved status</th><th>Seed</th><th>Computed through</th><th>Frames</th><th>Recorded checks</th></tr></thead><tbody>${data.runs.map(r=>`<tr><td><a href="#runs/${r.id}" class="mono">${escape(r.id.split('-').at(-1).slice(0,12))} →</a><div class="small">${date(r.created_at)}</div></td><td>${pill(r.origin)}</td><td>${escape(r.saved_status)}</td><td class="metric">${number(r.seed)}</td><td class="metric">${number(r.time)} ${escape(data.time_unit)}</td><td class="metric">${number(r.frames)}</td><td>${pill(r.checks_passed===true?'Passed':r.checks_passed===false?'Failed':'Unknown',r.checks_passed===true?'ok':'warn')}</td></tr>`).join('')}</tbody></table></div>`:'<p class="empty">No run records found in the registered project directories.</p>'}<div class="row space">${button('← Previous','previous-runs',data.offset===0?'disabled':'')}${button('Next →','next-runs',data.next_offset==null?'disabled':'')}</div></section>`;
}
function checksTable(checks) {
  return `<div class="table-scroll"><table class="check"><thead><tr><th>Check</th><th>Actual</th><th>Expected</th><th>Tolerance</th><th>Result</th></tr></thead><tbody>${checks.map(c=>`<tr><td>${escape(c.name)}</td><td class="metric">${number(c.actual)}</td><td class="metric">${number(c.expected)}</td><td class="mono">${escape(c.tolerance)}</td><td>${pill(c.passed?'PASS':'FAIL',c.passed?'ok':'bad')}</td></tr>`).join('')}</tbody></table></div>`;
}
function metricsGrid(metrics) {
  const units=project.catalog.metric_units;
  return `<div class="metric-grid">${Object.entries(metrics).map(([k,v])=>`<div><span class="small">${escape(k)} ${units[k]?'· '+escape(units[k]):''}</span><strong>${number(v)}</strong></div>`).join('')}</div>`;
}
function runDetail() {
  const r=runData;
  return heading('EXECUTION / '+r.origin,'Run '+r.id.split('-').at(-1).slice(0,12),'A captured execution, with its own definition and provenance. It does not change when current defaults change.')+
    `<div class="row between"><a href="#runs" class="button">← All runs</a><div class="row">${pill('Saved: '+r.saved_status)}${pill(r.integrity.passed?'Integrity verified':'Integrity failure',r.integrity.passed?'ok':'bad')}<a class="button" href="${base()}/runs/${r.id}/record">Download original record</a></div></div>`+
    tabs([['summary','Run identity'],['config','Captured JSON'],['trace','Observation trace'],['checks','Final checks']],runMode,'run-mode')+
    (runMode==='summary'?`<div class="grid two"><section class="card"><h2>Execution identity</h2><dl class="kv"><dt>Created</dt><dd>${date(r.created_at)}</dd><dt>Model</dt><dd>${escape(r.model)}</dd><dt>Random seed</dt><dd>${number(r.seed)}</dd><dt>Computed through</dt><dd>${number(r.time)} ${escape(r.time_unit)}</dd><dt>Observations</dt><dd>${number(r.frames)}</dd><dt>Definition SHA-256</dt><dd class="mono">${escape(r.config_sha256)}</dd><dt>Runner SHA-256</dt><dd class="mono">${escape(r.runner_sha256)}</dd><dt>Record SHA-256</dt><dd class="mono">${escape(r.record_sha256)}</dd><dt>Artifact</dt><dd class="path">${escape(r.path)}</dd></dl><p class="small">${escape(r.integrity.scope)}</p>${r.integrity.errors.map(e=>`<div class="callout warn">${escape(e)}</div>`).join('')}</section><section class="card"><h2>Models used in this run</h2>${r.methods.map(m=>`<p>${pill(m.method,'blue')} <b>${escape(m.name)}</b></p><p class="small">${escape(m.rule)}</p>`).join('')}<details><summary>${r.source_count} captured source identities</summary><pre class="json">${pretty(r.source_sha256)}</pre></details><details class="space"><summary>Build information</summary><pre class="json">${pretty(r.build)}</pre></details></section></div><section class="card"><h2>Final recorded metrics</h2>${metricsGrid(r.metrics)}</section>`:
      runMode==='config'?`<section class="card"><div class="row between"><h2>Exact run configuration</h2>${button('Export captured definition','export-run-config')}</div><p class="small">Definition ${escape(r.config_sha256)}</p><div class="tree">${tree(r.config)}</div><details class="space"><summary>Raw JSON</summary><pre class="json">${pretty(r.config)}</pre></details></section>`:
      runMode==='checks'?`<section class="card"><h2>Checks at the final observation</h2>${checksTable(r.checks)}<p class="small space">Run list status checks all saved frames. Inspect a particular frame in Observation trace to see its checks.</p></section>`:
      `<section class="card"><h2>Observation trace</h2><p class="small">Browse states already computed by this run. This inspector does not execute or animate a simulation.</p><div class="trace-controls"><label for="frame-index">Recorded frame</label><select id="frame-index">${r.observations.map(f=>`<option value="${f.index}" ${frameData?.index===f.index?'selected':''}>${f.index} · ${number(f.time)} ${escape(r.time_unit)} · ${f.events} recent events</option>`).join('')}</select>${button('← Previous','previous-frame',!frameData||frameData.index===0?'disabled':'')}${button('Next →','next-frame',!frameData||frameData.index===r.frames-1?'disabled':'')}</div><div id="frame-content">${frameData?frameView():'Loading recorded observation…'}</div></section>`);
}
function eventIdentity(e) {
  return e.customer!=null ? e.customer<0?'System':`Customer ${e.customer+1}` : e.id!=null ? e.id<0?'System':`Recorded entity ID ${e.id}` : 'System';
}
function traceEvents() {
  const events=(frameData.frame.events||[]).filter(e=>(eventIdentity(e)+' '+JSON.stringify(e)).toLowerCase().includes(traceFilter.toLowerCase()));
  return `<p class="small">${events.length} of ${(frameData.frame.events||[]).length} retained events</p>`+(events.length?events.map(e=>`<div class="event"><time>${number(e.time)} ${escape(frameData.time_unit)}</time><span class="kind">${escape(e.kind)}<br>${escape(eventIdentity(e))}</span><p>${escape(e.text)}</p></div>`).join(''):'<p class="empty">No matching events in this observation.</p>');
}
function frameView() {
  const f=frameData.frame;
  return `<div class="callout warn">${escape(frameData.scope)} Displayed times are in ${escape(frameData.time_unit)} from the start.</div><div class="row between"><h2>Recent events at ${number(f.time)} ${escape(frameData.time_unit)}</h2>${pill(f.checks_passed?'Checks passed':'Check failure',f.checks_passed?'ok':'bad')}</div><div class="toolbar"><input id="trace-search" type="search" aria-label="Filter recorded events" placeholder="Filter by entity, event or reason…" value="${escape(traceFilter)}"></div><div id="trace-events">${traceEvents()}</div><details class="space"><summary>Metrics at this observation</summary><div class="space">${metricsGrid(f.metrics||{})}</div></details><details class="space"><summary>Checks at this observation</summary>${checksTable(f.checks||[])}</details><details class="space"><summary>Full frame JSON · agents, resources, events and hashes</summary><pre class="json">${pretty(f)}</pre></details><p class="small mono space">Frame SHA-256: ${escape(f.sha256)}</p>`;
}
function validation() {
  const v=project.verification, tests=v.receipt?.tests || v.receipt?.checks || [];
  return heading('EXECUTION & EVIDENCE','Validation','Separate technical correctness, reproducibility and business realism.')+
    `<div class="grid two"><div><section class="card accent"><div class="row between"><h2>Recorded technical verification</h2>${pill(v.status,v.status==='current'?'ok':'warn')}</div><p>${escape(v.scope || v.error || 'No receipt has been recorded.')}</p><dl class="kv"><dt>Receipt outcome</dt><dd>${v.passed===true?'Passed':v.available?'Review receipt':'Not recorded'}</dd><dt>Sources current</dt><dd>${v.sources_current===true?'Yes':v.sources_current===false?'No — changed since receipt':'Not known'}</dd><dt>Recorded at</dt><dd>${date(v.created_at)}</dd><dt>Source identities</dt><dd>${number(v.source_count)}</dd><dt>Receipt</dt><dd class="path">${escape(v.path)}</dd></dl>${v.changed?.length?`<div class="callout warn">Changed sources:<ul>${v.changed.map(s=>`<li>${escape(s)}</li>`).join('')}</ul></div>`:''}${tests.length?`<h3>Recorded checks</h3>${tests.map(t=>`<details class="space"><summary>${escape(t.build||t.command?.join(' ')||'Check')} ${pill(t.passed===true||t.exit_code===0?'PASS':'Review',t.passed===true||t.exit_code===0?'ok':'warn')}</summary><pre class="json">${pretty(t)}</pre></details>`).join('')}`:''}${v.receipt?`<details class="space"><summary>Full verification receipt JSON</summary><pre class="json">${pretty(v.receipt)}</pre></details>`:''}</section></div><aside><section class="card"><h2>What this establishes</h2><p class="small">Specific checks and experiments passed against the source identities in the receipt. Individual run pages verify artifact hashes and the saved frame chain.</p><a class="button" href="#runs">Inspect a recorded execution →</a></section><section class="card"><h2>What remains a hypothesis</h2><p class="small">Demand, costs, behavior and intervention effects remain assumed until validated against appropriate observations. No historical fit or independent holdout result is implied.</p><div class="callout warn">Operational calibration: not established.</div></section></aside></div>`;
}
function runtime() {
  const b=project.build,r=project.runtime;
  return heading('EXECUTION','Runtime','Inspect the native build and explicitly manage this project’s local customer application.')+
    `<div class="grid two"><section class="card"><div class="row between"><h2>Customer application service</h2>${pill(r.status,r.status==='online'?'ok':'')}</div><p>${escape(r.message)}</p><dl class="kv"><dt>Address</dt><dd class="mono">${escape(r.url)}</dd><dt>Process ownership</dt><dd>${r.owned?'Started by this console':'Not owned by this console'}</dd><dt>Last child exit</dt><dd>${r.exit_code??'—'}</dd></dl><div class="row">${button('Start customer service','start-service',r.status==='offline'&&b.ready?'':'disabled')}${button('Stop owned service','stop-service',r.owned?'':'disabled')}</div><p class="small space">Starting a service does not run the simulation. This console stops only processes it starts; other running applications remain independently managed.</p></section><section class="card"><div class="row between"><h2>Native build</h2>${pill(b.status,b.ready?'ok':'warn')}</div><p>${escape(b.message)}</p><dl class="kv"><dt>Runner</dt><dd class="path">${escape(b.runner)}</dd><dt>SHA-256</dt><dd class="mono">${escape(b.sha256||'Not recorded')}</dd><dt>Source identities</dt><dd>${number(b.source_count)}</dd></dl>${b.missing.length?`<p class="small">Missing startup prerequisites:</p><ul class="list">${b.missing.map(x=>`<li>${escape(x)}</li>`).join('')}</ul>`:''}${b.changed.length?`<p class="small">Changed native sources:</p><ul class="list">${b.changed.map(x=>`<li>${escape(x)}</li>`).join('')}</ul>`:''}<h3>Build command</h3><code class="runtime-command">${escape(b.command)}</code><p class="small">Run from the repository root. Builds are not launched automatically by this console.</p></section></div>`;
}
function delivery() {
  const r=project.runtime;
  return heading('APPLICATION','Customer UI','A separate project experience, built around the customer’s question.')+
    `<section class="card blue"><div class="eyebrow">SELECTED PROJECT · ${escape(project.client)}</div><h2 class="delivery-title">${escape(project.name)}</h2><p class="lede">${escape(project.catalog.default.problem)}</p><div class="row space">${r.status==='online'?`<a class="button primary" target="_blank" rel="noopener" href="${r.url}">Open customer UI ↗</a>`:button('Start customer service','start-service',r.status==='offline'&&project.build.ready?'':'disabled')}<a class="button" href="#validation">Review validation</a>${pill(r.status,r.status==='online'?'ok':'')}</div><p class="small space">${escape(r.message)}</p></section><div class="grid four">${[['01','Question & assumptions','The project supplies the problem, controls, initial state and operating rules.'],['02','Live simulation','Play computes new native state. Opening the page does not start a run.'],['03','Inspectable outcomes','Customers explore consequences and the constraints driving them.'],['04','Evidence & provenance','The console exposes models, captured definitions, checks and recorded traces.']].map(([n,h,p])=>`<section class="card"><div class="step-number">${n}</div><h3>${h}</h3><p class="small">${p}</p></section>`).join('')}</div><section class="card"><h2>Platform and project have different jobs</h2><p class="small">AnkuraFathom provides the simulation engine, execution interfaces and this operator console. Each project owns its business model and customer UI. Adding an industry use case does not turn its screens into platform features.</p></section>`;
}
async function render() {
  if(!project) return;
  const ticket=++sequence, runId=route(); chrome();
  unmountWorkspace();
  document.body.classList.toggle('visual-view', view==='workspace');
  if(view==='workspace') {
    await mountWorkspace($('content'),projectId,project,api,()=>ticket===sequence); return;
  }
  const key=projectId;
  if(view==='runs') {
    $('content').innerHTML='<p class="empty">Reading saved execution records…</p>';
    if(runId) {
      const data=await api(base()+'/runs/'+runId);
      if(ticket!==sequence || key!==projectId) return;
      if(runData?.id!==data.id) { runMode='summary'; frameData=null; traceFilter=''; }
      runData=data;
      if(runMode==='trace' && !frameData) frameData=await api(base()+'/runs/'+runId+'/frames/'+(data.frames-1));
      if(ticket!==sequence) return;
      $('content').innerHTML=runDetail();
    } else {
      const data=await api(base()+'/runs?offset='+offset);
      if(ticket!==sequence) return;
      $('content').innerHTML=runList(data);
    }
    return;
  }
  const renders={overview,model,configuration,inputs,validation,runtime,delivery};
  $('content').innerHTML=renders[view]();
  if(view==='model' && ['source','spec'].includes(modelMode)) {
    const source=await api(base()+'/source/'+modelMode);
    if(ticket!==sequence) return;
    $('source-view').innerHTML=`<div class="row between"><h2>${escape(source.path)}</h2>${pill('Current source')}</div><p class="small">${escape(source.scope)}</p><p class="mono">SHA-256 ${escape(source.sha256)}</p><pre class="json source">${escape(source.content)}</pre>`;
  }
}
async function selectFrame(index) {
  const ticket=++sequence,key=projectId,id=runData.id;
  const data=await api(base()+'/runs/'+id+'/frames/'+index);
  if(ticket!==sequence||key!==projectId||view!=='runs') return;
  frameData=data; $('content').innerHTML=runDetail();
}
$('project').onchange=()=>handle(async()=>{history.replaceState(null,'',location.pathname+'?project='+encodeURIComponent($('project').value)+'#overview');await selectProject($('project').value);});
$('refresh').onclick=()=>handle(()=>selectProject(projectId));
window.addEventListener('hashchange',()=>handle(render));
$('content').addEventListener('input',event=>{if(event.target.id==='parameter-search') $('parameter-rows').innerHTML=parameterRows(event.target.value);if(event.target.id==='trace-search'){traceFilter=event.target.value;$('trace-events').innerHTML=traceEvents();}});
$('content').addEventListener('change',event=>{if(event.target.id==='frame-index') handle(()=>selectFrame(Number(event.target.value)));});
$('content').addEventListener('click',event=>handle(async()=>{
  const target=event.target.closest('button'); if(!target) return;
  if(target.dataset.modelMode) { modelMode=target.dataset.modelMode; return render(); }
  if(target.dataset.configurationMode) { configurationMode=target.dataset.configurationMode; return render(); }
  if(target.dataset.runMode) { runMode=target.dataset.runMode; return render(); }
  switch(target.dataset.action) {
    case 'model': return navigation('workspace');
    case 'export-definition': return download(projectId+'-definition.json',project.catalog.default);
    case 'export-contract': return download(projectId+'-contract.json',project.catalog);
    case 'export-run-config': return download(runData.id+'-definition.json',runData.config);
    case 'save-notes': {
      const id=projectId, result=await api(base()+'/notes',{revision:project.notes.revision,text:$('operator-notes').value});
      if(id===projectId) { project.notes=result; notice('Operator notes saved. Simulation inputs are unchanged.'); }
      return;
    }
    case 'previous-runs': offset=Math.max(0,offset-20); return render();
    case 'next-runs': offset+=20; return render();
    case 'previous-frame': return selectFrame(frameData.index-1);
    case 'next-frame': return selectFrame(frameData.index+1);
    case 'start-service': case 'stop-service': {
      const id=projectId, action=target.dataset.action==='start-service'?'start':'stop'; target.disabled=true;
      await api(base()+'/'+action,{}); if(id!==projectId) return;
      await selectProject(id);
      if(project.runtime.status==='starting') setTimeout(()=>{if(id===projectId) handle(()=>selectProject(id));},1500);
      return;
    }
  }
}));
await handle(async()=>{
  projects=(await api('/api/projects')).projects;
  $('project').innerHTML=projects.map(p=>`<option value="${escape(p.id)}">${escape(p.name)}</option>`).join('');
  const requested=new URL(location.href).searchParams.get('project');
  await selectProject(projects.some(p=>p.id===requested)?requested:projects[0].id);
});
