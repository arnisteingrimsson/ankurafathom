import hashlib
import json
from pathlib import Path
import struct
import sys


def numeric_hash(trajectories):
    word=lambda x:struct.pack('<Q',x)
    data=bytearray(b'AnkuraFathom.observations.v1\0')
    data+=word(len(trajectories))
    for trajectory in trajectories:
        data+=word(trajectory['scenario'])+word(trajectory['replication'])+word(len(trajectory['rows']))
        for row in trajectory['rows']:
            name=row['id'].encode('utf-8')
            data+=word(row['time_bits'])+word(len(name))+name+word(row['value_bits'])
    return hashlib.sha256(data).hexdigest()


if __name__=='__main__':
    report=json.loads(Path(sys.argv[1]).read_text())
    for case in report['cases']:
        assert numeric_hash(case['trajectories'])==case['identity']['sha256']
        assert case['identity']['rows']==24
    assert numeric_hash([])==report['empty']['sha256']
    print('32 numeric identities / 768 IEEE-754 values plus empty result match hashlib')
