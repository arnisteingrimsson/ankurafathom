"""Cross-check the native byte patterns with Python's independent hashlib."""
import hashlib
import json
from pathlib import Path
import sys

cases = json.loads(Path(sys.argv[1]).read_text())
expected = list(range(130)) + [255, 256, 257, 4095, 4096, 4097, 65535, 65536, 65537, 1000000, 4194304]
assert [c['length'] for c in cases] == expected, 'missing or duplicated SHA-256 cases'
block = bytes((i * 131 + 17) % 256 for i in range(256))
for case in cases:
    size = case['length']
    data = (block * (size // 256 + 1))[:size]
    assert case['sha256'] == hashlib.sha256(data).hexdigest(), f'hash mismatch at {size} bytes'
print(f'{len(cases)} native binary hashes agree with hashlib')
