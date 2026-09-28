# Arrow C interface declarations

`runtime/include/ankurafathom/arrow_c.h` contains the CPU C Data and C Stream
sections from Apache Arrow 25.0.1 `arrow/c/abi.h` (the pinned PyArrow SDK).
Device and asynchronous interfaces are omitted. Standard interface guards allow
coexistence with an SDK header in either include order. The declarations require
only C11 and stdint.h, without a dependency on the Arrow C++ SDK.

The upstream license and notice are preserved alongside this file.
