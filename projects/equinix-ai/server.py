"""Standalone local host for the Equinix AI opportunity project; no T&R registration."""
import argparse,mimetypes,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];sys.path.insert(0,str(ROOT))
from application.fathom_service.http import Handler,Server
from application.fathom_service.contracts import APIError,require
from service import Workshop
from spec import catalog
class ProjectHandler(Handler):
 def dispatch(self):
  try:
   self.guard();route=self.path.split('?',1)[0];parts=route.strip('/').split('/');w=self.server.workshop
   if self.command=='GET' and route=='/api/catalog':return self.respond(200,catalog())
   if self.command=='GET' and route=='/api/verification':return self.respond(200,w.verification())
   if self.command=='POST' and route=='/api/validate':
    from spec import validate
    return self.respond(200,dict(valid=True,definition=validate(self.body())))
   if self.command=='POST' and route=='/api/sessions':return self.respond(200,w.create(self.body()))
   if self.command=='POST' and route=='/api/experiments':return self.respond(200,w.experiment(self.body()))
   if len(parts)==4 and parts[:2]==['api','sessions']:
    session=w.get(parts[2]);action=parts[3]
    if self.command=='POST' and action=='step':return self.respond(200,session.step(self.body()))
    if self.command=='POST' and action=='stop':require(self.body()=={},'Empty object required');return self.respond(200,session.close())
    if self.command=='POST' and action=='evidence':return self.respond(200,w.evidence(parts[2],self.body()))
    if self.command=='GET' and action=='state':
     with session.lock:return self.respond(200,session.view())
    if self.command=='GET' and action=='record':
     with session.lock:return self.respond(200,session.record)
   if len(parts) in (3,4) and parts[:2]==['api','experiments']:
    if self.command=='GET' and len(parts)==3:return self.respond(200,w.experiment_state(parts[2]))
    if self.command=='POST' and len(parts)==4 and parts[3]=='cancel':require(self.body()=={},'Empty object required');return self.respond(200,w.experiment_state(parts[2],True))
   paths={'/':'index.html','/app.js':'app.js','/style.css':'style.css'}
   if self.command!='GET' or route not in paths:return self.respond(404,dict(error=dict(message='Not found')))
   file=HERE/paths[route];data=file.read_bytes();self.send_response(200);self.send_header('Content-Type',mimetypes.guess_type(file.name)[0] or 'application/octet-stream');self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.send_header('Content-Security-Policy',"default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");self.end_headers();self.wfile.write(data)
  except (BrokenPipeError,ConnectionResetError):pass
  except APIError as e:self.respond(e.status,dict(error=dict(message=str(e),code=e.code)))
  except Exception as e:self.respond(400,dict(error=dict(message=str(e))))
 do_GET=dispatch
 do_POST=dispatch
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=8088);p.add_argument('--state',type=Path,default=ROOT/'artifacts/equinix-ai-state');a=p.parse_args()
 w=Workshop(a.state,HERE/'native/build/equinix-model');server=Server(('127.0.0.1',a.port),None);server.RequestHandlerClass=ProjectHandler;server.workshop=w
 print(f'Equinix AI Opportunity Lab: http://127.0.0.1:{a.port}',flush=True)
 try:server.serve_forever()
 except KeyboardInterrupt:pass
 finally:w.close();server.server_close()
