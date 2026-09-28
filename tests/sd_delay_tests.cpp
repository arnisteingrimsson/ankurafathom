#include "ankurafathom/sd/delay.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using ankurafathom::sd::DelayKind;
using ankurafathom::sd::EulerDelay;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_equilibrium_and_first_order() {
    EulerDelay one(DelayKind::material, 1, 2, 4);
    require(one.pipeline() == 8 && one.output(2) == 4,
            "DELAY1 initial pipeline or output is wrong");
    one.step(4, 2, 0.5);
    require(one.pipeline() == 8 && one.output(2) == 4,
            "constant material input should preserve the equilibrium pipeline");
    EulerDelay three(DelayKind::material, 3, 6, 4);
    require(three.pipeline() == 24 && three.output(6) == 4,
            "DELAY3 initial pipeline or output is wrong");
    three.step(4, 6, 1);
    require(three.pipeline() == 24 && three.output(6) == 4,
            "three-stage equilibrium should remain fixed");

    EulerDelay pulse(DelayKind::material, 1, 2, 0);
    pulse.step(10, 2, 1);
    require(pulse.pipeline() == 10 && pulse.output(2) == 5,
            "first-order material Euler step is wrong");
    pulse.step(0, 2, 1);
    require(pulse.pipeline() == 5 && pulse.output(2) == 2.5,
            "first-order material output is wrong after input stops");
}

void test_three_stage_impulse_and_conservation() {
    EulerDelay delay(DelayKind::material, 3, 3, 0);
    delay.step(1, 3, 1);
    require(delay.stages()[0] == 1 && delay.stages()[1] == 0 &&
            delay.stages()[2] == 0 && delay.output(3) == 0,
            "first stage did not receive the impulse");
    delay.step(0, 3, 1);
    require(delay.stages()[1] == 1 && delay.output(3) == 0,
            "second stage did not receive the impulse");
    delay.step(0, 3, 1);
    require(delay.stages()[2] == 1 && delay.output(3) == 1,
            "third stage did not receive the impulse");
    delay.step(0, 3, 1);
    require(delay.pipeline() == 0, "impulse was not completely released");

    EulerDelay changing(DelayKind::material, 3, 3, 2);
    const double initial_pipeline = changing.pipeline();
    double cumulative_input = 0;
    double cumulative_output = 0;
    for (const auto [input, duration] : {
            std::pair<double, double>{4, 3}, {0, 6}, {1, 3}, {5, 4}}) {
        const double dt = 0.25;
        const double outgoing = changing.output(duration);
        changing.step(input, duration, dt);
        cumulative_input += input * dt;
        cumulative_output += outgoing * dt;
        require(std::abs(changing.pipeline() -
                (initial_pipeline + cumulative_input - cumulative_output)) < 1e-12,
                "variable-delay material conservation failed");
    }
}

void test_information_delay_and_invalid_step() {
    EulerDelay smooth(DelayKind::information, 3, 3, 0);
    smooth.step(10, 3, 1);
    require(smooth.stages()[0] == 10 && smooth.output(3) == 0,
            "first information stage is wrong");
    smooth.step(10, 3, 1);
    require(smooth.stages()[1] == 10 && smooth.output(3) == 0,
            "second information stage is wrong");
    smooth.step(10, 3, 1);
    require(smooth.output(3) == 10, "third information stage is wrong");
    bool caught = false;
    try { smooth.step(10, 2, 1); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && smooth.output(3) == 10,
            "time step larger than stage duration must fail without changing state");
    caught = false;
    try { (void)EulerDelay(DelayKind::material, 0, 3, 0); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "unsupported delay order must fail");
}

void test_abrupt_two_stage_hand_oracle() {
    EulerDelay delay(DelayKind::material, 2, 4, 2);
    require(delay.stages()[0]==4 && delay.stages()[1]==4, "two-stage initialization");
    delay.step(6,4,.5);
    require(delay.stages()[0]==6 && delay.stages()[1]==4 && delay.output(2)==4,
            "shorter duration must affect output immediately without rescaling contents");
    delay.step(0,2,.5);
    require(delay.stages()[0]==3 && delay.stages()[1]==5 && delay.output(6)==5./3,
            "longer duration must use current duration and shared old stages");
    delay.step(3,6,.5);
    require(delay.stages()[0]==4 && std::abs(delay.stages()[1]-14./3)<1e-14 &&
            std::abs(delay.pipeline()-(8+4.5-23./6))<1e-14,
            "hand-calculated stages and cumulative material balance");
}

