# Local installation and packaging

Packaging is intended for local/internal use. No package has been published.
The Python wheel contains the nanobind extension and the versioned Fathom C library;
PyArrow 25.0.1 is an external, pinned dependency. Relative loader paths connect them
inside site-packages. Third-party notices are included in the distribution.

From the project root with the packaging requirements installed:

```sh
.venv-runtime/bin/python -m pip install -r runtime/packaging-requirements.txt
CMAKE_GENERATOR="Unix Makefiles" CMAKE_BUILD_PARALLEL_LEVEL=3 \
  .venv-runtime/bin/python -m build --no-isolation --outdir dist
```

This creates a source archive and builds the wheel from that archive. Production
sources are allowlisted; local models/data, tests, environments and build products
are excluded. The wheel is specific to the build interpreter, architecture and
platform; it does not claim abi3, manylinux, Windows, or cross-platform compatibility.
Use standard build isolation by omitting `--no-isolation` when dependencies may be
installed from the package index. Sanitizer wheels are rejected.

Install the resulting wheel using pip in a fresh virtual environment. Installation
needs neither CMake nor nanobind; source builds need both plus a C++20 toolchain.
PyArrow must be installed alongside ankurafathom in the same site-packages directory.
The wheel has no bundled CLI or C development headers. Those are native SDK artifacts.

## Editable source installation

For development, install from the repository root (the location of `pyproject.toml`):

```sh
CMAKE_GENERATOR="Unix Makefiles" CMAKE_BUILD_PARALLEL_LEVEL=3 \
  .venv-runtime/bin/python -m pip install --no-build-isolation --no-deps -e .
.venv-runtime/bin/python -I tests/editable_contract.py
```

The same pinned build/runtime requirements must already be installed when using
these flags. Python source edits are reflected through the editable import hook;
rerun installation after changing C++ sources. The contract checks the actual
editable installation without adding a package directory to `sys.path`, including
analytic values, thread equality and expression-limit errors across the C ABI.
This is local macOS/arm64 evidence; remote Linux/macOS CI verification is configured
but has not run here.

## Native C SDK and CLI

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DFATHOM_ENABLE_ARROW=OFF
cmake --build build-native --target fathom fathom_c_api -j 3
cmake --install build-native --prefix "$PWD/install-native"
```

The prefix contains `bin/fathom`, the versioned shared C library, public headers,
third-party notices, and a CMake package exporting `AnkuraFathom::c_api`.
Consumers use `find_package(AnkuraFathom CONFIG REQUIRED)` with the prefix in
`CMAKE_PREFIX_PATH`. Installed public headers require no Arrow C++ headers.
Runtime and Development are separate installation components; install both for
C consumers. The C ABI version remains 1, independently of package version 0.1.0.

CSV-only native installs are relocatable without external SDK dependencies. An
Arrow-enabled native install retains a dependency on its externally configured
Arrow/Parquet SDK; it does not bundle that SDK. Python wheels use their separate,
relative PyArrow dependency layout. Native installation does not install the
build-directory Python package; use its wheel for Python installation.

Full-checkout CMake builds keep tests enabled by default. Production-only sdists
must be configured with `BUILD_TESTING=OFF`, as their wheel configuration does.
See the session log for measured local acceptance and platform limitations.
