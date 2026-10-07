"""Project-owned evidence and historical evaluation. Never executes a second simulator."""
import json
import math
import re
import sys
import time
from pathlib import Path
from application.fathom_service.contracts import canonical, describe, digest, fields, integer, number, require
from application.fathom_service.core import save

METRICS = {'revenue':'USD', 'ebitda':'USD', 'cash_flow':'USD', 'headcount':'person', 'utilization':'1'}
TIMESTEP = dict(value=1, unit='month', solver='Euler', adjustable=False,
    reason='This model implements monthly state transitions. Hiring, approval and collection queues advance once per step. A smaller step would change those business rules. A time-scaled model revision and new validation are required before a convergence study.',
    convergence='not_applicable_to_this_monthly_transition_model')


def reconcile(results):
    """Read recorded observations, independently compare accounting sides; no eval()."""
    rules=[('ebitda_identity','cum_ebitda',['cum_revenue','cum_cost']),
           ('revenue_identity','cum_revenue',['cum_earned','cum_disallowed']),
           ('cash_identity','cum_cash_flow',['cum_collections','cum_cost'])]
    report=[]
    for name,left,(a,b) in rules:
        worst=None;failures=0;count=0
        for (scenario,replication,output),samples in results.series.items():
            if output!=left:continue
            av=dict(results.series[(scenario,replication,a)]);bv=dict(results.series[(scenario,replication,b)])
            for t,observed in samples:
                expected=av[t]-bv[t];gap=abs(observed-expected);tolerance=2e-6+2e-10*max(abs(observed),abs(expected))
                row=dict(scenario=scenario,replication=replication,time=t,observed=observed,expected=expected,absolute_gap=gap,tolerance=tolerance)
                if worst is None or gap>worst['absolute_gap']:worst=row
                failures+=gap>tolerance;count+=1
        report.append(dict(id=name,expression=f'{left} = {a} - {b}',samples=count,failures=failures,
                           verdict='pass' if count and not failures else 'fail',worst=worst))
    return report


def evaluate_history(results, request):
    fields(request,('run_id','scenario','dataset'),('run_id','scenario','dataset'))
    require(integer(request['scenario'],0,65535),'Invalid scenario')
    data=request['dataset']
    fields(data,('label','source','kind','start_month','calibration_through','heldout_attestation','thresholds','rows'),
           ('label','source','kind','start_month','calibration_through','heldout_attestation','thresholds','rows'))
    for key in ('label','source'):require(isinstance(data[key],str) and 0<len(data[key])<=500,key+' is required (1..500 characters)')
    require(data['kind'] in ('synthetic','observed'),'kind must be synthetic or observed')
    require(isinstance(data['start_month'],str) and re.fullmatch(r'\d{4}-(0[1-9]|1[0-2])',data['start_month']) is not None and not data['start_month'].startswith('0000'),'start_month must be YYYY-MM with a nonzero year')
    require(integer(data['calibration_through'],1,59),'Calibration boundary must be month 1..59')
    require(type(data['heldout_attestation']) is bool,'heldout_attestation must be boolean')
    thresholds=data['thresholds'];require(isinstance(thresholds,dict) and 1<=len(thresholds)<=len(METRICS),'Provide thresholds for 1..5 metrics')
    for metric,rule in thresholds.items():
        require(metric in METRICS,'Unsupported historical metric: '+metric)
        fields(rule,('unit','mae_max'),('unit','mae_max'))
        require(rule['unit']==METRICS[metric],f'{metric} requires unit {METRICS[metric]}')
        require(number(rule['mae_max']) and rule['mae_max']>=0,'MAE threshold must be finite and nonnegative')
    rows=data['rows'];require(isinstance(rows,list) and 2<=len(rows)<=60,'Provide 2..60 consecutive monthly rows')
    observations=[];start=None
    for row in rows:
        fields(row,('month',*thresholds),('month',*thresholds))
        month=row['month'];require(integer(month,1,60),'month must be 1..60')
        if start is None:start=month
        require(month==start+len(observations),'Months must be sorted, unique and consecutive; gaps are rejected')
        values={}
        for metric in thresholds:
            actual=row[metric];require(number(actual),'Actual values must be finite numbers; missing data is not zero')
            if metric in ('headcount','utilization'):require(actual>=0,metric+' cannot be negative')
            if metric=='utilization':require(actual<=1,'Utilization is a fraction in [0,1]')
            predicted=results.value(metric,request['scenario'],0,month-1,month)['value']
            require(predicted is not None,'Selected simulated observation is undefined')
            values[metric]=dict(actual=actual,simulated=predicted,error=predicted-actual)
        observations.append(dict(month=month,partition='calibration' if month<=data['calibration_through'] else 'holdout',values=values))
    require(any(r['partition']=='calibration' for r in observations) and any(r['partition']=='holdout' for r in observations),'Both calibration and later holdout observations are required')
    summaries=[]
    for partition in ('calibration','holdout'):
        subset=[r for r in observations if r['partition']==partition]
        for metric,rule in thresholds.items():
            values=[r['values'][metric] for r in subset];errors=[v['error'] for v in values]
            mae=sum(abs(e) for e in errors)/len(errors);bias=sum(errors)/len(errors)
            rmse=math.hypot(*errors)/math.sqrt(len(errors));den=sum(abs(v['actual']) for v in values)
            wape=sum(abs(e) for e in errors)/den if den else None
            require(all(math.isfinite(v) for v in (mae,bias,rmse)) and (wape is None or math.isfinite(wape)),'Historical error calculation overflow')
            summaries.append(dict(metric=metric,partition=partition,n=len(values),mae=mae,rmse=rmse,bias=bias,wape=wape,
                mae_max=rule['mae_max'],within_threshold=mae<=rule['mae_max'],unit=rule['unit']))
    return dict(dataset_label=data['label'],source=data['source'],kind=data['kind'],start_month=data['start_month'],
        calibration_through=data['calibration_through'],heldout_attestation=data['heldout_attestation'],
        interpretation='Synthetic workflow test; not historical accuracy evidence.' if data['kind']=='synthetic' else
            'Observed-data comparison. Holdout independence is user-attested, not independently verified. No parameters were fitted by this tool.',
        threshold_policy='MAE limits supplied with this evaluation; not preregistered acceptance criteria.',
        holdout_within_threshold=all(r['within_threshold'] for r in summaries if r['partition']=='holdout'),
        observations=observations,summaries=summaries)


