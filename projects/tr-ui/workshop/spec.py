"""Project-owned assumptions and contract for the T&R operating model."""
import copy
from application.fathom_service.contracts import fields, require, number, integer

LEVELS = [
    ('Analyst',18,250,85000),('Consultant',27,350,110000),('Senior consultant',30,450,145000),
    ('Manager',24,600,185000),('Director',18,800,240000),('Senior director',12,1000,310000),
    ('Managing director',6,1200,420000),('Support',15,0,80000)]
# id, label, default, minimum, maximum, unit, group, explanation
ROWS = [
('days','Calendar days',90,1,365,'day','Experiment','Starts on a Monday; weekends have no work or arrivals.'),
('seed','Random seed',42,0,1000000,'integer','Experiment','Philox streams are addressed by employee, day and opportunity. Reuse seeds for paired comparisons.'),
('sd_dt','Maximum financial integration step',1,.05,4,'hour','Experiment','Financial SD steps stop at every event. Observation pace does not change business behavior.'),
('arrival_min','Earliest arrival',8,0,12,'hour of day','People','Daily uniform arrival time for each employee.'),
('arrival_max','Latest arrival',9.5,0,12,'hour of day','People','Daily uniform arrival time for each employee.'),
('leave_min','Earliest departure',16.5,12,24,'hour of day','People','Daily uniform departure time. Unfinished work pauses overnight.'),
('leave_max','Latest departure',18,12,24,'hour of day','People','Daily uniform departure time for each employee.'),
('secondary_skill','Second skill coverage',.35,0,1,'fraction','People','Each employee has one of three practice skills and may have a second. Skills are liquidity, restructuring and operations.'),
('liquidity_skill','Primary liquidity skill share',.3333333333,0,1,'fraction','People','Remaining employees after liquidity and restructuring shares have operations as their primary skill.'),
('restructuring_skill','Primary restructuring skill share',.3333333333,0,1,'fraction','People','Together with liquidity share must not exceed 1. Assignments are sampled, not exact quotas.'),
('skill_variation','Productivity variation',.2,0,.5,'fraction','People','Employee productivity is uniform from 1 − variation to 1 + variation.'),
('admin_fraction','Administration time',.15,0,.5,'fraction of shift','People','Blocked at the beginning of each shift; support staff remain support-only.'),
('prospects_day','Prospects per working day',8,0,100,'opportunities/day','Demand','Poisson arrivals between 08:00 and 18:00. No opening pipeline or backlog.'),
('market_growth','Annual demand growth',0,-.8,1,'fraction/year','Demand','Exponential market multiplier; a scenario assumption, not a forecast.'),
('win_rate','Opportunity win probability',.4,0,1,'fraction','Demand','Independent win draw; a won engagement enters the analysis queue immediately.'),
('bd_fraction','Senior idle time used for BD',.25,0,1,'fraction','Demand','Directors and above use this fraction of otherwise idle time; delivery takes priority.'),
('bd_conversion','Additional prospects per BD hour',.01,0,.2,'opportunities/hour','Demand','Past senior BD hours add to arrival intensity after the specified calendar-day lag.'),
('bd_lag','Business development lag',14,1,90,'calendar days','Demand','Delayed conversion from BD activity to opportunity intensity.'),
('analysis_hours','Analysis effort',240,.1,1000,'standard hours/project','Work','Analysts through senior consultants perform analysis. One employee per stage at a time.'),
('plan_hours','Plan and implementation effort',80,.1,1000,'standard hours/project','Work','Managers and directors perform the second stage.'),
('review_hours','Senior review effort',24,.1,1000,'standard hours/project','Work','Senior directors and MDs perform final review.'),
('liquidity_work','Liquidity engagement share',.3333333333,0,1,'fraction','Work','Remaining engagements after liquidity and restructuring shares require operations skill.'),
('restructuring_work','Restructuring engagement share',.3333333333,0,1,'fraction','Work','Together with liquidity engagement share must not exceed 1.'),
('complexity','Project size variation',.5,0,1,'log standard deviation','Work','Mean-one lognormal multiplier shared by all stages of a project.'),
('fixed_share','Fixed-fee project share',.15,0,1,'fraction','Economics','Exploratory contract mix; eligibility must be agreed before business use.'),
('fixed_fee','Fixed price per standard project',150000,0,1000000,'USD/project','Economics','Scales with project size. Recognized only on completion; hourly fees accrue as hours are worked.'),
('realization','Hourly realization',.9,0,1,'fraction','Economics','Actual service hours × employee standard rate × realization.'),
('cost_load','Benefits and bonus load',.4,0,1,'fraction of salary','Economics','Salary plus this load accrues across all calendar hours.'),
('overhead','Annual allocated overhead',2500000,0,20000000,'USD/year','Economics','Excludes taxes, financing and capital expenditure.'),
('license_share','Employees with AI license',.3,0,1,'fraction','AI & training','Exact rounded share of billable employees, selected by a stable random ranking.'),
('usage','Probability of using AI on a stage',.7,0,1,'fraction','AI & training','A licensed assignee makes a stable per-project, per-stage usage draw.'),
('ai_saving','Maximum eligible-task time saving',.35,0,.8,'fraction','AI & training','Actual saving = maximum × stage exposure × proficiency, if AI is used.'),
('analysis_exposure','Analysis AI exposure',1,0,1,'fraction','AI & training','Share of analysis work eligible for the configured AI saving.'),
('plan_exposure','Planning AI exposure',.5,0,1,'fraction','AI & training','Share of planning work eligible for the configured AI saving.'),
('review_exposure','Review AI exposure',0,0,1,'fraction','AI & training','Share of review work eligible for savings; added review effort is modeled separately.'),
('review_extra','Additional review effort from AI',.15,0,1,'fraction','AI & training','Extra senior review work = normal review effort × this factor × earlier-stage AI work share.'),
('initial_proficiency','Initial AI proficiency',.15,0,1,'fraction','AI & training','Initial proficiency of licensed staff; effectiveness is separate from license ownership.'),
('training_hours','Weekly AI training',2,0,10,'hours/person/week','AI & training','Split equally across five working days and blocked after administration.'),
('training_gain','Learning rate per training hour',.08,0,.5,'1/hour','AI & training','Proficiency approaches 1 exponentially. Training method scales this assumed learning rate.'),
('learning_by_use','Learning rate per AI work hour',.005,0,.05,'1/hour','AI & training','Proficiency gained through actual use affects later assignments.'),
('license_cost','AI license monthly cost',30,0,500,'USD/license/month','AI & training','Accrued over calendar time for licensed employees.'),
('training_cost','Training delivery cost',75,0,1000,'USD/attendee-hour','AI & training','Additional cost; employee salary and delivery time are accounted separately.'),
]
PARAMETERS=[dict(id=k,label=l,default=d,minimum=lo,maximum=hi,unit=u,group=g,description=x,
                status='scenario setting' if g=='Experiment' else 'assumed',role='setting' if g=='Experiment' else 'decision' if k in ('license_share','training_hours','bd_fraction') else 'uncertain assumption',validation={'People':'HR roster and working patterns','Demand':'CRM cohorts, opportunity and referral history','Work':'Project task hours and review records','Economics':'Contracts, finance and payroll','AI & training':'Pilot observations or sensitivity analysis','Experiment':'Reproducibility checks'}[g])
            for k,l,d,lo,hi,u,g,x in ROWS]
