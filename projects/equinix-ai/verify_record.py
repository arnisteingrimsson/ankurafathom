"""Check exported record integrity and optionally recompute every saved frame."""
import argparse
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from application.fathom_service.contracts import canonical, digest, require
from service import Session


def verify(record, replay=False, runner=None):
    require(record['record_sha256'] == digest(canonical({k: v for k, v in record.items() if k != 'record_sha256'})), 'Record hash mismatch')
    require(record['config_sha256'] == digest(canonical(record['config'])), 'Definition hash mismatch')
    previous = None
    for frame in record['frames']:
        require(frame['previous_sha256'] == previous, 'Frame chain mismatch')
        require(frame['sha256'] == digest(canonical({k: v for k, v in frame.items() if k != 'sha256'})), 'Frame hash mismatch')
        require(frame['checks_passed'], 'Recorded model checks failed')
        previous = frame['sha256']
    require(bool(record['frames']), 'No recorded frames')
    if replay:
        runner = runner or HERE / 'native/build/equinix-model'
        require(digest(runner.read_bytes()) == record['runner_sha256'], 'Replay requires the recorded runner binary')
        with tempfile.TemporaryDirectory() as directory:
            session = Session(Path(directory), record['config'], runner.resolve())
            try:
                require(session.frames[0] == record['frames'][0], 'Initial frame differs')
                for frame in record['frames'][1:]:
                    delta = frame['time'] - session.frames[-1]['time']
                    session.step(dict(expected_revision=session.revision, days=max(.25, delta)))
                    require(session.frames[-1] == frame, 'Recomputed frame differs at day ' + str(frame['time']))
            finally:
                session.close()
    return dict(passed=True, frames=len(record['frames']), replayed=replay,
                scope='Integrity is self-consistency, not proof of authenticity or business validity.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('record', type=Path)
    parser.add_argument('--replay', action='store_true')
    args = parser.parse_args()
    print(json.dumps(verify(json.loads(args.record.read_text()), args.replay), indent=2))