void test_duration_changes_and_convergence() {
    for (std::size_t order : {1u, 3u}) {
        EulerDelay material(DelayKind::material, order, 3, 10);
        EulerDelay information(DelayKind::information, order, 3, 10);
        const auto stages = material.stages();
        require(material.output(6) == 5 && material.pipeline() == 30 && material.stages() == stages,
                "duration change must alter material rate without rescaling pipeline");
        require(information.output(6) == 10, "duration change must not rescale information output");
        double balance = material.pipeline();
        for (int tick = 0; tick < 100; ++tick) {
            const double duration = tick % 3 == 0 ? 1.5 : tick % 3 == 1 ? 6 : 3;
            const double input = tick % 2 == 0 ? 2 : 9;
            balance += .125*(input-material.output(duration));
            material.step(input, duration, .125);
            information.step(input, duration, .125);
            require(std::abs(balance-material.pipeline()) < 1e-11, "variable-duration material balance");
            for (double value : information.stages())
                require(value >= 2 && value <= 10, "variable-duration smoothing must preserve input/initial bounds");
        }
        const auto before = material.stages();
        bool caught = false;
        try { material.step(1, .1, 1); } catch (const std::invalid_argument&) { caught = true; }
        require(caught && material.stages() == before, "invalid variable duration must preserve all stages");

        // tau(t)=2+t/2; transform time u=2*order*log((2+t/2)/2).
        // The continuous unit-step response has closed form for each order.
        const double u = 2*order*std::log(2.);
        const double exact = 1-std::exp(-u)*(order==1 ? 1 : 1+u+u*u/2);
        double previous = 0;
        for (int steps : {80, 160, 320}) {
            EulerDelay smooth(DelayKind::information, order, 2, 0);
            const double dt = 4./steps;
            for (int i=0; i<steps; ++i) smooth.step(1, 2+i*dt/2, dt);
            const double error = std::abs(smooth.output(4)-exact);
            if (previous > 0) require(previous/error > 1.9 && previous/error < 2.1,
                                      "variable-duration smooth must converge at Euler order");
            previous = error;
        }
    }
}

void test_general_orders() {
    for (std::size_t order : {2u, 5u, 8u, 255u}) {
        EulerDelay material(DelayKind::material, order, order, 0);
        EulerDelay smooth(DelayKind::information, order, order, 0);
        // At dt=tau/N, each stage shifts exactly once per tick.
        for (std::size_t tick=1; tick<=order+2; ++tick) {
            material.step(tick==1 ? 7 : 0, order, 1);
            smooth.step(tick==1 ? -7 : 0, order, 1);
            require(material.output(order) == (tick==order ? 7 : 0), "N-stage impulse timing");
            require(smooth.output(order) == (tick==order ? -7 : 0), "N-stage signed smoothing timing");
            require(material.pipeline() == (tick<=order ? 7 : 0), "N-stage pulse conservation");
        }
    }
    for (std::size_t order : {2u, 5u, 8u}) {
        EulerDelay material(DelayKind::material, order, 3, 2);
        double balance = material.pipeline();
        for (int k=0; k<200; ++k) {
            const double tau = k%2 ? 1.5 : 6;
            const double input = k%3 ? 4 : 0;
            balance += .05*(input-material.output(tau));
            material.step(input, tau, .05);
            require(std::abs(material.pipeline()-balance) < 1e-11, "N-stage variable duration balance");
        }
        // Analytic Erlang step response under tau(t)=2+t/2.
        const double u = 2*order*std::log(2.);
        double term=1, sum=1;
        for (std::size_t j=1; j<order; ++j) { term *= u/j; sum += term; }
        const double exact = 1-std::exp(-u)*sum;
        double previous=0;
        for (int count : {320, 640, 1280}) {
            EulerDelay smooth(DelayKind::information, order, 2, 0);
            const double dt=4./count;
            for (int k=0; k<count; ++k) smooth.step(1, 2+k*dt/2, dt);
            const double error=std::abs(smooth.output(4)-exact);
            if (previous) require(previous/error>1.9 && previous/error<2.1, "N-stage Euler convergence");
            previous=error;
        }
    }
    for (auto kind : {DelayKind::material, DelayKind::information, DelayKind::fixed}) {
        bool caught=false;
        try { EulerDelay invalid(kind, 256, 1, 0); } catch (const std::invalid_argument&) { caught=true; }
        require(caught, "unbounded delay order accepted");
    }
    bool caught=false;
    try { EulerDelay invalid(DelayKind::fixed, 1, 1, 0); } catch (const std::invalid_argument&) { caught=true; }
    require(caught, "fixed kind accepted by Euler cascade");
}

