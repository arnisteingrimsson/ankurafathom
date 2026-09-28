#include "ankurafathom/ir/expression.hpp"
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size) {
    if(size>65536) return 0;
    try {
        const ankurafathom::ir::Expression expression(std::string(reinterpret_cast<const char*>(data),size));
        std::map<std::string,double> values;
        std::map<std::string,ankurafathom::ir::Dimension> units;
        std::map<std::string,std::function<double(double)>> functions;
        std::map<std::string,std::pair<ankurafathom::ir::Dimension,ankurafathom::ir::Dimension>> function_units;
        for(const auto& name:expression.symbols()) { values[name]=0.5;units[name]={}; }
        for(const auto& name:expression.functions()) { functions[name]=[](double value){return value;};function_units[name]={}; }
        (void)expression.infer_unit(units,function_units);
        (void)expression.evaluate(values,functions);
    } catch(const std::invalid_argument&) {} // grammar, symbols, function contracts
      catch(const std::domain_error&) {} // invalid arithmetic is a diagnosed input
      catch(const std::overflow_error&) {} // time-input functions diagnose unrepresentable endpoints/results
      catch(const std::out_of_range&) {} // missing implicit runtime symbols
    return 0;
}
