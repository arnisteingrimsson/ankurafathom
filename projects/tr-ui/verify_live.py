"""Independently check an exported live session's record and native frame chain."""
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from application.fathom_service.contracts import canonical,digest

def verify(record):
    assert record['record_sha256']==digest(canonical({k:v for k,v in record.items() if k!='record_sha256'})),'Session record changed'
    previous=None
    for month,frame in enumerate(record['frames']):
        assert frame['month']==month,'Missing or out-of-order month'
        assert frame['previous_sha256']==previous,'Broken chain'
        assert frame['sha256']==digest(canonical({k:v for k,v in frame.items() if k!='sha256'})),'Frame changed'
        assert frame['checks_passed']==all(c['passed'] for c in frame['checks']),'Check verdict mismatch'
        previous=frame['sha256']
    assert record['computed_through']==len(record['frames'])-1,'Computed clock mismatch'
    assert record['status']!='completed' or record['computed_through']==record['horizon_months'],'Premature completion'
    return dict(months=record['computed_through'],frames=len(record['frames']),status=record['status'],record_sha256=record['record_sha256'])

if __name__=='__main__':print(json.dumps(verify(json.loads(Path(sys.argv[1]).read_text())),indent=2))
