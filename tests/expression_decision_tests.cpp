#include "ankurafathom/ir/expression.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace ankurafathom::ir;
void require(bool condition) { if(!condition) throw std::runtime_error("decision expression contract failed"); }
template<class F> void invalid(F function) {
    try { function(); } catch(const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid function accepted");
}
int main() {
    try {
        require(Expression("MIN(4,2)+MAX(-4,-2)").evaluate({})==0);
        require(Expression("IF_POSITIVE(1,7,1/0)").evaluate({})==7);
        require(Expression("IF_POSITIVE(0,1/0,8)").evaluate({})==8);
        require(Expression("IF_POSITIVE(-1,missing,9)").evaluate({})==9);
        require(Expression("IF_POSITIVE(1,MIN(2,3),missing(1))").evaluate({})==2);
        const auto hours=Dimension::parse("hours");
        const std::map<std::string,Dimension> units{{"x",hours},{"y",hours},{"c",{}}};
        for(const auto* source:{"MIN(x,y)","MAX(x,y)","IF_POSITIVE(c,x,y)"})
            require(Expression(source).infer_unit(units)==hours);
        invalid([&] { Expression("IF_POSITIVE(x,x,y)").infer_unit(units); });
        invalid([&] { Expression("IF_POSITIVE(1,x,0)").infer_unit(units); });
        invalid([&] { Expression("MIN(x,c)").infer_unit(units); });
        for(const auto* source:{"MIN(1)","MAX(1,2,3)","IF_POSITIVE(1,2)"})
            invalid([&] { (void)Expression(source); });
        bool caught=false;
        try { Expression("IF_POSITIVE(c,1,2)").evaluate({{"c",std::numeric_limits<double>::infinity()}}); }
        catch(const std::domain_error&) { caught=true; }
        require(caught);
        std::cout<<"Decision functions: arithmetic, lazy branches, finite conditions, units and arity pass\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
