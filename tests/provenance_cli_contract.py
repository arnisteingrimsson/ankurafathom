"""Manifest identities and strict replay, independently checked from result files."""
import argparse
import copy
import csv
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from provenance_hash_contract import numeric_hash
from output_provenance_contract import COLUMNS, check_rows, check_table

ROOT=Path(__file__).resolve().parents[1]

def sha(data):return hashlib.sha256(data).hexdigest()
def canonical(obj):return json.dumps(obj,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()
def reseal(obj):
    obj.pop('id',None);obj['id']=sha(canonical(obj));return obj

def records_identity(rows):
    trajectories=[]
    for row in rows:
        address=int(row.get('scenario',0)),int(row.get('replication',0))
        if not trajectories or address!=(trajectories[-1]['scenario'],trajectories[-1]['replication']):
            trajectories.append({'scenario':address[0],'replication':address[1],'rows':[]})
        trajectories[-1]['rows'].append({'id':row['output_id'],
            'time_bits':struct.unpack('<Q',struct.pack('<d',float(row['time'])))[0],
            'value_bits':struct.unpack('<Q',struct.pack('<d',float(row['value'])))[0]})
    return numeric_hash(trajectories)


def csv_identity(text):return records_identity(csv.DictReader(io.StringIO(text)))

def main():
    p=argparse.ArgumentParser();p.add_argument('executable',type=Path)
    p.add_argument('--without-arrow',action='store_true');p.add_argument('--schema',action='store_true')
    args=p.parse_args();exe=args.executable.resolve();calls=runs=0
    validator=None
    if args.schema:
        from jsonschema import Draft202012Validator
        validator=Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-manifest.schema.json').read_text()))
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp);shutil.copytree(ROOT/'models/data',root/'data')
        def call(*argv,code=None):
            nonlocal calls
            calls+=1
            r=subprocess.run([str(exe),*map(str,argv)],cwd=root.parent,capture_output=True,text=True)
            if code:
                assert r.returncode==1,(code,r.stdout,r.stderr)
                error=json.loads(r.stderr)['diagnostics'][0];assert error['code']==code,error
            else:assert r.returncode==0,r.stderr
            return r.stdout
        fixtures=['decay.ir.json','stochastic_process.ir.json','typed_abm_rates.ir.json','agent_stock_sd.ir.json']
        if not args.without_arrow:fixtures+=['data/parameter_decay.ir.json','data/seasonal_stock.ir.json','data/workforce.ir.json']
        for number,fixture in enumerate(fixtures):
            path=root/fixture
            if not path.exists():shutil.copyfile(ROOT/'models'/fixture,path)
            for ensemble in (False,True):
                tag=f'{number}-{ensemble}';output=root/(tag+'.csv');manifest=root/(tag+'.json')
                extra=[]
                if ensemble:
                    experiment=root/(tag+'-experiment.json')
                    experiment.write_text(json.dumps({'seed':3,'replications':2,'scenarios':[
                        {'id':0,'parameters':{}},{'id':17,'parameters':{}}]}))
                    extra=['--experiment',experiment]
                call('run',path,'--out',output,'--manifest',manifest,'--seed',42,'--threads',8,*extra);runs+=1
                saved=json.loads(manifest.read_text())
                assert saved['manifest_version']=='0.2' and saved['output']['schema_version']=='0.2'
                reader=csv.DictReader(io.StringIO(output.read_text()));assert reader.fieldnames==COLUMNS
                check_rows(list(reader),saved)
                if validator:validator.validate(saved)
                check=copy.deepcopy(saved);ident=check.pop('id');assert sha(canonical(check))==ident
                assert saved['result']['sha256']==csv_identity(output.read_text())
                assert saved['inputs']['execution']['seed']==42
                for key in ('model','experiment'):
                    item=saved['inputs'][key]
                    if item:
                        assert item['file_sha256']==sha(Path(item['path']).read_bytes())
                        assert item['canonical_sha256']==sha(item['canonical_json'].encode())
                        assert json.loads(item['canonical_json'])==json.loads(Path(item['path']).read_text())
                for binding in saved['inputs']['data']:
                    assert binding['file_sha256']==sha(Path(binding['path']).read_bytes())
                for threads in (1,8,32):
                    verified=json.loads(call('replay',manifest,'--threads',threads))
                    assert verified['verdict']=='pass' and verified['manifest_id']==ident
                    assert verified['result']==saved['result']
                regenerated=root/(tag+'-replay.csv')
                call('replay',manifest,'--out',regenerated,'--threads',1)
                assert regenerated.read_bytes()==output.read_bytes()
                if not args.without_arrow:
                    import pyarrow.ipc as ipc
                    import pyarrow.parquet as pq
                    for suffix in ('parquet','arrow'):
                        result=root/(tag+'.'+suffix)
                        call('replay',manifest,'--out',result)
                        table=pq.read_table(result) if suffix=='parquet' else ipc.open_file(result).read_all()
                        check_table(table,saved)
                        assert table.num_rows==saved['result']['rows']
                        assert records_identity(table.to_pylist())==saved['result']['sha256']
        model=root/'decay.ir.json';experiment=root/'grid.json'
        experiment.write_text(json.dumps({'seed':10,'replications':2,'design':{'kind':'grid','axes':{'decay_rate':[.25,.5]}}}))
        manifest=root/'grid-manifest.json';out=root/'grid.csv'
        call('run',model,'--experiment',experiment,'--out',out,'--manifest',manifest);runs+=1
        saved=json.loads(manifest.read_text());assert [x['overrides']['decay_rate'] for x in saved['inputs']['scenarios']]==[.25,.5]
        assert saved['inputs']['scenarios'][1]['effective_parameters']['decay_rate']==.5
        if not args.without_arrow:
            for suffix in ('parquet','arrow'):
                sidecar=root/('grid-'+suffix+'.json')
                call('run',model,'--experiment',experiment,'--out',root/('grid.'+suffix),'--manifest',sidecar);runs+=1
                original=json.loads(sidecar.read_text());assert original['result']==saved['result']
                table=pq.read_table(root/('grid.'+suffix)) if suffix=='parquet' else ipc.open_file(root/('grid.'+suffix)).read_all()
                check_table(table,original)
                call('replay',sidecar)
        # v0.1 remains readable and preserves its original result schema. This
        # same-build fixture does not relax the strict build identity policy.
        legacy=copy.deepcopy(saved);legacy['manifest_version']='0.1';del legacy['output']['schema_version']
        legacy_path=root/'legacy.json';legacy_path.write_text(json.dumps(reseal(legacy)))
        if validator:validator.validate(legacy)
        legacy_output=root/'legacy.csv';call('replay',legacy_path,'--out',legacy_output)
        assert csv.DictReader(io.StringIO(legacy_output.read_text())).fieldnames==COLUMNS[:5]
        assert csv_identity(legacy_output.read_text())==saved['result']['sha256']
        sentinel=root/'protected.csv';sentinel.write_text('KEEP')
        # Tampering is detected before publication. Checksums are integrity checks, not signatures.
        bad=root/'bad.json'
        bad.write_text(manifest.read_text().replace('ordered-ieee754-le-v1','changed'))
        call('replay',bad,'--out',sentinel,code='IR_MANIFEST');assert sentinel.read_text()=='KEEP'
        for section in ('result','build','validation'):
            altered=copy.deepcopy(saved)
            if section=='result':altered['result']['sha256']='0'*64
            elif section=='build':altered['inputs']['execution']['build']['compiler']='different'
            else:altered['validation']['accuracy']='pass'
            bad.write_text(json.dumps(reseal(altered)))
            call('replay',bad,'--out',sentinel,code='IR_MANIFEST' if section=='validation' else 'IR_REPLAY_MISMATCH')
            assert sentinel.read_text()=='KEEP'
        for section in ('output','schema','missing_schema','timestamp','seed','thread_count','input_path'):
            altered=copy.deepcopy(saved)
            if section=='output':altered['output']['format']='unsupported'
            elif section=='schema':altered['output']['schema_version']='0.1'
            elif section=='missing_schema':del altered['output']['schema_version']
            elif section=='timestamp':altered['created_utc']='tomorrow'
            elif section=='seed':altered['inputs']['execution']['seed']=-1
            elif section=='thread_count':altered['inputs']['execution']['threads']=257
            else:altered['inputs']['model']['path']='relative.json'
            bad.write_text(json.dumps(reseal(altered)))
            call('replay',bad,code='IR_MANIFEST')
        for source in (model,experiment):
            original=source.read_bytes();source.write_bytes(original+b'\n')
            call('replay',manifest,'--out',sentinel,code='IR_REPLAY_MISMATCH');assert sentinel.read_text()=='KEEP'
            source.write_bytes(original)
        if not args.without_arrow:
            bound=root/'data/workforce.ir.json';receipt=root/'bound.json'
            call('run',bound,'--out',root/'bound.csv','--manifest',receipt);runs+=1
            source=root/'data/synthetic_workforce.csv';original=source.read_bytes();lines=original.decode().splitlines()
            source.write_text('\n'.join([lines[0],*reversed(lines[1:])])+'\n')
            call('replay',receipt,'--out',sentinel,code='IR_REPLAY_MISMATCH');source.write_bytes(original)
            for input_path in (source,bound,receipt):
                call('replay',receipt,'--out',input_path,code='IR_USAGE')
        # Manifest destinations, including aliases and failures, cannot destroy inputs.
        for option,value in [('--seed','1'),('--experiment',experiment),('--manifest',root/'new.json')]:
            call('replay',manifest,option,value,code='IR_USAGE')
        call('run',model,'--manifest',root/'missing-out.json',code='IR_USAGE')
        call('run',model,'--out',sentinel,'--manifest',manifest,code='IR_USAGE');assert sentinel.read_text()=='KEEP'
        call('run',model,'--out',sentinel,'--manifest',root/'missing/manifest.json',code='IR_IO');assert sentinel.read_text()=='KEEP'
        call('run',model,'--out',root/'no-parent/out.csv','--manifest',root/'uncommitted.json',code='IR_IO')
        assert not (root/'uncommitted.json').exists()
        alias=root/'model-alias.json';alias.symlink_to(model)
        for target in (model,alias):call('run',model,'--out',sentinel,'--manifest',target,code='IR_USAGE')
        (root/'dir').mkdir()
        call('run',model,'--out',root/'new.csv','--manifest',root/'dir/../new.csv',code='IR_USAGE')
        assert not (root/'new.csv').exists()
        assert not list(root.rglob('.fathom-*.tmp-*'))
        # A signed-zero stock proves numeric identity does not use JSON float equality.
        zero=json.loads(model.read_text());zero['components']=[{'id':'z','kind':'stock','init':-0.0,'unit':'kg'}]
        zero['outputs']=[{'id':'z_ts','stock':'z'}];zero_path=root/'zero.json';zero_path.write_text(json.dumps(zero))
        zero_manifest=root/'zero-manifest.json';zero_output=root/'zero.csv'
        call('run',zero_path,'--out',zero_output,'--manifest',zero_manifest);runs+=1
        assert json.loads(zero_manifest.read_text())['result']['sha256']==csv_identity(zero_output.read_text())
        call('replay',zero_manifest)
        print(f'{calls} CLI checks, {runs} manifests; independent result hashes, replay, drift and publication controls passed')

if __name__=='__main__':main()