DEFAULT=dict(parameters={p['id']:p['default'] for p in PARAMETERS},levels=[dict(name=n,count=c,rate=r,salary=s) for n,c,r,s in LEVELS],training='workshop',problem='How can a 150-person T&R practice grow profitably, and under what conditions does AI help?',notes='Illustrative workforce, work and commercial assumptions. Replace or constrain with evidence as it becomes available.')
METHODS=[
 dict(id='people',name='People and AI behavior',method='ABM',implementation='ankurafathom::abm::SyncPopulation<Employee>',rule='Individual role, primary/secondary skills, productivity, license and proficiency. Shift and task events change employee state. Proficiency = 1 − (1 − prior) × exp(−learning rate × hours).',evidence='Assumed. Validate role counts against HR; training and AI effects need separate evidence.'),
 dict(id='delivery',name='Engagement delivery',method='DES',implementation='ankurafathom::devs::Simulator',rule='Poisson prospects → win draw → analysis → plan → review → complete. FIFO among tasks compatible with each available employee. Interrupted stages resume before new work. Tasks pause outside shifts.',evidence='Assumed. Validate task effort, waiting and completions against project histories.'),
 dict(id='finance',name='Revenue and operating costs',method='SD',implementation='ankurafathom::sd::Model (Euler)',rule='Hourly revenue rate = sum of busy hourly-contract employee rates × realization. Costs accrue over calendar time; training adds cost while active. Fixed fees enter the revenue stock at completion.',evidence='Accounting checks verify implementation. Contract recognition conventions need business review.'),
 dict(id='feedback',name='Capacity, learning and demand',method='Hybrid',implementation='Project-owned coupling over native ABM / DES / SD',rule='Agent skills determine task eligibility; AI changes task duration; service events change financial rates. Senior idle BD hours increase future prospect intensity after a lag.',evidence='BD conversion, AI savings and learning coefficients remain assumptions; scan them in experiments.')]
