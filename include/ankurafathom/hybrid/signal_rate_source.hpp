#pragma once
#include "ankurafathom/des/process_reference.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/hybrid/scalar_publication.hpp"
#include "ankurafathom/rng/philox.hpp"
#include <functional>

namespace ankurafathom::hybrid {

template<class Tag,class Message=std::variant<des::EntityToken<Tag>,ScalarPublication>>
class SignalRateSource final:public devs::Atomic<Message> {
public:
    using Store=des::RuntimeEntityStore<Tag>;
    using Reference=typename Store::Reference;
    using Record=typename Store::Record;
    using Token=des::EntityToken<Tag>;
    using Rate=std::function<double(const std::vector<double>&,double)>;
    using Factory=std::function<Record(Reference,const std::vector<double>&,double)>;
    struct Draws { std::uint64_t seed=0;std::uint32_t scenario=0,replication=0,stream=0; };
    static constexpr std::uint32_t input_port=0,output_port=1;
    SignalRateSource(Store records,std::vector<double> initial,Rate rate,Factory factory,Draws draws={},std::size_t limit=1000000)
        :records_(std::move(records)),signals_(std::move(initial)),rate_fn_(std::move(rate)),factory_(std::move(factory)),draws_(draws),limit_(limit) {
        if(records_.size()!=0 || signals_.empty() || !rate_fn_ || !factory_ || limit_>1000000 ||
           (limit_ && limit_-1>Store::max_entity_id-records_.first_id()))
            throw std::invalid_argument("invalid signal rate source configuration");
        (void)rng::pack_counter({draws_.scenario,draws_.replication,records_.first_id(),0,draws_.stream,0});
        for(double x:signals_) if(!std::isfinite(x)) throw std::invalid_argument("nonfinite source input");
        rate_=rate_fn_(signals_,0);validate_rate(rate_);
        if(limit_) remaining_=hazard(records_.next_id());
        schedule();
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<SignalRateSource>(*this); }
    double time_advance()const override { return deadline_-clock_; }
    std::optional<double> next_event_time()const override { return deadline_; }
    std::vector<devs::PortValue<Message>> output()const override {
        if(!std::isfinite(deadline_)) return {};
        const auto token=arrival();
        return {{output_port,Message{token}}};
    }
    void internal_transition()override {
        auto candidate=*this;candidate.arrive();*this=std::move(candidate);
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(bag.size()!=1 || !std::holds_alternative<ScalarPublication>(bag[0].value))
            throw std::invalid_argument("source requires one scalar publication");
        external_transition_at(std::get<ScalarPublication>(bag[0].value).time,elapsed,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time<clock_ || time-clock_!=elapsed || time>=deadline_)
            throw std::invalid_argument("invalid source update timestamp or missed arrival");
        auto candidate=*this;candidate.accept(time,bag);*this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        auto candidate=*this;candidate.arrive(false);candidate.accept(candidate.clock_,bag);*this=std::move(candidate);
    }
    const Store& store()const noexcept { return records_; }
    const std::vector<double>& signals()const noexcept { return signals_; }
    double time()const noexcept { return clock_; }
    double rate()const noexcept { return rate_; }
    double remaining_hazard()const noexcept { return remaining_; }
    std::size_t generated_count()const noexcept { return records_.size(); }
    std::optional<std::uint64_t> revision()const noexcept { return revision_; }
private:
    static void validate_rate(double rate) {
        if(!std::isfinite(rate) || rate<0) throw std::invalid_argument("source rate must be finite and nonnegative");
    }
    double hazard(std::uint64_t id)const {
        return rng::exponential(1,rng::draw(draws_.seed,{draws_.scenario,draws_.replication,id,0,draws_.stream,0})[0]);
    }
    Token arrival()const {
        const Reference ref{records_.store_id(),records_.next_id()};
        auto record=factory_(ref,signals_,deadline_);
        auto validation=records_;(void)validation.spawn(record);
        return Token{ref,0,{},des::EntitySnapshot<Tag>{deadline_,records_.schema(),std::move(record)}};
    }
    void schedule() {
        deadline_=std::numeric_limits<double>::infinity();
        if(records_.size()==limit_ || rate_==0) return;
        const double delay=remaining_/rate_;
        deadline_=clock_+delay;
        if(!std::isfinite(delay) || delay<=0 || !std::isfinite(deadline_) || deadline_<=clock_)
            throw std::overflow_error("source arrival deadline is not representable");
    }
    void arrive(bool reschedule=true) {
        if(!std::isfinite(deadline_)) throw std::logic_error("rate source has no arrival");
        auto token=arrival();
        const auto ref=records_.spawn(token.snapshot->record);
        if(ref!=token.entity) throw std::logic_error("source allocation disagrees with published identity");
        clock_=deadline_;
        remaining_=records_.size()<limit_?hazard(records_.next_id()):0;
        if(reschedule) schedule();
    }
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.size()!=1 || bag[0].port!=input_port) throw std::invalid_argument("source requires one input batch");
        const auto* p=std::get_if<ScalarPublication>(&bag[0].value);
        if(!p || p->time!=time || p->values.size()!=signals_.size() || (owner_ && *owner_!=bag[0].source) ||
           (revision_ && p->revision<=*revision_)) throw std::invalid_argument("invalid source scalar width, timestamp, owner or revision");
        for(double x:p->values) if(!std::isfinite(x)) throw std::invalid_argument("nonfinite source scalar");
        const auto rate=rate_fn_(p->values,time);validate_rate(rate);
        if(records_.size()<limit_ && rate_>0 && time>clock_) {
            const auto consumed=rate_*(time-clock_);
            remaining_-=consumed;
            if(!std::isfinite(remaining_) || remaining_<=0)
                throw std::overflow_error("source residual hazard is not representable before deadline");
        }
        const bool unchanged=rate==rate_;
        clock_=time;signals_=p->values;rate_=rate;owner_=bag[0].source;revision_=p->revision;
        if(!unchanged || deadline_<=clock_) schedule();
    }
    Store records_;
    std::vector<double> signals_;
    Rate rate_fn_;
    Factory factory_;
    Draws draws_;
    std::size_t limit_;
    std::optional<std::size_t> owner_;
    std::optional<std::uint64_t> revision_;
    double clock_=0,rate_=0,remaining_=0,deadline_=std::numeric_limits<double>::infinity();
};
} // namespace ankurafathom::hybrid
