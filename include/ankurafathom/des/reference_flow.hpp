#pragma once

#include "ankurafathom/des/process_reference.hpp"
#include "ankurafathom/des/reference_publication.hpp"
#include "ankurafathom/rng/categorical.hpp"
#include <functional>

namespace ankurafathom::des {

template<class Tag,class Message=ProcessMessage<Tag>>
class ReferenceSource final:public devs::Atomic<Message> {
    using Token=EntityToken<Tag>;
public:
    struct Entry { double time; Token token; };
    ReferenceSource(std::uint32_t store,std::vector<Entry> schedule):schedule_(std::move(schedule)) {
        double previous=0;
        std::set<std::uint64_t> seen;
        for(const auto& entry:schedule_) {
            if(!std::isfinite(entry.time) || entry.time<previous || entry.token.entity.store!=store ||
                entry.token.entity.id>0xFFFFFFFFFFFFULL || !entry.token.leases.empty() || !seen.insert(entry.token.entity.id).second)
                throw std::invalid_argument("invalid reference source schedule");
            previous=entry.time;
        }
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<ReferenceSource>(*this); }
    double time_advance()const override { return *next_event_time()-clock_; }
    std::optional<double> next_event_time()const override {
        return index_<schedule_.size() ? schedule_[index_].time : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output()const override {
        std::vector<devs::PortValue<Message>> result;
        if(index_==schedule_.size()) return result;
        for(std::size_t i=index_;i<schedule_.size() && schedule_[i].time==schedule_[index_].time;++i)
            result.push_back({0,schedule_[i].token});
        return result;
    }
    void internal_transition()override {
        if(index_==schedule_.size()) throw std::logic_error("reference source is exhausted");
        clock_=schedule_[index_].time;
        do { ++index_; } while(index_<schedule_.size() && schedule_[index_].time==clock_);
    }
    void external_transition(double,const std::vector<devs::Input<Message>>&)override { throw std::logic_error("source cannot receive input"); }
    void confluent_transition(const std::vector<devs::Input<Message>>&)override { throw std::logic_error("source cannot receive input"); }
    std::size_t emitted_count()const noexcept { return index_; }
private:
    std::vector<Entry> schedule_;
    std::size_t index_=0;
    double clock_=0;
};

template<class Tag,class Message=ProcessMessage<Tag>>
class ReferenceSink final:public devs::Atomic<Message> {
    using Token=EntityToken<Tag>;
public:
    using Origin=std::function<double(const Token&)>;
    struct Completion { Token token; double time; };
    ReferenceSink(std::uint32_t store,Origin origin):store_(store),origin_(std::move(origin)) {
        if(!origin_) throw std::invalid_argument("reference sink needs an origin provider");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<ReferenceSink>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override { throw std::logic_error("sink has no internal event"); }
    void confluent_transition(const std::vector<devs::Input<Message>>&)override { throw std::logic_error("sink has no internal event"); }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid sink elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time-clock_!=elapsed)
            throw std::invalid_argument("invalid sink timestamp");
        ReferenceSink candidate(*this);
        for(const auto& input:bag) {
            if(input.port!=0 || !std::holds_alternative<Token>(input.value)) throw std::invalid_argument("invalid sink input or port");
            const auto& token=std::get<Token>(input.value);
            if(token.entity.store!=store_ || token.entity.id>0xFFFFFFFFFFFFULL || !token.leases.empty() ||
                !candidate.seen_.insert(token.entity.id).second) throw std::invalid_argument("invalid, leased, or duplicate sink token");
            const double origin=origin_(token);
            if(!std::isfinite(origin) || origin<0 || origin>time) throw std::invalid_argument("invalid entity origin time");
            candidate.cycle_total_+=time-origin;
            if(!std::isfinite(candidate.cycle_total_)) throw std::overflow_error("sink cycle sum overflow");
            candidate.completed_.push_back({token,time});
        }
        candidate.clock_=time;
        *this=std::move(candidate);
    }
    std::size_t completed_count()const noexcept { return completed_.size(); }
    const std::vector<Completion>& completed()const noexcept { return completed_; }
    double total_cycle_time()const noexcept { return cycle_total_; }
    double mean_cycle_time()const {
        if(completed_.empty()) throw std::logic_error("mean cycle time requires completions");
        return cycle_total_/completed_.size();
    }
private:
    std::uint32_t store_;
    Origin origin_;
    double clock_=0,cycle_total_=0;
    std::set<std::uint64_t> seen_;
    std::vector<Completion> completed_;
};

struct CategoricalRouting {
    std::vector<double> probabilities;
    std::uint64_t seed=0;
    std::uint32_t scenario=0,replication=0,stream=0;
};

template<class Tag,class Message=ProcessMessage<Tag>>
class ReferenceSelect final:public detail::PublicationAtomic<ReferenceSelect<Tag,Message>,Message> {
    using Token=EntityToken<Tag>;
    friend class detail::PublicationAtomic<ReferenceSelect<Tag,Message>,Message>;
public:
    using Condition=std::function<bool(const Token&)>;
    ReferenceSelect(std::uint32_t store,std::vector<Condition> conditions)
        :store_(store),conditions_(std::move(conditions)),counts_(conditions_.size()+1) {
        if(conditions_.size()>65534 || std::any_of(conditions_.begin(),conditions_.end(),[](const auto& rule) { return !rule; }))
            throw std::invalid_argument("invalid condition routing rules");
    }
    ReferenceSelect(std::uint32_t store,CategoricalRouting routing)
        :store_(store),routing_(std::move(routing)),distribution_(routing_->probabilities),counts_(distribution_->size()) {
        if(counts_.size()>65535) throw std::invalid_argument("too many routing exits");
        (void)rng::pack_counter({routing_->scenario,routing_->replication,0,0,routing_->stream,0});
    }
    std::size_t received_count()const noexcept { return seen_.size(); }
    std::size_t routed_count(std::uint32_t port)const {
        if(port==0 || port>counts_.size()) throw std::out_of_range("unknown selector output port");
        return counts_[port-1];
    }
private:
    void accept(const std::vector<devs::Input<Message>>& bag) {
        for(const auto& input:bag) {
            if(input.port!=0 || !std::holds_alternative<Token>(input.value)) throw std::invalid_argument("invalid selector input or port");
            const auto& token=std::get<Token>(input.value);
            if(token.entity.store!=store_ || token.entity.id>0xFFFFFFFFFFFFULL || !seen_.insert(token.entity.id).second)
                throw std::invalid_argument("invalid or duplicate selector reference");
            std::size_t selected=0;
            if(routing_) selected=distribution_->sample(rng::draw(routing_->seed,
                {routing_->scenario,routing_->replication,token.entity.id,0,routing_->stream,0})[0]);
            else while(selected<conditions_.size() && !conditions_[selected](token)) ++selected;
            ++counts_[selected];
            this->pending_.push_back({static_cast<std::uint32_t>(selected+1),token});
        }
    }
    std::uint32_t store_;
    std::vector<Condition> conditions_;
    std::optional<CategoricalRouting> routing_;
    std::optional<rng::Categorical> distribution_;
    std::vector<std::size_t> counts_;
    std::set<std::uint64_t> seen_;
};

} // namespace ankurafathom::des
