"""Locate the C++ SDK in an explicitly selected PyArrow environment."""
from pathlib import Path
import sys
import pyarrow

root = Path(pyarrow.__file__).parent
print(pyarrow.get_include())
for name in ('arrow', 'parquet'):
    pattern = f'lib{name}.*dylib' if sys.platform == 'darwin' else f'lib{name}.so*'
    libraries = sorted(root.glob(pattern))
    if not libraries:
        raise RuntimeError(f'PyArrow SDK missing {name} library under {root}')
    print(libraries[0].resolve())
