# Parser and loader fuzzing

Full checkout required; tests are not included in the production source archive.
Use Clang with its libFuzzer runtime:

```sh
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
  -DFATHOM_ENABLE_SANITIZERS=ON -DFATHOM_ENABLE_FUZZING=ON \
  -DFATHOM_ENABLE_ARROW=OFF -DFATHOM_ENABLE_PYTHON=OFF
cmake --build build-fuzz --target fuzz_expression fuzz_ir_loader -j 3
python3 tests/fuzz/run.py --build build-fuzz --output build-fuzz/evidence --seconds 600
```

The runner creates a new evidence directory, seeds two separate writable corpora,
runs targets concurrently and retains logs, crash inputs, binary hashes, commands,
platform, CMake settings, execution counts and elapsed time in `report.json`.
Nonzero exits, sanitizer diagnostics, missing final statistics and timeouts fail.
Only clean campaigns of at least 600 seconds satisfy `ten_minute_gate`; short CI
smokes do not. Reusing an evidence directory is an error. Mutation counts depend
on host throughput even with the fixed seed. Nightly/manual CI uses 600 seconds
per target, push/PR uses 20 seconds, and evidence is uploaded even on failure.

Fathom code has ASan, UBSan and libFuzzer coverage instrumentation. The expression
target parses, infers units and evaluates with deterministic values and identity
lookup functions. The IR loader validates models without a filesystem base or
simulation execution. File readers, arbitrary trajectory workloads, Arrow SDKs
and Python are outside these targets. Inputs are capped at 65,536 bytes. Expected
validation/arithmetic exceptions are accepted; sanitizer findings and other
exceptions fail. Corpora include all six bounded model modes and deliberately
deep JSON/expressions. The expression interpreter additionally enforces the
source and structural limits in ADR-A08; ordinary regression tests exercise exact
boundaries and structured IR diagnostics.

## Apple Clang without a bundled libFuzzer

The local Apple Clang 21 toolchain does not ship the libFuzzer archive. The local
campaign uses LLVM `llvmorg-21.1.0`, commit
`3623fe661ae35c6c80ac221f14d85be76aa870f1`, fetched only as a test dependency:

```sh
git clone --depth 1 --filter=blob:none --sparse --branch llvmorg-21.1.0 \
  https://github.com/llvm/llvm-project.git build-fuzz/llvm-src
git -C build-fuzz/llvm-src sparse-checkout set compiler-rt/lib/fuzzer
mkdir -p build-fuzz/libfuzzer-runtime
cd build-fuzz/libfuzzer-runtime
CXX='clang++ -fsanitize=address' sh ../llvm-src/compiler-rt/lib/fuzzer/build.sh
cd ../..
cmake -S . -B build-fuzz \
  -DFATHOM_LIBFUZZER_LIBRARY="$PWD/build-fuzz/libfuzzer-runtime/libFuzzer.a"
cmake --build build-fuzz --target fuzz_expression fuzz_ir_loader -j 3
```

ASan on this standalone runtime keeps Apple libc++ container annotations
consistent with the target. Do not add UBSan to libFuzzer itself: upstream's
coverage scanner uses null range arithmetic that UBSan diagnoses at startup.
Fathom remains ASan/UBSan-instrumented. The dependency stays under the build
directory, retains upstream notices and is not linked into distributed Fathom.