LIMITS=['No empirical Ankura calibration yet; every business input is an assumption.', 'One worker per stage, three task stages and three skill domains; no collaborative teams or case-specific workflows.', 'Fixed workforce during each run: hiring, attrition, promotions and acquisitions are not implemented in this operating model.', 'No court approvals, holdbacks, receivables, success fees or cash-flow forecast; the separate monthly model retains its richer financial rules.', 'No opening backlog. Short runs include a startup transient; results are not steady-state estimates.', 'Training method multipliers (self-study 0.5, workshop 1, coaching 1.5) are hypothetical, not measured efficacy.']

def validate(value):
    fields(value,DEFAULT,DEFAULT)
    require(isinstance(value['problem'],str) and 0<len(value['problem'])<=2000,'Problem must contain 1–2000 characters')
    require(isinstance(value['notes'],str) and len(value['notes'])<=10000,'Notes too long')
    require(value['training'] in ('self-study','workshop','coaching'),'Unknown training method')
    fields(value['parameters'],DEFAULT['parameters'],DEFAULT['parameters'])
    for p in PARAMETERS:
        v=value['parameters'][p['id']]
        require(number(v) and p['minimum']<=v<=p['maximum'],'Out of range: '+p['label'])
        if p['id'] in ('days','seed','bd_lag'):require(type(v) is int,'Integer required: '+p['label'])
    p=value['parameters'];require(p['arrival_min']<=p['arrival_max']<p['leave_min']<=p['leave_max'],'Arrival/departure ranges overlap or are reversed')
    require(p['liquidity_skill']+p['restructuring_skill']<=1,'Primary skill shares exceed 100%')
    require(p['liquidity_work']+p['restructuring_work']<=1,'Engagement skill shares exceed 100%')
    require(isinstance(value['levels'],list) and len(value['levels'])==8,'Eight levels required')
    for i,l in enumerate(value['levels']):
        fields(l,('name','count','rate','salary'),('name','count','rate','salary'))
        require(l['name']==LEVELS[i][0],'Level names/order are fixed')
        require(integer(l['count'],0,500),'Employee counts must be integers from 0 to 500')
        require(number(l['rate']) and 0<=l['rate']<=5000,'Invalid hourly rate')
        require(number(l['salary']) and 0<=l['salary']<=2000000,'Invalid salary')
    require(1<=sum(l['count'] for l in value['levels'])<=500,'Total workforce must be 1–500')
    return copy.deepcopy(value)

METRIC_UNITS=dict(ai_users='person',ai_use_share='1',headcount='person',prospects='opportunity',won='engagement',completed='engagement',backlog='engagement',busy='person',on_shift='person',licensed='person',proficiency='1',revenue='USD',cost='USD',contribution='USD',utilization='1',delivery_hours='hour',training_hours='hour',bd_hours='hour',mean_cycle_hours='hour',mean_wait_hours='hour',**{f'headcount_level{i}':'person' for i in range(8)})

def catalog():return dict(version='tr-operating-1',default=DEFAULT,parameters=PARAMETERS,methods=METHODS,limitations=LIMITS,metric_units=METRIC_UNITS)
