#pragma once
#include "ankurafathom/hybrid/signal_sd.hpp"

namespace ankurafathom::hybrid {

template<class Message=std::variant<ScalarPublication,StockPulse>>
class PublishingSignalSD final:public devs::Atomic<Message> {
public:
    using Core=SignalSD<Message>;
    using Projection=std::function<std::vector<double>(const typename Core::State&,const std::vector<double>&,double)>;
    static constexpr std::uint32_t output_port=1;
    PublishingSignalSD(Core core,Projection projection):core_(std::move(core)),projection_(std::move(projection)) {
        if(core_.time()!=0 || !projection_) throw std::invalid_argument("invalid publishing SD configuration");
        auto values=projection_(core_.state(),core_.signals(),0);
        if(values.empty()) throw std::invalid_argument("publishing SD requires nonempty projection");
        width_=values.size();validate(values);
        pending_=ScalarPublication{0,0,std::move(values)};
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<PublishingSignalSD>(*this); }
    double time_advance()const override { return pending_?0:core_.time_advance(); }
    std::optional<double> next_event_time()const override { return pending_?std::optional<double>(core_.time()):core_.next_event_time(); }
    std::vector<devs::PortValue<Message>> output()const override {
        return pending_?std::vector<devs::PortValue<Message>>{{output_port,Message{*pending_}}}:std::vector<devs::PortValue<Message>>{};
    }
    void internal_transition()override {
        if(pending_) { pending_.reset();return; }
        auto candidate=*this;candidate.core_.internal_transition();candidate.publish();*this=std::move(candidate);
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(pending_) throw std::logic_error("publishing SD has a pending output");
        auto candidate=*this;candidate.core_.external_transition(elapsed,bag);candidate.publish();*this=std::move(candidate);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(pending_) throw std::logic_error("publishing SD has a pending output");
        auto candidate=*this;candidate.core_.external_transition_at(time,elapsed,bag);candidate.publish();*this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        auto candidate=*this;
        if(pending_) candidate.core_.external_transition_at(core_.time(),0,bag);
        else candidate.core_.confluent_transition(bag);
        candidate.publish();*this=std::move(candidate);
    }
    const Core& core()const noexcept { return core_; }
    std::uint64_t revision()const noexcept { return revision_; }
private:
    void validate(const std::vector<double>& values)const {
        if(values.size()!=width_) throw std::invalid_argument("publishing SD projection width changed");
        for(double value:values) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite publishing SD projection");
    }
    void publish() {
        if(revision_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("publishing SD revision exhausted");
        auto values=projection_(core_.state(),core_.signals(),core_.time());validate(values);
        pending_=ScalarPublication{core_.time(),++revision_,std::move(values)};
    }
    Core core_;
    Projection projection_;
    std::size_t width_=0;
    std::uint64_t revision_=0;
    std::optional<ScalarPublication> pending_;
};
} // namespace ankurafathom::hybrid
