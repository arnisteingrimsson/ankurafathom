"""Read lineage and exact numeric bits independently from native writer fixtures."""
import argparse
import csv
import json
from pathlib import Path
import struct
from provenance_hash_contract import numeric_hash

COLUMNS=['scenario','replication','time','output_id','value','manifest_id','scenario_parameters']

def bits(value):return struct.unpack('<Q',struct.pack('<d',float(value)))[0]

def check_rows(rows,manifest):
    scenarios={s['id']:s['effective_parameters'] for s in manifest['inputs']['scenarios']}
    for row in rows:
        assert row['manifest_id']==manifest['id']
        actual=json.loads(row['scenario_parameters'])
        expected=scenarios[int(row['scenario'])]
        assert actual.keys()==expected.keys()
        assert {k:bits(v) for k,v in actual.items()}=={k:bits(v) for k,v in expected.items()}

def check_table(table,manifest):
    import pyarrow as pa
    assert table.column_names==COLUMNS
    assert [f.type for f in table.schema]==[pa.uint32(),pa.uint32(),pa.float64(),pa.string(),pa.float64(),pa.string(),pa.string()]
    assert all(not field.nullable for field in table.schema)
    assert all(column.null_count==0 for column in table.columns)
    metadata=table.schema.metadata
    assert metadata[b'ankurafathom.schema_version']==b'0.2'
    assert metadata[b'ankurafathom.table']==b'observations'
    assert metadata[b'ankurafathom.manifest_id'].decode()==manifest['id']
    assert json.loads(metadata[b'ankurafathom.manifest'])==manifest
    check_rows(table.to_pylist(),manifest)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    parser.add_argument('--without-arrow',action='store_true');args=parser.parse_args()
    report=json.loads((args.directory/'expected.json').read_text());checked=0
    for case in report:
        kind=case['kind'];manifest=json.loads((args.directory/(kind+'.json')).read_text())
        expected=case['trajectories']
        assert numeric_hash(expected)==manifest['result']['sha256']
        for suffix in ['csv']+([] if args.without_arrow else ['parquet','arrow']):
            path=args.directory/(kind+'.'+suffix)
            if suffix=='csv':
                with path.open(newline='') as stream:
                    reader=csv.DictReader(stream);assert reader.fieldnames==COLUMNS;rows=list(reader)
                check_rows(rows,manifest)
            else:
                import pyarrow.ipc as ipc
                import pyarrow.parquet as pq
                table=pq.read_table(path) if suffix=='parquet' else ipc.open_file(path).read_all()
                check_table(table,manifest);rows=table.to_pylist()
                assert table.schema.metadata[b'ankurafathom.manifest']==(args.directory/(kind+'.json')).read_bytes()
            decoded={(t['scenario'],t['replication']):[] for t in expected}
            addresses=[]
            for row in rows:
                address=int(row['scenario']),int(row['replication']);addresses.append(address)
                decoded[address].append({'time_bits':bits(row['time']),'id':row['output_id'],'value_bits':bits(row['value'])})
            assert addresses==sorted(addresses)
            actual=[{'scenario':s,'replication':r,'rows':values} for (s,r),values in decoded.items()]
            assert actual==expected,(kind,suffix)
            assert numeric_hash(actual)==manifest['result']['sha256']
            assert len(rows)==manifest['result']['rows'];checked+=1
    print(f'{checked} decoded lineage artifacts: complete and empty trajectories, exact IEEE bits, parameters and embedded manifests pass')

if __name__=='__main__':main()
