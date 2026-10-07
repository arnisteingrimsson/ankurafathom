"""Local T&R project host. Presentation and registration only; calculations use the shared API."""
import argparse
import copy
import json
import mimetypes
from pathlib import Path
import sys
import tempfile
from urllib.parse import urlsplit, parse_qs

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from application.examples.register_tr import create
from application.fathom_service.contracts import APIError, describe, loads, require
from application.fathom_service.core import Store
from application.fathom_service.http import Handler, Server
from validation import Validation
from live import LiveSessions
from workshop.service import Workshop
from workshop.spec import catalog as workshop_catalog


def package(model_path, destination):
    """Constrain this project's descriptor using actual bound model parameters."""
    create(model_path, destination)
    path = destination/'descriptor.json'
    descriptor = json.loads(path.read_text())
    model = loads(model_path.read_bytes())
    require(model.get('mode')=='sd' and model.get('integrator','euler')=='euler' and
            model.get('time')==dict(unit='month',dt=1,horizon=60),
            'This project requires the 60-month Euler transition model with dt=1 month; finer timesteps require a separately validated model revision.')
    expanded = copy.deepcopy(descriptor)
    for name in ('fixed_share', 'fixed_eligible_share', 'retainer_share', 'success_share'):
        expanded['parameters'].append(dict(id=name,label=name,description='Project fee constraint',
            kind='number',minimum=0,maximum=1,status='synthetic',source='Captured native parameter table'))
    effective = describe(model, expanded, model_path.parent)
    defaults = {p['id']:p['default'] for p in effective['parameters']}
    limit = min(defaults['fixed_eligible_share']-defaults['fixed_share'],
                1-defaults['fixed_share']-defaults['retainer_share']-defaults['success_share'])
    if limit < 0: raise ValueError('Infeasible initial contract mix')
    next(p for p in descriptor['parameters'] if p['id']=='fixed_shift')['maximum'] = limit
    # Domain-specific presentation metrics belong to this project descriptor.
    for name,label in [('earned_tm','Hourly fees'),('earned_fixed','Fixed fees'),
                       ('earned_retainer','Earned retainer services'),('earned_success','Expected success fees'),
                       ('disallowed','Approval disallowance')]:
        descriptor['metrics'].append(dict(id=name,label=label,definition=label+' during the selected period.',
            unit='USD',format='currency',dimensions={'practice':'TR'},aggregation='delta',
            interpolation='hold',output='cum_'+name))
    path.write_text(json.dumps(descriptor,indent=2)+'\n')
    config = json.loads((model_path.parent.parent/'config.json').read_text())
    return dict(initial_fte=sum(l['count'] for l in config['levels']),levels=config['levels'],
        fixed_share=defaults['fixed_share'],fixed_ceiling=defaults['fixed_eligible_share'],
        assumptions={k:v for k,v in config.items() if k!='levels'},source='Synthetic T&R monthly pilot',
        fee_shift_maximum=limit)


