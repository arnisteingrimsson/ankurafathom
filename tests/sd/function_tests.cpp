#include "ankurafathom/sd/test_inputs.hpp"
#include "ankurafathom/sd/smooth.hpp"
#include "ankurafathom/ir/expression.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace ankurafathom;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    check(caught, "invalid function argument accepted");
}
void boundaries() {
    using namespace sd;
    check(step_input(7, 1, 0, 1) == 0 && step_input(7, 1, 1, 1) == 7, "STEP grid boundary");
    check(step_input(-7, .5, 0, 1) == 0, "STEP exact half-step is excluded");
    check(step_input(-7, std::nextafter(.5, 0.), 0, 1) == -7, "STEP just before half-step");
    check(step_input(7, std::nextafter(.5, 1.), 0, 1) == 0, "STEP just after half-step");
    check(pulse_input(1, 0, 0, 1) == 0 && pulse_input(1, 0, 1, 1) == 1 &&
          pulse_input(1, 0, 2, 1) == 0, "zero width PULSE is one tick");
    check(pulse_input(.5, 1, 0, 1) == 0 && pulse_input(.5, 1, 1, 1) == 0,
          "PULSE strict endpoints");
    check(pulse_input(std::nextafter(.5, 0.), 1, 0, 1) == 1, "PULSE start neighbor");
    check(pulse_input(0, std::nextafter(.5, 1.), 0, 1) == 1 &&
          pulse_input(0, .5, 0, 1) == 0, "PULSE end neighbor");
    double area = 0;
    for (int i = 0; i < 8; ++i) area += .25*pulse_input(.5, .5, i*.25, .25);
    check(area == .5, "PULSE unit height, not unit area");
    check(ramp_input(2, 1, 3, .75, 1) == 0 && ramp_input(2, 1, 3, 1, 1) == 0 &&
          ramp_input(2, 1, 3, 2, 1) == 2 && ramp_input(2, 1, 3, 5, 1) == 4,
          "RAMP uses tick time and holds terminal height");
    check(ramp_input(-2, 1, 3, 2, 1) == -2 && ramp_input(2, 1, 1, 2, 1) == 0, "signed/empty RAMP");
    const auto inf = std::numeric_limits<double>::infinity();
    rejects([&]{ step_input(inf, 0, 0, 1); });
    rejects([&]{ step_input(1, 0, 0, 0); });
    rejects([&]{ step_input(1, 0, 1e30, 1); });
    rejects([&]{ pulse_input(0, -1, 0, 1); });
    rejects([&]{ pulse_input(1e30, 1, 0, 1); });
    rejects([&]{ ramp_input(1, 2, 1, 0, 1); });
    rejects([&]{ ramp_input(1e308, 0, 10, 9, 1); });
}
void smoothing() {
    for (const auto order : {1u, 3u}) {
        sd::EulerSmooth equilibrium(order, -4, 3);
        equilibrium.step(-4, 6, .5);
        check(equilibrium.output() == -4, "SMOOTH defaults to initial input");
    }
    sd::EulerSmooth one(1, 10, 2, 0);
    for (double expected : {5., 7.5, 8.75}) {
        one.step(10, 2, 1); check(one.output() == expected, "SMOOTHI recurrence");
    }
    one.step(-3.25, 4, 1);
    check(one.output() == 5.75, "variable duration signed SMOOTH");
    const auto before = one.stages();
    rejects([&]{ one.step(10, .25, 1); });
    check(one.stages() == before && one.output() == 5.75, "failed SMOOTH step mutated state");
    sd::EulerSmooth three(3, 10, 3, 0);
    for (double expected : {0., 0., 1.25, 3.125}) {
        three.step(10, 3, .5); check(three.output() == expected, "SMOOTH3I shared old stages");
    }
    rejects([]{ sd::EulerSmooth bad(0, 1, 3); });
    rejects([]{ sd::EulerSmooth bad(256, 1, 3); });
    rejects([]{ sd::EulerSmooth bad(1, std::numeric_limits<double>::quiet_NaN(), 3, 0); });
    // Closed continuous step response; require first-order convergence at t=4.
    for (const auto order : {1u, 3u}) {
        double previous = 0;
        const double exact = order == 1 ? 1-std::exp(-4.) : 1-13*std::exp(-4.);
        for (const int steps : {40, 80, 160}) {
            sd::EulerSmooth smooth(order, 1, order, 0);
            for (int i=0; i<steps; ++i) smooth.step(1, order, 4./steps);
            const double error = std::abs(smooth.output()-exact);
            if (previous) check(previous/error > 1.9 && previous/error < 2.2, "SMOOTH Euler convergence");
            previous = error;
        }
    }
}
void xmile_inputs() {
    using namespace sd;
    for (double dt : {.1,.125,.5,1.}) {
        double area=0;
        for (int k=0;k<20;++k) {
            const double t=k*dt;
            const double pulse=xmile_pulse(-3,2*dt,4*dt,t,dt);
            check(pulse==(k>=2 && (k-2)%4==0 ? -3/dt : 0), "XMILE periodic tick schedule");
            area+=dt*pulse;
            check(xmile_step(7,2*dt,t,dt)==(k>=2 ? 7 : 0), "XMILE STEP includes event tick");
            check(std::abs(xmile_ramp(-2,2*dt,t,dt)-(-2*std::max(k-2,0)*dt))<1e-13, "XMILE RAMP no end argument");
            check(xmile_pulse(5,2*dt,0,t,dt)==(k==2 ? 5/dt : 0), "XMILE one-shot quantity pulse");
        }
        check(std::abs(area+15)<1e-13, "XMILE pulse quantity must not depend on dt");
    }
    check(xmile_pulse(2,-2,2,0,1)==2, "pulse schedule may begin before simulation start");
    for (double first : {.5,1e30,std::numeric_limits<double>::infinity()})
        rejects([&]{ xmile_step(1,first,0,1); });
    for (double interval : {-.1,.25,1.5,std::numeric_limits<double>::quiet_NaN()})
        rejects([&]{ xmile_pulse(1,0,interval,0,1); });
    rejects([]{ xmile_pulse(1e308,0,0,0,.125); });
    rejects([]{ xmile_ramp(1e308,0,2,1); });
    rejects([]{ xmile_step(1,0,1e30,1); });
    using ir::Expression; using ir::Dimension;
    const std::map<std::string,Dimension> units{{"t",Dimension::parse("day")}, {"dt",Dimension::parse("day")},
        {"amount",Dimension::parse("kg")}, {"slope",Dimension::parse("kg/day")}};
    check(Expression("XMILE_PULSE(amount,1,2)").infer_unit(units)==Dimension::parse("kg/day"),"XMILE pulse units");
    check(Expression("XMILE_RAMP(slope,1)").infer_unit(units)==Dimension::parse("kg"),"XMILE ramp units");
    check(Expression("XMILE_STEP(amount,1)").infer_unit(units)==Dimension::parse("kg"),"XMILE step units");
    rejects([&]{ Expression("XMILE_PULSE(amount,amount,2)").infer_unit(units); });
    rejects([]{ Expression bad("XMILE_RAMP(1,2,3)"); });
    rejects([]{ Expression bad("XMILE_PULSE(1,2)"); });
}
void expressions() {
    using ir::Expression; using ir::Dimension;
    const Expression filter("NONNEGATIVE(x)");
    check(filter.symbols() == std::set<std::string>{"x"}, "pure filter must not inject clock symbols");
    check(!std::signbit(filter.evaluate({{"x",-0.}})), "filter must canonicalize signed zero like clipped flows");
    for (const double value : {-7., 0., 4., 1e308})
        check(filter.evaluate({{"x",value}}) == (value < 0 ? 0 : value), "nonnegative filter value");
    check(filter.infer_unit({{"x",Dimension::parse("kg/day")}}) == Dimension::parse("kg/day"),
          "filter must preserve physical dimensions");
    for (const double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()})
        rejects([&]{ filter.evaluate({{"x",value}}); });
    rejects([]{ Expression bad("NONNEGATIVE(1,2)"); });
    rejects([]{ Expression bad("NONNEGATIVE(1/0)"); bad.evaluate({}); });
    const std::map<std::string, double> values{{"t",2},{"dt",1},{"height",4},{"slope",2},{"start",1}};
    const std::map<std::string, Dimension> units{{"t",Dimension::parse("day")},
        {"dt",Dimension::parse("day")},{"height",Dimension::parse("kg/day")},
        {"slope",Dimension::parse("kg/day/day")},{"start",Dimension::parse("day")}, {"bad",{}}};
    Expression expression("STEP(-height,start)+RAMP(slope,1,3)+height*PULSE(2,0)");
    check(expression.evaluate(values) == 2, "composed functions");
    check(expression.infer_unit(units) == Dimension::parse("kg/day"), "function dimensions");
    check(expression.symbols().contains("dt") && expression.symbols().contains("t"), "implicit clock dependency");
    Expression table("STEP(table(height),1)");
    check(table.evaluate(values, {{"table",[](double x){ return 2*x; }}}) == 8, "table nesting");
    check(table.infer_unit(units, {{"table",{Dimension::parse("kg/day"),Dimension::parse("kg/day")}}})
          == Dimension::parse("kg/day"), "table nested dimensions");
    check(Expression("STEP(height,1+2)").infer_unit(units) == Dimension::parse("kg/day"), "literal time context");
    for (const auto* bad : {"STEP(1)", "STEP(1,2,3)", "PULSE()", "RAMP(1,2)", "table(1,2)", "PULSE(1,)"})
        rejects([&]{ Expression invalid(bad); });
    rejects([&]{ Expression("STEP(height,bad)").infer_unit(units); });
    rejects([&]{ Expression("PULSE(height,1)").infer_unit(units); });
    rejects([&]{ Expression("STEP(height/0,1)").evaluate(values); });
}

