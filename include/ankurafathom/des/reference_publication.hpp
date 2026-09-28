#pragma once

#include "ankurafathom/devs/simulator.hpp"
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ankurafathom::des::detail {

template<class Derived,class Message>
class PublicationAtomic:public devs::Atomic<Message> {
public:
    std::unique_ptr<devs::Atomic<Message>> clone()const override {
        return std::make_unique<Derived>(static_cast<const Derived&>(*this));
    }
    double time_advance()const override { return pending_.empty() ? std::numeric_limits<double>::infinity() : 0; }
    std::optional<double> next_event_time()const override {
        return pending_.empty() ? std::numeric_limits<double>::infinity() : clock_;
    }
    std::vector<devs::PortValue<Message>> output()const override { return pending_; }
    void internal_transition()override {
        if(pending_.empty()) throw std::logic_error("resource block has no publication");
        pending_.clear();
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid resource block elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time-clock_!=elapsed ||
            (!pending_.empty() && elapsed!=0)) throw std::invalid_argument("invalid resource block timestamp");
        auto& self=static_cast<Derived&>(*this);
        Derived candidate(self);
        candidate.clock_=time;
        candidate.accept(bag);
        self=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(pending_.empty()) throw std::logic_error("resource block has no confluent publication");
        auto& self=static_cast<Derived&>(*this);
        Derived candidate(self);
        candidate.pending_.clear();
        candidate.accept(bag);
        self=std::move(candidate);
    }
protected:
    double clock_=0;
    std::vector<devs::PortValue<Message>> pending_;
};

} // namespace ankurafathom::des::detail
