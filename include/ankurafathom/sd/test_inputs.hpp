#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ankurafathom::sd {
// Explicit next-tick source-input policy. Use the schedule origin to compute
// adjacent windows from identical integer boundaries, rather than subtracting
// dt from rounded absolute clocks. Counts telescope, so events cannot duplicate.
inline double input_snap(double value,double scale=1) {
    const double tolerance=8*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(value),scale});
    if (!std::isfinite(value) || std::abs(value)>2000000 || !std::isfinite(tolerance) || tolerance>1e-6)
        throw std::invalid_argument("input schedule exceeds count or clock precision bounds");
    const double nearest=std::round(value);
    return std::abs(value-nearest)<=tolerance ? nearest : value;
}
// Bounded XMILE input dialect. Schedules are aligned with the observation grid;
// no half-step or off-grid event-selection convention is silently substituted.
inline long long xmile_grid_offset(double delta, double dt, double clock_scale=1) {
    if (!std::isfinite(delta) || !std::isfinite(dt) || dt <= 0)
        throw std::invalid_argument("XMILE schedule needs finite times and positive dt");
    const double ticks=delta/dt, nearest=std::round(ticks);
    // Subtracting absolute clocks loses precision even when both are on-grid.
    // Bound that allowance so coarse clocks cannot disguise off-grid schedules.
    const double tolerance=8*std::numeric_limits<double>::epsilon()*
        std::max({1.,std::abs(ticks),clock_scale});
    if (!std::isfinite(ticks) || std::abs(ticks)>2000000 ||
        !std::isfinite(tolerance) || tolerance>1e-6 || std::abs(ticks-nearest)>tolerance)
        throw std::invalid_argument("XMILE schedule requires bounded whole ticks");
    return static_cast<long long>(nearest);
}
inline long long xmile_offset(double first,double time,double dt) {
    if (!std::isfinite(time) || !std::isfinite(first) ||
        !std::isfinite(time+dt) || time+dt<=time ||
        !std::isfinite(first+dt) || first+dt<=first)
        throw std::invalid_argument("XMILE tick is not representable");
    return xmile_grid_offset(time-first,dt,std::max(std::abs(time/dt),std::abs(first/dt)));
}
inline double xmile_step(double height,double first,double time,double dt) {
    if (!std::isfinite(height)) throw std::invalid_argument("nonfinite XMILE STEP height");
    return xmile_offset(first,time,dt)>=0 ? height : 0.;
}
inline double xmile_ramp(double slope,double first,double time,double dt) {
    if (!std::isfinite(slope)) throw std::invalid_argument("nonfinite XMILE RAMP slope");
    const auto ticks=xmile_offset(first,time,dt);
    const double value=ticks>0 ? slope*(ticks*dt) : 0.;
    if (!std::isfinite(value)) throw std::overflow_error("XMILE RAMP overflow");
    return value;
}
inline double xmile_pulse(double magnitude,double first,double interval,double time,double dt) {
    if (!std::isfinite(magnitude) || !std::isfinite(interval) || interval<0)
        throw std::invalid_argument("invalid XMILE PULSE arguments");
    const auto tick=xmile_offset(first,time,dt), repeat=xmile_grid_offset(interval,dt);
    if (interval>0 && repeat<1) throw std::invalid_argument("XMILE PULSE interval must cover a tick");
    if (tick<0 || (repeat==0 ? tick!=0 : tick%repeat!=0)) return 0.;
    const double value=magnitude/dt;
    if (!std::isfinite(value)) throw std::overflow_error("XMILE PULSE magnitude/dt overflow");
    return value;
}

struct NextInputClock { double tick, first, scale; };
inline NextInputClock next_input_clock(double first,double origin,double time,double dt) {
    const auto tick=xmile_offset(origin,time,dt);
    if (!std::isfinite(first) || !std::isfinite(first+dt) || first+dt<=first)
        throw std::invalid_argument("input first tick is not representable");
    const double scale=std::max({1.,std::abs(first/dt),std::abs(origin/dt)});
    return {static_cast<double>(tick),input_snap((first-origin)/dt,scale),scale};
}
inline double xmile_next_step(double height,double first,double origin,double time,double dt) {
    if (!std::isfinite(height)) throw std::invalid_argument("nonfinite STEP height");
    const auto clock=next_input_clock(first,origin,time,dt);
    return clock.tick>=clock.first ? height : 0.;
}
inline double xmile_next_ramp(double slope,double first,double origin,double time,double dt) {
    if (!std::isfinite(slope)) throw std::invalid_argument("nonfinite RAMP slope");
    const auto clock=next_input_clock(first,origin,time,dt);
    const double value=slope*(std::max(0.,clock.tick-clock.first)*dt);
    if (!std::isfinite(value)) throw std::overflow_error("RAMP overflow");
    return value;
}
inline double xmile_next_pulse(double quantity,double first,double interval,double origin,double time,double dt) {
    if (!std::isfinite(quantity) || !std::isfinite(interval) || interval<0)
        throw std::invalid_argument("invalid PULSE arguments");
    const auto clock=next_input_clock(first,origin,time,dt);
    double count;
    if (interval==0) count=clock.first>clock.tick-1 && clock.first<=clock.tick ? 1 : 0;
    else {
        const double period=interval/dt;
        if (!std::isfinite(period) || period<1e-6 || period>1000000)
            throw std::invalid_argument("PULSE interval exceeds supported tick bounds");
        const auto cumulative=[&](double boundary) {
            const double index=input_snap((boundary-clock.first)/period,clock.scale/period);
            return std::max(0.,std::floor(index)+1);
        };
        count=cumulative(clock.tick)-cumulative(clock.tick-1);
    }
    if (count==0) return 0.;
    const double value=(quantity/dt)*count;
    if (!std::isfinite(value)) throw std::overflow_error("PULSE quantity rate overflow");
    return value;
}
// Vensim-style grid test inputs. Pass the integration tick origin, not an RK
// stage time; hold this value over the tick when composing higher-order solvers.
inline double input_tick_midpoint(double time, double dt) {
    if (!std::isfinite(time) || !std::isfinite(dt) || dt <= 0)
        throw std::invalid_argument("test input requires finite time and positive dt");
    const double midpoint = time + dt/2;
    if (!std::isfinite(midpoint) || midpoint <= time)
        throw std::overflow_error("test input tick midpoint is not representable");
    return midpoint;
}
inline double step_input(double height, double start, double time, double dt) {
    if (!std::isfinite(height) || !std::isfinite(start)) throw std::invalid_argument("invalid STEP argument");
    return input_tick_midpoint(time, dt) > start ? height : 0;
}
inline double pulse_input(double start, double width, double time, double dt) {
    const double midpoint = input_tick_midpoint(time, dt);
    if (!std::isfinite(start) || !std::isfinite(width) || width < 0)
        throw std::invalid_argument("invalid PULSE argument");
    if (width == 0) width = dt;
    const double end = start + width;
    if (!std::isfinite(end) || end <= start) throw std::overflow_error("PULSE endpoint is not representable");
    return midpoint > start && midpoint < end ? 1 : 0;
}
inline double ramp_input(double slope, double start, double end, double time, double dt) {
    (void)input_tick_midpoint(time, dt);
    if (!std::isfinite(slope) || !std::isfinite(start) || !std::isfinite(end) || end < start)
        throw std::invalid_argument("invalid RAMP argument");
    if (time <= start) return 0;
    const double value = slope * (std::min(time, end)-start);
    if (!std::isfinite(value)) throw std::overflow_error("RAMP result overflow");
    return value;
}
} // namespace ankurafathom::sd
