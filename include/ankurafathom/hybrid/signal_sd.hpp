#pragma once
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/hybrid/scalar_publication.hpp"
#include "ankurafathom/hybrid/stock_pulse.hpp"
#include "ankurafathom/sd/model.hpp"
#include <map>
#include <variant>
#include <type_traits>

namespace ankurafathom::hybrid {

// Piecewise-constant scalar inputs are value-owned. No flow captures a pointer
// to this atomic or to another component, so checked kernel clones are independent.
template<class Message=std::variant<ScalarPublication,StockPulse>>
class SignalSD final:public devs::Atomic<Message> {
public:
    using State=sd::Model::State;
    using Rate=std::function<double(const State&,const std::vector<double>&,double)>;
    struct Stock { std::string name; double initial; bool nonnegative=true; };
    struct Flow { std::size_t source,destination; Rate rate; bool nonnegative=true; };
    static constexpr auto boundary=sd::Model::boundary;
    static constexpr std::uint32_t input_port=0;
    static constexpr std::uint32_t pulse_port=2;
    SignalSD(std::vector<Stock> stocks,std::vector<Flow> flows,std::vector<double> initial,double dt,
             std::vector<std::string> pulse_channels={})
        :stocks_(std::move(stocks)),flows_(std::move(flows)),signals_(std::move(initial)),dt_(dt),next_tick_(dt) {
        if(!std::isfinite(dt_) || dt_<=0 || (signals_.empty() && pulse_channels.empty()))
            throw std::invalid_argument("invalid signal SD clock or width");
        for(auto& channel:pulse_channels)
            if(channel.empty() || !channels_.emplace(std::move(channel),Channel{}).second)
                throw std::invalid_argument("invalid or duplicate stock pulse channel");
        if(!channels_.empty() && stocks_.empty()) throw std::invalid_argument("stock pulses require stocks");
        for(double value:signals_) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite initial signal");
        sd::Model validation;
        for(const auto& stock:stocks_) {
            validation.add_stock(stock.name,stock.initial,stock.nonnegative);
            state_.push_back(stock.initial);
        }
        for(const auto& flow:flows_) {
            if(!flow.rate) throw std::invalid_argument("empty signal SD flow");
            validation.add_flow(flow.source,flow.destination,[](const State&,double) { return 0.; },flow.nonnegative);
        }
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<SignalSD>(*this); }
    double time_advance()const override { return next_tick_-clock_; }
    std::optional<double> next_event_time()const override { return next_tick_; }
    std::vector<devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override {
        auto candidate=*this;
        candidate.integrate(next_tick_);candidate.advance_tick();
        *this=std::move(candidate);
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(bag.empty()) throw std::invalid_argument("signal SD requires an input batch");
        const double time=std::visit([](const auto& value)->double {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,ScalarPublication> || std::is_same_v<T,StockPulse>) return value.time;
            else throw std::invalid_argument("invalid signal SD message type");
        },bag.front().value);
        external_transition_at(time,elapsed,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || !std::isfinite(time) || time<clock_ || time-clock_!=elapsed)
            throw std::invalid_argument("invalid signal SD timestamp");
        auto candidate=*this;
        candidate.accept(time,bag);
        *this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        auto candidate=*this;
        candidate.accept(next_tick_,bag);candidate.advance_tick();
        *this=std::move(candidate);
    }
    const State& state()const noexcept { return state_; }
    const std::vector<double>& signals()const noexcept { return signals_; }
    std::optional<std::uint64_t> revision()const noexcept { return revision_; }
    std::optional<std::uint64_t> pulse_revision(const std::string& channel)const { return channels_.at(channel).revision; }
    double time()const noexcept { return clock_; }
private:
    void integrate(double target) {
        if(!std::isfinite(target) || target<clock_ || target>next_tick_)
            throw std::invalid_argument("signal SD input lies outside its current interval");
        if(target==clock_) return;
        sd::Model model;
        for(std::size_t i=0;i<stocks_.size();++i) model.add_stock(stocks_[i].name,state_[i],stocks_[i].nonnegative);
        for(const auto& flow:flows_)
            model.add_flow(flow.source,flow.destination,[rate=flow.rate,signals=signals_](const State& state,double time) {
                return rate(state,signals,time);
            },flow.nonnegative);
        model.step(clock_,target-clock_);
        state_=model.state();
        clock_=target;
    }
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.empty()) throw std::invalid_argument("signal SD requires an input batch");
        const ScalarPublication* publication=nullptr;
        std::size_t scalar_owner=0;
        std::map<std::string,const StockPulse*> pulses;
        for(const auto& input:bag) std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,ScalarPublication>) {
                if(publication || input.port!=input_port || value.time!=time || signals_.empty() ||
                   value.values.size()!=signals_.size() || (revision_ && value.revision<=*revision_) ||
                   (owner_ && *owner_!=input.source))
                    throw std::invalid_argument("invalid scalar width, timestamp, revision or owner");
                for(double x:value.values) if(!std::isfinite(x)) throw std::invalid_argument("nonfinite scalar publication");
                publication=&value;scalar_owner=input.source;
            } else if constexpr(std::is_same_v<T,StockPulse>) {
                const auto found=channels_.find(value.channel);
                if(input.port!=pulse_port || value.time!=time || value.amounts.size()!=state_.size() ||
                   found==channels_.end() || !pulses.emplace(value.channel,&value).second)
                    throw std::invalid_argument("invalid stock pulse channel, port, timestamp or width");
                auto& channel=found->second;
                if((channel.revision && value.revision<=*channel.revision) ||
                   (channel.owner && *channel.owner!=input.source))
                    throw std::invalid_argument("invalid stock pulse revision or owner");
                for(double x:value.amounts) if(!std::isfinite(x)) throw std::invalid_argument("nonfinite stock pulse");
                channel.owner=input.source;channel.revision=value.revision;
            } else throw std::invalid_argument("invalid signal SD message type");
        },input.value);
        integrate(time); // Last committed inputs fund the interval ending here.
        std::vector<double> net(state_.size(),0);
        for(const auto& [channel,pulse]:pulses) {
            (void)channel;
            for(std::size_t i=0;i<net.size();++i) {
                net[i]+=pulse->amounts[i];
                if(!std::isfinite(net[i])) throw std::overflow_error("stock pulse sum overflow");
            }
        }
        for(std::size_t i=0;i<state_.size();++i) {
            state_[i]+=net[i];
            if(!std::isfinite(state_[i]) || (stocks_[i].nonnegative && state_[i]<0))
                throw std::invalid_argument("stock pulse violates stock domain");
        }
        if(publication) {
            signals_=publication->values;revision_=publication->revision;owner_=scalar_owner;
        }
    }
    void advance_tick() {
        if(tick_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("signal SD tick index exhausted");
        ++tick_;
        next_tick_=static_cast<double>(tick_)*dt_;
        if(!std::isfinite(next_tick_) || next_tick_<=clock_) throw std::overflow_error("signal SD tick did not advance");
    }
    std::vector<Stock> stocks_;
    std::vector<Flow> flows_;
    State state_;
    std::vector<double> signals_;
    std::optional<std::size_t> owner_;
    std::optional<std::uint64_t> revision_;
    struct Channel { std::optional<std::size_t> owner; std::optional<std::uint64_t> revision; };
    std::map<std::string,Channel> channels_;
    double dt_,next_tick_,clock_=0;
    std::uint64_t tick_=1;
};
} // namespace ankurafathom::hybrid
