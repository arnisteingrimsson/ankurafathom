"""A C-compiled consumer must match the CLI and independent numeric identity."""
import argparse
import csv
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from provenance_hash_contract import numeric_hash

ROOT=Path(__file__).resolve().parents[1]
def bits(value):return struct.unpack('<Q',struct.pack('<d',float(value)))[0]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('executable',type=Path);parser.add_argument('probe',type=Path)
    parser.add_argument('--without-arrow',action='store_true');args=parser.parse_args()
    exe=args.executable.resolve();probe=args.probe.resolve();checks=observations=0
    def call(command):
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        assert result.returncode==0,(command,result.stderr)
        return result.stdout
    def decode(text):
        header,*lines=text.splitlines();trajectories,count,digest=header.split()
        encoded=[];rows=[]
        for line in lines:
            scenario,replication,time,value,name=line.split()
            scenario=int(scenario);replication=int(replication);name=bytes.fromhex(name).decode()
            time=int(time,16);value=int(value,16);rows.append((scenario,replication,time,name,value))
            if not encoded or (scenario,replication)!=(encoded[-1]['scenario'],encoded[-1]['replication']):
                encoded.append({'scenario':scenario,'replication':replication,'rows':[]})
            encoded[-1]['rows'].append({'time_bits':time,'id':name,'value_bits':value})
        assert int(count)==len(rows) and int(trajectories)==len(encoded)
        assert numeric_hash(encoded)==digest
        return rows,digest
    fixtures=['decay.ir.json','stochastic_process.ir.json','typed_abm_rates.ir.json','agent_stock_sd.ir.json',
              'agent_pool.ir.json','hybrid_completion.ir.json']
    if not args.without_arrow:fixtures+=['data/parameter_decay.ir.json','data/seasonal_stock.ir.json','data/workforce.ir.json']
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp)
        for number,fixture in enumerate(fixtures):
            model=ROOT/'models'/fixture;assert model.exists(),model
            for ensemble in (False,True):
                experiment=root/'experiment.json'
                experiment.write_text(json.dumps({'seed':42,'replications':2,'scenarios':[
                    {'id':0,'parameters':{}},{'id':17,'parameters':{}}]}))
                output=root/'rows.csv';manifest=root/f'manifest-{number}-{ensemble}.json'
                call([exe,'run',model,'--out',output,'--manifest',manifest,'--seed','18446744073709551615',
                      *(['--experiment',experiment] if ensemble else [])])
                with output.open(newline='') as stream:
                    expected=[(int(r['scenario']),int(r['replication']),bits(r['time']),r['output_id'],bits(r['value'])) for r in csv.DictReader(stream)]
                digest=json.loads(manifest.read_text())['result']['sha256']
                for mode in ('file','json'):
                    for threads in ((1,8,32) if ensemble else (1,)):
                        rows,actual=decode(call([probe,model,experiment if ensemble else '-',threads,mode,model.parent,'18446744073709551615']))
                        assert rows==expected and actual==digest,(fixture,ensemble,mode,threads)
                        checks+=1;observations+=len(rows)
        # Generated scenarios share the CLI validator and ordering, without seed override.
        for kind in ('grid','lhs','sobol'):
            model=ROOT/'models/decay.ir.json';experiment=ROOT/f'models/decay.{kind}.experiment.json'
            output=root/'design.csv';manifest=root/(kind+'.json')
            call([exe,'run',model,'--experiment',experiment,'--out',output,'--manifest',manifest])
            expected=json.loads(manifest.read_text())['result']['sha256']
            for mode in ('file','json'):
                rows,digest=decode(call([probe,model,experiment,8,mode,model.parent,'-']))
                assert digest==expected;checks+=1;observations+=len(rows)
        # Explicit buffer lengths retain negative zero through JSON -> ABI -> C.
        source=json.loads((ROOT/'models/decay.ir.json').read_text())
        source['components']=[{'id':'z','kind':'stock','init':-0.0,'unit':'kg'}]
        source['outputs']=[{'id':'zero','stock':'z'}]
        model=root/'zero.json';model.write_text(json.dumps(source))
        rows,_=decode(call([probe,model,'-',1,'json',root,'-']))
        assert rows[0][4]==1<<63;checks+=1;observations+=len(rows)
    print(f'{checks} C-consumer comparisons / {observations} observations pass: exact CLI bits, independent hashlib, JSON/file loading, 1/8/32 workers and all scenario designs')
if __name__=='__main__':main()
