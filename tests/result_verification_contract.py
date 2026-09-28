"""Independent artifact re-encoding and corruption checks for verify-results."""
import argparse
import copy
import csv
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from output_provenance_contract import COLUMNS, bits
from provenance_cli_contract import canonical, reseal
from provenance_hash_contract import numeric_hash

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('executable',type=Path)
    parser.add_argument('fixtures',type=Path);parser.add_argument('--without-arrow',action='store_true')
    args=parser.parse_args();exe=args.executable.resolve();fixtures=args.fixtures.resolve();calls=embedded_calls=0
    formats=['csv']+([] if args.without_arrow else ['parquet','arrow'])
    if not args.without_arrow:
        import pyarrow as pa
        import pyarrow.ipc as ipc
        import pyarrow.parquet as pq
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp)
        def call(*argv,error=None):
            nonlocal calls
            calls+=1
            result=subprocess.run([str(exe),*map(str,argv)],capture_output=True,text=True)
            if error:
                assert result.returncode==1,(argv,result.stdout,result.stderr)
                assert not result.stdout
                assert json.loads(result.stderr)['diagnostics'][0]['code']==error,(argv,result.stderr)
                return None
            assert result.returncode==0,(argv,result.stderr)
            return result.stdout
        def verify_embedded(path,expected=None,error=None,*extra):
            nonlocal embedded_calls
            embedded_calls+=1
            before=path.read_bytes() if path.is_file() else None
            verdict=call('verify-results',path,'--embedded',*extra,error=error)
            if before is not None:assert path.read_bytes()==before,'embedded verification mutated result'
            if not error:
                value=json.loads(verdict)
                assert value['verdict']=='pass' and value['scope']=='artifact-integrity'
                assert value['manifest_source']=='embedded'
                assert value['manifest_id']==expected['id'] and value['result']==expected['result']
                assert Path(value['path']).resolve()==path.resolve()
        def verify(manifest,path=None,error=None,*extra):
            before={p:p.read_bytes() for p in (manifest,path) if p and p.is_file()}
            verdict=call('verify-results',manifest,*(['--results',path] if path else []),*extra,error=error)
            assert all(p.read_bytes()==data for p,data in before.items()),'verification mutated inputs'
            if not error:
                value=json.loads(verdict);expected=json.loads(manifest.read_text())
                assert value['verdict']=='pass' and value['scope']=='artifact-integrity'
                assert value['manifest_source']=='sidecar'
                assert value['manifest_id']==expected['id'] and value['result']==expected['result']
            if path and path.suffix in ('.parquet','.arrow') and not args.without_arrow and error in (None,'IR_RESULT_VERIFY'):
                verify_embedded(path,json.loads(manifest.read_text()),error)
        def write_csv(path,rows,header=COLUMNS):
            with path.open('w',newline='') as stream:
                writer=csv.DictWriter(stream,fieldnames=header,lineterminator='\r\n')
                writer.writeheader();writer.writerows(rows)
        def write_table(path,table,suffix):
            if suffix=='parquet':pq.write_table(table,path,row_group_size=10000,compression='NONE',use_dictionary=False)
            else:
                with ipc.new_file(path,table.schema) as writer:writer.write_table(table,max_chunksize=7000)
        # Native fixtures include empty trajectories and adversarial IEEE values.
        for kind in ('mixed','empty','single'):
            manifest=fixtures/(kind+'.json')
            for suffix in formats:verify(manifest,fixtures/(kind+'.'+suffix))
        manifest=fixtures/'mixed.json';saved=json.loads(manifest.read_text())
        with (fixtures/'mixed.csv').open(newline='') as stream:original=list(csv.DictReader(stream))
        csv_path=root/'independent.csv';write_csv(csv_path,original);verify(manifest,csv_path)
        csv_path.write_bytes(csv_path.read_bytes().removesuffix(b'\r\n'));verify(manifest,csv_path)
        # The same rows survive independent encoding, not just writer/reader symmetry.
        table=None
        if not args.without_arrow:
            table=ipc.open_file(fixtures/'mixed.arrow').read_all()
            for suffix in formats[1:]:
                path=root/('independent.'+suffix);write_table(path,table,suffix);verify(manifest,path)
        mutations=[]
        for field,value in [('scenario','18'),('replication','2'),('time','-1'),('value','nan'),('value','inf'),
                            ('value','-0'),('output_id','changed'),('output_id',''),('manifest_id','0'*64),
                            ('scenario_parameters','{}'),('scenario_parameters',original[0]['scenario_parameters']+' ')]:
            rows=copy.deepcopy(original);rows[0][field]=value;mutations.append(rows)
        for mode in ('missing','duplicate','observation_order','trajectory_order'):
            rows=copy.deepcopy(original)
            if mode=='missing':rows.pop()
            elif mode=='duplicate':rows[1]=rows[0]
            elif mode=='observation_order':rows[0],rows[1]=rows[1],rows[0]
            else:rows=rows[8:]+rows[:8]
            mutations.append(rows)
        for number,rows in enumerate(mutations):
            path=root/'corrupt.csv';write_csv(path,rows);verify(manifest,path,'IR_RESULT_VERIFY')
            if table is not None:
                typed=[{**r,'scenario':int(r['scenario']),'replication':int(r['replication']),
                        'time':float(r['time']),'value':float(r['value'])} for r in rows]
                changed=pa.Table.from_pylist(typed,schema=table.schema)
                for suffix in formats[1:]:
                    path=root/('corrupt.'+suffix);write_table(path,changed,suffix);verify(manifest,path,'IR_RESULT_VERIFY')
        # CSV syntax, exact header, numeric grammar and field count controls.
        text=(fixtures/'mixed.csv').read_text()
        for bad in ['',text.replace('scenario,replication','replication,scenario',1),text+'\n',
                    text+'"unterminated',text+'"x"junk\n',text+'x"y\n',text+'1,2\n',text+'\r']:
            csv_path.write_text(bad);verify(manifest,csv_path,'IR_RESULT_VERIFY')
        for field,value in [('scenario','-1'),('scenario','4294967296'),('replication','0.0'),
                            ('time',' 0'),('value','1junk'),('value','1e9999'),('value','')]:
            rows=copy.deepcopy(original);rows[0][field]=value;write_csv(csv_path,rows)
            verify(manifest,csv_path,'IR_RESULT_VERIFY')
        if table is not None:
            changes=[table.drop(['value']),table.select(list(reversed(COLUMNS))),table.replace_schema_metadata(None)]
            metadata=pa.KeyValueMetadata([*table.schema.metadata.items(),(b'ankurafathom.manifest',b'conflicting')])
            changes.append(table.replace_schema_metadata(metadata))
            for key in table.schema.metadata:
                metadata=dict(table.schema.metadata);metadata[key]=b'wrong';changes.append(table.replace_schema_metadata(metadata))
            changes.append(table.set_column(0,pa.field('scenario',pa.uint64(),False),table.column(0).cast(pa.uint64())))
            changes.append(table.set_column(4,pa.field('value',pa.float64(),True),table.column(4)))
            nulls=table.column(4).to_pylist();nulls[0]=None
            changes.append(table.set_column(4,pa.field('value',pa.float64(),True),pa.array(nulls,pa.float64())))
            for changed in changes:
                for suffix in formats[1:]:
                    path=root/('bad-schema.'+suffix);write_table(path,changed,suffix);verify(manifest,path,'IR_RESULT_VERIFY')
            for suffix in formats[1:]:
                path=root/('truncated.'+suffix);path.write_bytes((fixtures/('mixed.'+suffix)).read_bytes()[:100])
                verify(manifest,path,'IR_RESULT_VERIFY')
        # Independently sealed catalog and numeric digest, crossing row groups/batches.
        large=copy.deepcopy(saved);large['inputs']['scenarios']=large['inputs']['scenarios'][:1]
        large['inputs']['replications']=1
        count=65539
        expected=[{'scenario':0,'replication':0,'rows':[{'id':'value','time_bits':bits(i),'value_bits':bits(i*.5)} for i in range(count)]}]
        large['result']={'encoding':'ordered-ieee754-le-v1','sha256':numeric_hash(expected),'rows':count,'trajectories':1}
        reseal(large);large_manifest=root/'large.json';large_manifest.write_bytes(canonical(large))
        parameters=canonical(large['inputs']['scenarios'][0]['effective_parameters']).decode()
        rows=[dict(zip(COLUMNS,[0,0,i,'value',i*.5,large['id'],parameters])) for i in range(count)]
        write_csv(csv_path,rows);verify(large_manifest,csv_path)
        if table is not None:
            metadata={**table.schema.metadata,b'ankurafathom.manifest_id':large['id'].encode(),b'ankurafathom.manifest':canonical(large)}
            independent=pa.Table.from_pylist(rows,schema=table.schema.with_metadata(metadata))
            for suffix in formats[1:]:
                path=root/('large.'+suffix);write_table(path,independent,suffix);verify(large_manifest,path)
        # Verification needs neither source inputs nor the original output location.
        model=root/'model.json';shutil.copyfile(ROOT/'models/decay.ir.json',model)
        receipt=root/'run.json';output=root/'run.csv'
        call('run',model,'--out',output,'--manifest',receipt);verify(receipt)
        if table is not None:
            binary_receipt=root/'binary.json';binary=root/'binary.dat'
            call('run',model,'--out',binary,'--format','arrow','--manifest',binary_receipt)
            verify(binary_receipt)
            verify(binary_receipt,binary,'IR_RESULT_VERIFY')
            verify(binary_receipt,binary,None,'--format','arrow')
        model.unlink();moved=root/'moved.json';output.rename(root/'moved.csv');receipt.rename(moved)
        verify(moved,root/'moved.csv');verify(moved,error='IR_RESULT_VERIFY')
        if table is not None:
            # Only the binary artifact remains: source and sidecar have gone.
            expected_binary=json.loads(binary_receipt.read_text());binary_receipt.unlink()
            renamed=root/'relocated.ipc';binary.rename(renamed)
            verify_embedded(renamed,expected_binary)
            unknown=root/'relocated.data';renamed.rename(unknown)
            verify_embedded(unknown,error='IR_USAGE')
            verify_embedded(unknown,expected_binary,None,'--format','arrow')
            verify_embedded(unknown,None,'IR_RESULT_VERIFY','--format','parquet')
            verify_embedded(root/'missing.arrow',error='IR_RESULT_VERIFY')
            verify_embedded(root,None,'IR_RESULT_VERIFY','--format','arrow')
            # Empty artifacts still require a valid full manifest envelope.
            empty=ipc.open_file(fixtures/'empty.arrow').read_all()
            empty_manifest=json.loads((fixtures/'empty.json').read_text())
            for mode in ('validation','timestamp','path','extra','missing'):
                altered=copy.deepcopy(empty_manifest)
                if mode=='validation':altered['validation']['accuracy']='pass'
                elif mode=='timestamp':altered['created_utc']='tomorrow'
                elif mode=='path':altered['inputs']['model']['path']='relative.json'
                elif mode=='extra':altered['unexpected']=1
                else:del altered['inputs']['model']
                reseal(altered)
                metadata={**empty.schema.metadata,b'ankurafathom.manifest':canonical(altered),
                          b'ankurafathom.manifest_id':altered['id'].encode()}
                for suffix in formats[1:]:
                    path=root/('invalid-envelope.'+suffix);write_table(path,empty.replace_schema_metadata(metadata),suffix)
                    verify_embedded(path,error='IR_MANIFEST')
            # Self-consistent replacement metadata is not an authenticity proof.
            resealed=copy.deepcopy(empty_manifest);resealed['created_utc']='2000-01-01T00:00:00Z';reseal(resealed)
            metadata={**empty.schema.metadata,b'ankurafathom.manifest':canonical(resealed),
                      b'ankurafathom.manifest_id':resealed['id'].encode()}
            for suffix in formats[1:]:
                path=root/('resealed.'+suffix);write_table(path,empty.replace_schema_metadata(metadata),suffix)
                verify_embedded(path,resealed)
                call('verify-results',fixtures/'empty.json','--results',path,error='IR_RESULT_VERIFY')
            for payload in (b'',b'{',b'null',b'[]',canonical(empty_manifest)+b' ',b'{}'):
                metadata={**empty.schema.metadata,b'ankurafathom.manifest':payload}
                for suffix in formats[1:]:
                    path=root/('invalid-manifest.'+suffix);write_table(path,empty.replace_schema_metadata(metadata),suffix)
                    verify_embedded(path,error='IR_RESULT_VERIFY')
        for options in [('--out',root/'sentinel'),('--seed','1'),('--results',csv_path,'--results',csv_path),
                        ('--format','bad'),('--format','csv','--format','csv'),('--results',),('--results','')]:
            call('verify-results',moved,*options,error='IR_USAGE')
        call('verify-results',error='IR_USAGE')
        for options in [('--embedded','--embedded'),('--embedded','--results',csv_path),
                        ('--results',csv_path,'--embedded'),('--embedded','--format','csv')]:
            call('verify-results',root/'ignored.parquet',*options,error='IR_USAGE')
        verify_embedded(root/'moved.csv',error='IR_USAGE')
        verify(moved,root/'moved.csv',None,'--format','csv')
        verify(moved,root/'missing.csv','IR_RESULT_VERIFY')
        verify(moved,root,'IR_RESULT_VERIFY')
        legacy=copy.deepcopy(saved);legacy['manifest_version']='0.1';del legacy['output']['schema_version']
        legacy_path=root/'legacy.json';legacy_path.write_bytes(canonical(reseal(legacy)))
        verify(legacy_path,fixtures/'mixed.csv','IR_MANIFEST')
        wrong=copy.deepcopy(saved);wrong['id']='0'*64;bad_manifest=root/'bad.json';bad_manifest.write_bytes(canonical(wrong))
        verify(bad_manifest,fixtures/'mixed.csv','IR_MANIFEST')
        # Recomputed checksums still cannot justify inconsistent catalogs or bits.
        for mode in ('parameters','rows','catalog','bound'):
            changed=copy.deepcopy(saved)
            if mode=='parameters':changed['inputs']['scenarios'][0]['effective_parameters']['decay_rate']=7
            elif mode=='rows':changed['result']['rows']+=1
            elif mode=='catalog':changed['inputs']['scenarios'][0]['id']=1
            else:
                changed['inputs']['replications']=65536;changed['inputs']['scenarios']=[
                    {**changed['inputs']['scenarios'][0],'id':i} for i in range(16)]
                changed['result']['trajectories']=16*65536
            bad_manifest.write_bytes(canonical(reseal(changed)))
            verify(bad_manifest,fixtures/'mixed.csv','IR_RESULT_VERIFY')
        if args.without_arrow:
            verify(manifest,root/'unavailable.parquet','IR_RESULT_UNAVAILABLE')
            verify_embedded(root/'unavailable.parquet',error='IR_RESULT_UNAVAILABLE')
        assert not list(root.rglob('.fathom-*'))
        print(f'{calls} artifact verification CLI checks ({embedded_calls} embedded) pass: independent encoding, 65539-row boundaries, corruption and read-only behavior')

if __name__=='__main__':main()