void test_fixed_delays() {
    using ankurafathom::sd::FixedDelay;
    for (std::size_t ticks : {1u, 2u, 5u, 255u}) {
        const double dt=.125, duration=ticks*dt;
        FixedDelay delay(duration, dt, -3);
        for (std::size_t k=0; k<3*ticks+4; ++k) {
            require(delay.output(duration) == (k<ticks ? -3 : static_cast<double>(k-ticks)),
                    "fixed delay must return input exactly N ticks earlier across ring wrap");
            delay.step(k, duration, dt);
        }
        auto copy=delay;
        const double before=delay.output(duration);
        for (int failure=0; failure<3; ++failure) {
            bool caught=false;
            try { delay.step(failure==0 ? std::numeric_limits<double>::infinity() : 2,
                             failure==1 ? duration+dt : duration, failure==2 ? dt*2 : dt); }
            catch (const std::invalid_argument&) { caught=true; }
            require(caught && delay.output(duration)==before, "fixed rejection mutated visible output");
        }
        for (std::size_t k=0; k<ticks+1; ++k) {
            delay.step(7,duration,dt); copy.step(7,duration,dt);
            require(delay.output(duration)==copy.output(duration), "failed fixed step mutated buffered history");
        }
    }
    require(FixedDelay::ticks(.3,.1)==3, "binary rounding of whole ticks");
    require(FixedDelay::ticks(1000000,1)==1000000, "fixed tick cap boundary");
    for (double duration : {0., -1., .5, 1.5, 1000001., std::numeric_limits<double>::infinity()}) {
        bool caught=false;
        try { FixedDelay bad(duration,1,0); } catch (const std::invalid_argument&) { caught=true; }
        require(caught, "invalid fixed duration accepted");
    }
}

void test_history_two() {
    using ankurafathom::sd::HistoryDelay2;
    HistoryDelay2 delay(4,2,.5);
    delay.step(6,4,.5);
    require(delay.stages()[0]==6 && delay.stages()[1]==4 && delay.output(2)==2,
            "history output must ignore current duration change");
    delay.step(0,2,.5);
    require(delay.stages()[0]==3 && delay.stages()[1]==6 && delay.output(6)==6,
            "history first stage uses current duration, output uses previous");
    delay.step(3,6,.5);
    require(delay.stages()[0]==4 && delay.stages()[1]==3.5 && delay.pipeline()==7.5,
            "history hand stages conserve 8+4.5-5");
    require(delay.output(2)==7./6 && delay.output(6)==7./6,
            "observations must not advance duration history");
    for (int which=0;which<5;++which) {
        const auto before=delay.stages();const auto previous=delay.previous_duration();
        bool caught=false;
        try { delay.step(which==0 ? std::numeric_limits<double>::infinity() : which==4 ? 1e308 : 1,
                         which==1 ? .5 : 4, which==2 ? .25 : which==3 || which==4 ? 2 : .5); }
        catch (const std::exception&) {caught=true;}
        require(caught && delay.stages()==before && delay.previous_duration()==previous,
                "failed history step must preserve stages and duration");
    }
    HistoryDelay2 large(4,0,2);
    bool overflow=false;
    try {large.step(1e308,4,2);} catch (const std::overflow_error&) {overflow=true;}
    require(overflow && large.pipeline()==0 && large.previous_duration()==4,"history overflow rollback");
    for (double dt:{.125,.0625}) for (double after:{2.,4.,6.}) {
        HistoryDelay2 drained(4,2,dt);double emitted=0;
        for (int tick=0;tick<static_cast<int>(256/dt);++tick) {
            const double duration=tick*dt<1 ? 4 : after;
            emitted+=dt*drained.output(duration);drained.step(0,duration,dt);
            require(std::abs(emitted+drained.pipeline()-8)<2e-12,"history quantity conservation");
        }
        require(std::abs(emitted-8)<2e-12,"history full release");
    }
    HistoryDelay2 signed_delay(4,-2,.5);double balance=-8;
    for (int k=0;k<100;++k) {
        const double tau=k%2 ? 2 : 6, input=k%3 ? -3 : 4;
        balance+=.5*(input-signed_delay.output(tau));signed_delay.step(input,tau,.5);
        require(std::abs(signed_delay.pipeline()-balance)<1e-12,"signed history balance");
    }
    for (double tau:{0.,-1.,.5,std::numeric_limits<double>::infinity()}) {
        bool caught=false;try {HistoryDelay2 invalid(tau,1,.5);} catch (const std::exception&) {caught=true;}
        require(caught,"invalid history initial duration accepted");
    }
}

} // namespace

int main() {
    try {
        test_equilibrium_and_first_order();
        test_three_stage_impulse_and_conservation();
        test_information_delay_and_invalid_step();
        test_abrupt_two_stage_hand_oracle();
        test_duration_changes_and_convergence();
        test_general_orders();
        test_fixed_delays();
        test_history_two();
        std::cout << "SD delay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