class Validation:
    def __init__(self,store,source,state):
        self.store=store;self.source=source;self.state=state;state.mkdir(exist_ok=True,parents=True)

    def catalog(self):
        model=self.store.model('ankura_tr');self.store._verify_inputs(model)
        native=json.loads((model['directory']/'model.json').read_text())
        bound={m['parameter'] for b in native.get('data',[]) if b['use']['kind']=='parameter_table' for m in b['use']['parameters']}
        spec=json.loads((model['directory']/'descriptor.json').read_text())
        # Read-only descriptor used solely to reuse the platform's typed binding resolver.
        # These wide numeric bounds are not exposed as editable parameter domains.
        spec['parameters']=[dict(id=p['id'],label=p['id'],description='Inspectable model default',kind='number',minimum=-sys.float_info.max,maximum=sys.float_info.max,
            status='synthetic',source='Captured parameter table' if p['id'] in bound else 'Native model literal') for p in native['parameters']]
        defaults={p['id']:p for p in describe(native,spec,model['directory'])['parameters']}
        parameters=[dict(p,effective_value=defaults[p['id']]['default'],status='synthetic',source=defaults[p['id']]['source']) for p in native['parameters']]
        with self.store.lock:runs=[self.store.get(k) for k in self.store.jobs]
        runs=sorted((r for r in runs if r['model_id']=='ankura_tr'),key=lambda r:r['created_at'],reverse=True)
        preview=json.loads((Path(__file__).parent/'preview.json').read_text())
        pilot_path=self.source/'validation.json'
        pilot=json.loads(pilot_path.read_text()) if pilot_path.exists() else None
        check_path=Path(__file__).parent/'verification.json'
        project_checks=json.loads(check_path.read_text()) if check_path.exists() else None
        if project_checks:
            project_checks['current_sources_match']=all((check_path.parent/name).is_file() and digest((check_path.parent/name).read_bytes())==value for name,value in project_checks['source_sha256'].items())
        return dict(project_checks=project_checks,model=dict(name=native['name'],mode=native['mode'],time=native['time'],timestep=TIMESTEP,
            components=native['components'],parameters=parameters,checks=native['checks'],data=native['data'],description=model['description']),
            runs=runs,pilot=dict(receipt=pilot,receipt_sha256=digest(pilot_path.read_bytes()) if pilot else None,
                receipt_matches_preview=bool(pilot) and digest(pilot_path.read_bytes())==preview.get('source_validation_sha256'),
                inputs_match_current=all(model['hashes'].get(k)==v for k,v in preview['input_sha256'].items()),
                scope='Recorded 34-scenario synthetic pilot. These checks do not validate every new scenario or real-world forecasts.'),
            agent_provenance=dict(status='not_recorded',model_ids=None,
                explanation='The saved runs contain engine/build identity, inputs and commands. AI author/reviewer model IDs and their work logs were not captured in these artifacts. No model names or review approvals are inferred.'),
            historical_status='No historical accuracy claim. Open Historical fit to evaluate supplied observations.')

    def evidence(self,key):
        results=self.store.results(key);job=self.store.get(key)
        folder=self.store.state/'runs'/key;manifest=json.loads((folder/'manifest.json').read_text())
        native=json.loads(manifest['inputs']['model']['canonical_json'])
        checks=[dict(c,status='passed_during_execution' if job['request']['require_check'] else 'not_run',
                     note='Native require-check execution passed; per-rule residuals were not retained in this live run.') for c in native.get('checks',[])]
        command=[str(self.store.engine),'run',str(self.store.state/'models'/job['model_version']/'model.json'),
                 '--experiment',str(folder/'experiment.json'),'--threads',str(job['request']['threads']),
                 '--out',str(folder/'results.csv'),'--manifest',str(folder/'manifest.json')]
        if job['request']['require_check']:command.append('--require-check')
        return dict(job=job,captured_model=native,model=dict(name=native['name'],mode=native['mode'],time=native['time'],checks=checks),
                    execution=manifest['inputs']['execution'],native_validation=manifest['validation'],checks=checks,
                    reconciliations=reconcile(results),commands=[command,[str(self.store.engine),'verify-results',str(folder/'manifest.json'),'--results',str(folder/'results.csv')]],
                    verification='Saved artifact hashes checked on this request. Reconstruction of commands follows the recorded request and application worker contract.',
                    historical_accuracy='not_assessed',timestep=TIMESTEP)

    def playback(self,key,step):
        require(step in (1,3,12),'Observation interval must be 1, 3 or 12 months')
        results=self.store.results(key);job=self.store.get(key);horizon=results.description['time']['horizon']
        frames=[]
        for end in range(step,int(horizon)+1,step):
            frames.append(dict(start=end-step,end=end,scenarios=[dict(id=s['id'],values={m:results.value(m,s['id'],0,end-step,end)['value'] for m in METRICS}) for s in job['effective_parameters']]))
        return dict(run_id=key,model_version=job['model_version'],provenance=self.store.provenance(key),step=step,frames=frames,
                    timestep=TIMESTEP,aggregation='Full-resolution shared API summaries; period finances, ending FTE, weighted utilization. Replication 0.')

    def history(self,request):
        fields(request,('run_id','scenario','dataset'),('run_id','scenario','dataset'))
        result=evaluate_history(self.store.results(request['run_id']),request)
        receipt=dict(result,run_id=request['run_id'],scenario=request['scenario'],created_at=time.time(),
                     provenance=self.store.provenance(request['run_id']),dataset_sha256=digest(canonical(request['dataset'])),request=request)
        receipt['receipt_id']=digest(canonical(receipt));save(self.state/(receipt['receipt_id']+'.json'),receipt)
        return receipt

    def history_list(self,key):
        self.store.get(key)
        receipts=[]
        for path in self.state.glob('*.json'):
            record=json.loads(path.read_text())
            if record.get('run_id')!=key:continue
            identity=record.pop('receipt_id')
            require(digest(canonical(record))==identity and path.stem==identity,'Historical receipt has changed','RECEIPT_CORRUPT',409)
            record['receipt_id']=identity;receipts.append(record)
        # Result identities are checked again before showing any historical evidence.
        if receipts:self.store._verify_artifacts(self.store._job(key))
        return dict(evaluations=sorted(receipts,key=lambda r:r['created_at'],reverse=True))
