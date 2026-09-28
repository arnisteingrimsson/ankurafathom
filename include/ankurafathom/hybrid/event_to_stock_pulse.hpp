#pragma once
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/hybrid/stock_pulse.hpp"
#include <algorithm>
#include <functional>
#include <set>
#include <variant>
#include <type_traits>

namespace ankurafathom::hybrid {

template<class Event, class Message=std::variant<Event,StockPulse>>
class EventToStockPulse final:public devs::Atomic<Message> {
public:
    using Key=std::function<std::uint64_t(const Event&)>;
    using Amount=std::function<std::vector<double>(const Event&,double)>;
    static constexpr std::uint32_t input_port=0,output_port=1;
    EventToStockPulse(std::string channel,std::size_t width,Key key,Amount amount)
        :channel_(std::move(channel)),width_(width),key_(std::move(key)),amount_(std::move(amount)) {
        if(channel_.empty() || !width_ || !key_ || !amount_)
            throw std::invalid_argument("invalid typed event pulse configuration");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<EventToStockPulse>(*this); }
    double time_advance()const override { return pending_?0:std::numeric_limits<double>::infinity(); }
    std::optional<double> next_event_time()const override {
        return pending_?std::optional<double>(clock_):std::optional<double>(std::numeric_limits<double>::infinity());
    }
    std::vector<devs::PortValue<Message>> output()const override {
        if(!pending_) return {};
        return {{output_port,Message{*pending_}}};
    }
    void internal_transition()override {
        if(!pending_) throw std::logic_error("event pulse has no pending publication");
        pending_.reset();
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        external_transition_at(clock_+elapsed,elapsed,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(pending_) throw std::logic_error("pending event pulse requires confluence");
        if(!std::isfinite(elapsed) || elapsed<0 || !std::isfinite(time) || time<clock_ || time-clock_!=elapsed)
            throw std::invalid_argument("invalid event pulse timestamp");
        auto candidate=*this;
        candidate.accept(time,bag);
        *this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(!pending_) throw std::logic_error("event pulse has no confluent publication");
        auto candidate=*this;
        candidate.pending_.reset();candidate.accept(clock_,bag);
        *this=std::move(candidate);
    }
    double time()const noexcept { return clock_; }
    std::size_t event_count()const noexcept { return seen_.size(); }
    std::optional<std::uint64_t> revision()const noexcept { return revision_; }
private:
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.empty()) throw std::invalid_argument("empty event pulse bag");
        std::vector<std::pair<std::uint64_t,const Event*>> ordered;
        for(const auto& input:bag) {
            const auto* event=std::get_if<Event>(&input.value);
            if(input.port!=input_port || !event) throw std::invalid_argument("invalid typed event pulse input");
            const auto key=key_(*event);
            if(!seen_.insert(key).second) throw std::invalid_argument("duplicate event pulse key");
            ordered.emplace_back(key,event);
        }
        std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) { return a.first<b.first; });
        std::vector<double> amounts(width_,0);
        for(const auto& [key,event]:ordered) {
            (void)key;
            const auto values=amount_(*event,time);
            if(values.size()!=width_) throw std::invalid_argument("invalid event pulse amount width");
            for(std::size_t i=0;i<width_;++i) {
                if(!std::isfinite(values[i]) || !std::isfinite(amounts[i]+values[i]))
                    throw std::overflow_error("nonfinite event pulse amount");
                amounts[i]+=values[i];
            }
        }
        if(revision_ && *revision_==std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("event pulse revision exhausted");
        revision_=revision_?*revision_+1:0;
        clock_=time;
        pending_=StockPulse{time,channel_,*revision_,std::move(amounts)};
    }
    std::string channel_;
    std::size_t width_;
    Key key_;
    Amount amount_;
    std::set<std::uint64_t> seen_;
    std::optional<StockPulse> pending_;
    std::optional<std::uint64_t> revision_;
    double clock_=0;
};
} // namespace ankurafathom::hybrid
