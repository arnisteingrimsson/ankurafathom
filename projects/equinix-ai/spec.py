"""Assumption register for a fictional Equinix AI ecosystem demonstration."""
import copy
from application.fathom_service.contracts import fields,require,number,integer
ROWS=[
('days','Horizon',120,1,365,'days','Run','Calendar-day model; all teams are effective concurrent service lanes, not shift rosters.'),
('seed','Random seed',42,0,1000000,'integer','Run','Independent Philox streams for customer traits, arrivals, acceptance and rework.'),
('sd_dt','Maximum SD step',.25,.01,1,'day','Run','Financial and energy integration stops at each event. Display pace is independent.'),
('arrivals','AI opportunities',.8,0,10,'per day','Commercial','Poisson arrivals. Starts with no installed customers or pipeline.'),
('win_rate','Commercial acceptance probability',.75,0,1,'fraction','Commercial','Base win draw after design, before capacity reservation. Price and patience also matter.'),
('patience','Time until customer walks away',45,5,180,'days','Commercial','Customer-specific deadline sampled from 0.75–1.25 × this value.'),
('provider_lock','Customers requiring their preferred provider',.35,0,1,'fraction','Commercial','Other customers permit modeled substitutes; not a claim of software portability.'),
('enterprise_share','Enterprise AI opportunity share',.4,0,1,'fraction','Commercial','Remainder after enterprise and high-density shares is distributed inference.'),
('dense_share','High-density opportunity share',.3,0,1,'fraction','Commercial','Requires liquid-capable rack positions; deployment sizes are 3–6 blocks.'),
('it_load','Actual IT draw / reserved power',.65,.1,1,'fraction','Infrastructure','Constant load assumption after activation, independent of unused reserved capacity.'),
('site_lock','Customers requiring their preferred site',.25,0,1,'fraction','Commercial','Other customers may accept another fictional site if alternatives are enabled.'),
('alternatives','Offer compatible alternatives',1,0,1,'0 / 1','Commercial','Try other sites/providers when the preferred combination is not feasible.'),
('qualifiers','Qualification lanes',2,1,20,'concurrent jobs','Delivery','Effective teams; a lane remains occupied until qualification finishes.'),
('designers','Solution-design lanes',2,1,20,'concurrent jobs','Delivery','Represents scarce technical presales resources.'),
('installers','Installation lanes',2,1,20,'concurrent jobs','Delivery','Shared commissioning teams across sites.'),
('qualification_days','Qualification duration',1,0.1,20,'days','Delivery','Per opportunity; no automatic rejection until the stage finishes.'),
('design_days','Solution-design duration',4,.1,30,'days','Delivery','Varies with customer deployment size.'),
('design_saving','AI-assisted design time saving',0,0,.8,'fraction','Delivery','Hypothetical reduction; does not imply a deployed Prism integration.'),
('design_rework','AI design rework probability',.1,0,1,'fraction','Delivery','Only applied when design assistance is on; rework adds half the original design duration.'),
('installation_days','Installation duration per block',1,.1,10,'days','Delivery','A single lane commissions one customer deployment, scaled by block count.'),
('upgrade','Upgrade Site A',0,0,1,'0 / 1','Infrastructure','Adds 800 kW IT capacity, raises rack density to at least 80 kW and adds 12 liquid-capable rack positions at the upgrade event.'),
('upgrade_day','Upgrade completion day',45,1,300,'day','Infrastructure','An investment pulse and capacity change occur on this date when enabled.'),
('upgrade_cost','Upgrade investment',2000000,0,20000000,'USD','Infrastructure','Illustrative cash investment, separate from operating contribution.'),
('colo_price','Colocation price',250,1,1000,'USD / reserved kW / month','Economics','Revenue starts on activation. A model month is 30 days.'),
('electricity','Electricity price',.12,0,2,'USD / kWh','Economics','Energy cost follows modeled IT draw × site PUE.'),
('overhead_day','Site overhead',3000,0,50000,'USD / day','Economics','Shared across all three fictional sites.'),
('team_cost','Cost per service lane',600,0,5000,'USD / day','Economics','Qualification, design and installation lanes; calendar-day cost assumption.'),
]
PARAMETERS=[dict(id=k,label=l,default=d,minimum=lo,maximum=hi,unit=u,group=g,description=x,status='assumed' if g!='Run' else 'setting') for k,l,d,lo,hi,u,g,x in ROWS]
NAMES=['NVIDIA','AMD','Qualcomm','Tenstorrent','Other partner']
DEFAULT=dict(problem='Which combination of partner options, site capacity and delivery resources turns AI demand into operating customers?',notes='Fictional three-site metro. All operating, performance and price inputs are illustrative. Provider profiles start equal to avoid an unsupported vendor ranking.',parameters={p['id']:p['default'] for p in PARAMETERS},sites=[dict(name='Metro A · Enterprise',power_kw=800,max_rack_kw=40,racks=40,liquid_racks=0,pue=1.4),dict(name='Metro B · AI ready',power_kw=1200,max_rack_kw=80,racks=40,liquid_racks=12,pue=1.3),dict(name='Metro C · Expansion',power_kw=1800,max_rack_kw=120,racks=50,liquid_racks=32,pue=1.25)],providers=[dict(name=n,enabled=True,power_kw=40,lead_days=14) for n in NAMES])
PRESETS=[dict(id='baseline',label='Starting market',description='Current assumptions with no intervention.',changes={}),dict(id='pipeline',label='More AI pipeline',description='Double opportunity arrivals; keep delivery and infrastructure unchanged.',changes={'arrivals':1.6}),dict(id='presales',label='Faster solution design',description='Four design lanes plus a hypothetical 30% AI time saving.',changes={'designers':4,'design_saving':.3}),dict(id='upgrade',label='Prepare AI capacity',description='Upgrade Site A on day 45 and add one installation lane.',changes={'upgrade':1,'installers':3}),dict(id='alternatives',label='Preferred options only',description='Restrict proposals to the preferred site/provider and inspect feasibility queues.',changes={'alternatives':0}),dict(id='combined',label='Coordinated growth',description='More pipeline, four design lanes, three install lanes, AI design assistance and the Site A upgrade.',changes={'arrivals':1.6,'designers':4,'installers':3,'design_saving':.3,'upgrade':1})]
METHODS=[dict(name='Customer opportunity agents',method='ABM',implementation='ankurafathom::abm::SyncPopulation<Customer>',rule='Individual type, deployment size, provider/site preference, substitution rules, budget and deadline. Agents accept, wait, activate or leave.'),dict(name='Commercial and deployment workflow',method='DES',implementation='ankurafathom::devs::Simulator',rule='Qualification → design → capacity reservation → equipment arrival → installation → activation. Shared lanes create queues; deadlines release reservations.'),dict(name='Energy and economics',method='SD',implementation='ankurafathom::sd::Model, Euler',rule='Integrate recurring colocation revenue, operating expense and facility energy between events. Upgrade investment enters separately when incurred.'),dict(name='Cross-model interactions',method='Hybrid',implementation='Project-owned native coupling',rule='Customer requirements constrain infrastructure; activation changes demand for power and recurring revenue; delays affect customer retention.')]
LIMITS=['Fictional sites and synthetic economics. No Equinix operational data or provider benchmarks are used.', 'Provider labels define configurable infrastructure profiles. Equal defaults are placeholders; there is no supported vendor ranking or verified cross-provider software portability.', 'One deployment at one site per customer. Effective concurrent delivery lanes represent teams; detailed staff calendars are not modeled.', 'No outages, churn after activation, contract renewal or network latency simulation.', 'Customer-owned hardware is excluded from operator capital spending. Contribution excludes depreciation, tax and financing; cash proxy subtracts modeled upgrade investment only.', 'Power is reserved IT kW. Actual draw and facility energy are modeled separately; cooling is a compatibility constraint, not CFD.', 'Managed endpoints are meeting context, outside this first model. Prism and Forge are possible future input sources; neither executes in this demo.']
SOURCES=[dict(title='Equinix AI partner ecosystem, including Tenstorrent',url='https://blog.equinix.com/blog/2026/09/24/your-ai-strategy-depends-on-partners-heres-how-to-find-them/',scope='Public context only; no numerical calibration.'),dict(title='Equinix managed AI infrastructure with NVIDIA',url='https://www.equinix.com/partners/nvidia',scope='Public service context only. Other provider names follow the user-supplied ecosystem scope.')]
UNITS=dict(opportunities='customer',active='customer',lost='customer',pipeline='customer',revenue='USD',cost='USD',contribution='USD',capex='USD',cash_proxy='USD',energy_kwh='kWh',reserved_kw='kW',active_kw='kW',it_draw_kw='kW',activation_days='day',qualified='customer',accepted='customer')
def validate(c):
 fields(c,DEFAULT,DEFAULT);require(isinstance(c['problem'],str) and 0<len(c['problem'])<=2000,'Provide a business question');require(isinstance(c['notes'],str) and len(c['notes'])<=10000,'Invalid notes');fields(c['parameters'],DEFAULT['parameters'],DEFAULT['parameters'])
 for p in PARAMETERS:
  v=c['parameters'][p['id']];require(number(v) and p['minimum']<=v<=p['maximum'],'Out of range: '+p['label'])
  if p['id'] in ('days','seed','qualifiers','designers','installers','alternatives','upgrade'):require(type(v) is int,'Integer required: '+p['label'])
 require(c['parameters']['enterprise_share']+c['parameters']['dense_share']<=1,'Customer-type shares exceed 100%')
 require(isinstance(c['sites'],list) and len(c['sites'])==3,'Three sites required')
 for i,s in enumerate(c['sites']):
  fields(s,DEFAULT['sites'][i],DEFAULT['sites'][i]);require(s['name']==DEFAULT['sites'][i]['name'],'Site names/order are fixed');require(number(s['power_kw']) and 0<=s['power_kw']<=20000,'Invalid site power');require(integer(s['racks'],0,500) and integer(s['liquid_racks'],0,s['racks']),'Invalid rack capacity');require(number(s['max_rack_kw']) and 1<=s['max_rack_kw']<=200,'Invalid rack density limit');require(number(s['pue']) and 1<=s['pue']<=2.5,'PUE must be 1–2.5')
 require(isinstance(c['providers'],list) and len(c['providers'])==5,'Five partner profiles required')
 for i,p in enumerate(c['providers']):
  fields(p,DEFAULT['providers'][i],DEFAULT['providers'][i]);require(p['name']==NAMES[i] and type(p['enabled']) is bool,'Invalid partner profile')
  for k,lo,hi in [('power_kw',1,200),('lead_days',0,90)]:require(number(p[k]) and lo<=p[k]<=hi,'Invalid partner '+k)
 require(any(p['enabled'] for p in c['providers']),'Enable at least one provider');return copy.deepcopy(c)
def catalog():return dict(version='equinix-ai-1',default=DEFAULT,parameters=PARAMETERS,presets=PRESETS,methods=METHODS,limitations=LIMITS,sources=SOURCES,metric_units=UNITS)