void next_tick_inputs() {
    using namespace sd;
    for (double delta:{-1e-8,0.,1e-8}) {
        const double first=1+delta;
        check(xmile_next_step(2,first,0,1,1)==(delta<=0 ? 2 : 0),"next-tick STEP boundary");
        check(xmile_next_pulse(3,first,0,0,1,1)==(delta<=0 ? 3 : 0),"pulse boundary includes right endpoint");
        check(xmile_next_pulse(3,first,0,0,2,1)==(delta<=0 ? 0 : 3),"pulse boundary excludes left endpoint");
    }
    check(xmile_next_pulse(3,std::nextafter(1.,2.),0,0,1,1)==3,"documented roundoff snap");
    check(xmile_next_step(2,.5,0,0,1)==0 && xmile_next_step(2,.5,0,1,1)==2,"half-tick STEP does not anticipate");
    check(xmile_next_ramp(-2,.25,0,1,1)==-1.5,"ramp retains fractional elapsed time");
    check(xmile_next_pulse(2,.125,.25,0,1,1)==8,"multiple pulses in one tick");
    check(xmile_next_pulse(2,0,.25,0,0,1)==2,"initial boundary excludes nonexistent negative-index pulses");
    double quantity=0;
    for (int k=0;k<=190;++k) {
        const double rate=xmile_next_pulse(-3,10.05,.3,10,10+k*.1,.1);
        check(rate==(k>=1 && (k-1)%3==0 ? -30 : 0),"long decimal-clock pulse schedule");
        check(rate==xmile_next_pulse(-3,10.05,.3,10,10+k*.1,.1),"observation purity");
        quantity+=rate*.1;
    }
    check(std::abs(quantity+192)<1e-12,"decimal pulse train preserves all 64 quantities");
    for (double interval:{-1.,1e-8,1e7,std::numeric_limits<double>::infinity()})
        rejects([&]{xmile_next_pulse(1,0,interval,0,1,1);});
    rejects([]{xmile_next_pulse(1,0,1e-6,0,3,1);});
    rejects([]{xmile_next_step(1,1e20,1e20,1e20,1);});
    rejects([]{xmile_next_step(1,.25,.25,0,1);});
    rejects([]{xmile_next_pulse(1e308,0,.25,0,1,1);});
    rejects([]{xmile_next_ramp(1e308,.25,0,3,1);});
    using ir::Expression;using ir::Dimension;
    const std::map<std::string,Dimension> units{{"t",Dimension::parse("day")},{"dt",Dimension::parse("day")},
        {"q",Dimension::parse("kg")},{"s",Dimension::parse("kg/day")}};
    check(Expression("XMILE_NEXT_PULSE(q,.25,.5,0)").infer_unit(units)==Dimension::parse("kg/day"),"next pulse units");
    check(Expression("XMILE_NEXT_STEP(q,.25,0)").infer_unit(units)==Dimension::parse("kg"),"next step units");
    check(Expression("XMILE_NEXT_RAMP(s,.25,0)").infer_unit(units)==Dimension::parse("kg"),"next ramp units");
    rejects([&]{Expression("XMILE_NEXT_PULSE(q,.25,.5,q)").infer_unit(units);});
}
}
int main() {
    try { boundaries(); smoothing(); xmile_inputs(); next_tick_inputs(); expressions(); std::cout << "SD function semantics passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