class ProjectHandler(Handler):
    def dispatch(self):
        route = self.path.split('?',1)[0]
        if route.startswith('/v1/') or route in ('/health','/openapi.json'):
            return super().dispatch()
        try:
            self.guard()
            if route=='/workshop/verification' and self.command=='GET':return self.respond(200,self.server.workshop.verification())
            if route=='/workshop/catalog' and self.command=='GET':return self.respond(200,workshop_catalog())
            if route=='/workshop/sessions' and self.command=='POST':return self.respond(200,self.server.workshop.create(self.body()))
            if route=='/workshop/experiments' and self.command=='POST':return self.respond(200,self.server.workshop.experiment(self.body()))
            if route.startswith('/workshop/sessions/'):
                parts=route.strip('/').split('/');require(len(parts)==4,'Invalid session route');session=self.server.workshop.get(parts[2]);action=parts[3]
                if self.command=='POST' and action=='step':return self.respond(200,session.step(self.body()))
                if self.command=='POST' and action=='stop':
                    require(self.body()=={},'Empty object required');return self.respond(200,session.close())
                if self.command=='POST' and action=='evidence':return self.respond(200,self.server.workshop.evidence(parts[2],self.body()))
                if self.command=='GET' and action=='record':
                    with session.lock:return self.respond(200,session.record)
                if self.command=='GET' and action=='state':
                    with session.lock:return self.respond(200,session.view())
                return self.respond(404,dict(error=dict(message='Unknown session action')))
            if route.startswith('/workshop/experiments/'):
                parts=route.strip('/').split('/');require(len(parts) in (3,4),'Invalid experiment route')
                if self.command=='POST' and len(parts)==4 and parts[3]=='cancel':
                    require(self.body()=={},'Empty object required');return self.respond(200,self.server.workshop.experiment_state(parts[2],True))
                require(self.command=='GET' and len(parts)==3,'GET required');return self.respond(200,self.server.workshop.experiment_state(parts[2]))
            if route=='/project/live' and self.command=='POST':
                return self.respond(200,self.server.live.create(self.body()))
            if route.startswith('/project/live/'):
                parts=route.strip('/').split('/');require(len(parts)==4,'Invalid live route')
                session=self.server.live.get(parts[2]);action=parts[3]
                if self.command=='POST' and action=='step':
                    body=self.body();require(set(body)=={'expected_revision'},'Step requires expected_revision only')
                    return self.respond(200,session.step(body['expected_revision']))
                if self.command=='POST' and action=='stop':
                    require(self.body()=={},'Stop requires an empty object');return self.respond(200,session.close())
                if self.command=='GET' and action=='record':
                    with session.lock:return self.respond(200,session.record)
                if self.command=='GET' and action=='state':
                    with session.lock:return self.respond(200,session.view())
                return self.respond(404,dict(error=dict(message='Unknown live action')))
            if route.startswith('/project/'):
                query=parse_qs(urlsplit(self.path).query)
                if self.command=='POST' and route=='/project/history':
                    return self.respond(200,self.server.validation.history(self.body()))
                require(self.command=='GET','GET required')
                if route=='/project/validation':return self.respond(200,self.server.validation.catalog())
                key=query.get('run_id',[''])[0]
                if route=='/project/history':return self.respond(200,self.server.validation.history_list(key))
                if route=='/project/evidence':return self.respond(200,self.server.validation.evidence(key))
                if route=='/project/playback':return self.respond(200,self.server.validation.playback(key,int(query.get('step',['1'])[0])))
                return self.respond(404,dict(error=dict(message='Not found')))
            if self.command!='GET':return self.respond(405,dict(error=dict(message='GET required')))
            if route=='/project.json':return self.respond(200,self.server.project)
            paths={'/':HERE/'workshop/index.html','/index.html':HERE/'workshop/index.html','/monthly':HERE/'live.html','/analysis':HERE/'index.html',
                   '/workshop/app.js':HERE/'workshop/app.js','/workshop/style.css':HERE/'workshop/style.css',
                   '/live.js':HERE/'live.js','/live.css':HERE/'live.css',
                   '/app.js':HERE/'app.js','/style.css':HERE/'style.css','/ui-model.mjs':HERE/'ui-model.mjs',
                   '/preview.json':HERE/'preview.json','/validation.js':HERE/'validation.js',
                   '/client.mjs':ROOT/'application/client/fathom.mjs'}
            if route not in paths:return self.respond(404,dict(error=dict(message='Not found')))
            file=paths[route];data=file.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type',mimetypes.guess_type(file.name)[0] or 'application/octet-stream')
            self.send_header('Content-Length',str(len(data)))
            self.send_header('Cache-Control','no-store')
            self.send_header('X-Content-Type-Options','nosniff')
            self.send_header('Content-Security-Policy',"default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
            self.end_headers();self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass
        except APIError as error:self.respond(error.status,dict(error=dict(code=error.code,message=str(error))))
        except Exception as error:self.respond(400,dict(error=dict(message=str(error))))
    do_GET=dispatch
    do_POST=dispatch
    do_OPTIONS=dispatch


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=8087)
    parser.add_argument('--state',type=Path,default=ROOT/'artifacts/tr-ui-state')
    parser.add_argument('--engine',type=Path,default=ROOT/'build-arrow/fathom')
    parser.add_argument('--model',type=Path,default=ROOT/'artifacts/tr-pilot-150-validated-20260930/batch-00/inputs/model.json')
    args=parser.parse_args();args.state.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='registration-',dir=args.state) as tmp:
        directory=Path(tmp)/'package'
        project=package(args.model.resolve(),directory)
        store=Store(directory/'registry.json',args.state/'service',args.engine,workers=2)
        try:
            server=Server(('127.0.0.1',args.port),store)
            server.RequestHandlerClass=ProjectHandler;server.project=project
            server.validation=Validation(store,args.model.resolve().parent.parent,args.state/'historical-evaluations')
            server.live=LiveSessions(store,args.state/'live-sessions',HERE/'native/build/tr-live')
            server.workshop=Workshop(args.state/'operating-sessions',HERE/'native/build/tr-workshop')
            print(f'T&R Decision Lab: http://127.0.0.1:{server.server_port}',flush=True)
            try:server.serve_forever()
            except KeyboardInterrupt:pass
            finally:server.workshop.close();server.live.close();server.server_close()
        finally:store.close()


if __name__=='__main__':main()
