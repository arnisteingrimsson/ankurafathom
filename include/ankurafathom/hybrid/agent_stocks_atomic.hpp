#pragma once
#include "ankurafathom/hybrid/agent_stocks.hpp"
#include "ankurafathom/hybrid/population_aggregate.hpp"

namespace ankurafathom::hybrid {

template<class Tag,class Message=AggregateMessage<Tag>>
class AgentStocksAtomic final:public devs::Atomic<Message> {
public:
    using Core=AgentStocks<Tag>;
    using Snapshot=PopulationSnapshot<Tag>;
    static constexpr std::uint32_t output_port=1;
    AgentStocksAtomic(Core core,double dt):core_(std::move(core)),dt_(dt),next_tick_(dt) {
        if(core_.time()!=0 || !std::isfinite(dt_) || dt_<=0)
            throw std::invalid_argument("agent-stock publisher requires time zero and a positive finite step");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<AgentStocksAtomic>(*this); }
    double time_advance()const override { return pending_ ? 0 : next_tick_-core_.time(); }
    std::optional<double> next_event_time()const override { return pending_ ? core_.time() : next_tick_; }
    std::vector<devs::PortValue<Message>> output()const override {
        if(!pending_) return {};
        return {{output_port,Message{Snapshot{core_.time(),tick_,core_.store()}}}};
    }
    void internal_transition()override {
        if(pending_) { pending_=false;return; }
        auto candidate=*this;
        candidate.integrate();
        *this=std::move(candidate);
    }
    void external_transition(double,const std::vector<devs::Input<Message>>&)override {
        throw std::invalid_argument("autonomous agent-stock publisher does not accept inputs");
    }
    void confluent_transition(const std::vector<devs::Input<Message>>&)override {
        throw std::invalid_argument("autonomous agent-stock publisher does not accept confluent inputs");
    }
    const Core& core()const noexcept { return core_; }
    std::uint64_t revision()const noexcept { return tick_; }
private:
    void integrate() {
        if(tick_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("agent-stock tick index exhausted");
        core_.step(next_tick_-core_.time());
        if(core_.time()!=next_tick_) throw std::overflow_error("agent-stock absolute tick was not preserved");
        ++tick_;
        // The next index must also remain representable, before forming its product.
        if(tick_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("agent-stock next tick index exhausted");
        next_tick_=static_cast<double>(tick_+1)*dt_;
        if(!std::isfinite(next_tick_) || next_tick_<=core_.time())
            throw std::overflow_error("agent-stock next tick did not advance");
        pending_=true;
    }
    Core core_;
    double dt_,next_tick_;
    std::uint64_t tick_=0;
    bool pending_=true;
};
} // namespace ankurafathom::hybrid
