#include "ankurafathom/ir/model.hpp"
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size) {
    if(size>65536) return 0;
    try {
        // No base directory: bindings reject before filesystem reads. Do not run
        // arbitrary models: fuzzing must not execute unbounded model workloads.
        (void)ankurafathom::ir::load_json({reinterpret_cast<const char*>(data),size});
    } catch(const ankurafathom::ir::Error&) {}
      catch(const std::invalid_argument&) {} // native dimensional/model validation
      catch(const std::domain_error&) {}
      catch(const std::overflow_error&) {} // diagnosed native arithmetic bounds
      catch(const std::out_of_range&) {}
    return 0;
}
