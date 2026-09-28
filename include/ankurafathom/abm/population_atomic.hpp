#pragma once
#include "ankurafathom/abm/async_population.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/abm/topic_broker.hpp"

namespace ankurafathom::abm {
template<class Tag> struct PopulationCommand {
    double time;
    std::uint64_t sequence;
    typename AsyncPopulation<Tag>::Reference agent;
    std::string kind;
};
template<class Tag> struct PopulationTopicInput {
    double time;
    std::string topic;
    typename AsyncPopulation<Tag>::Reference sender;
    std::optional<typename AsyncPopulation<Tag>::Reference> receiver;
    std::uint64_t sequence;
    double value;
};
struct PopulationLifecycleTarget {
    std::uint32_t store;
    std::vector<des::EntityField> schema;
};
template<class Tag> struct PopulationLifecycleInput {
    double time;
    std::uint64_t sequence;
    std::vector<typename AsyncPopulation<Tag>::Reference> retirements;
    std::vector<typename AsyncPopulation<Tag>::Store::Record> births;
    std::optional<PopulationLifecycleTarget> target={};
};
template<class Tag> struct PopulationNetworkInput {
    double time;
    std::uint64_t sequence;
    std::vector<typename AsyncPopulation<Tag>::Store::Edge> additions,removals;
};
template<class Tag> struct PopulationDelivery {
    double time;
    std::size_t round;
    std::string topic;
    typename AsyncPopulation<Tag>::Reference sender,receiver;
    std::uint64_t sequence;
    double value;
    bool operator==(const PopulationDelivery&)const=default;
};
template<class Tag> struct PopulationResult {
    double time;
    typename AsyncPopulation<Tag>::Store snapshot;
    std::vector<typename AsyncPopulation<Tag>::Timer> timers;
    std::vector<PopulationCommand<Tag>> commands;
    std::vector<PopulationDelivery<Tag>> deliveries;
};
template<class Tag> using PopulationMessage=std::variant<PopulationCommand<Tag>,PopulationResult<Tag>,PopulationTopicInput<Tag>,PopulationLifecycleInput<Tag>,PopulationNetworkInput<Tag>>;

// Contract: docs/SEMANTICS.md, "Typed population DEVS wrapper and declarative ABM".
template<class Tag,class Payload=PopulationMessage<Tag>>
class PopulationAtomic final:public devs::Atomic<Payload> {
public:
    using Async=AsyncPopulation<Tag>; using Sync=TypedPopulation<Tag>;
    using Store=typename Async::Store; using Effects=typename Async::Effects;
    using Command=PopulationCommand<Tag>; using Result=PopulationResult<Tag>; using Message=Payload;
    using NetworkInput=PopulationNetworkInput<Tag>;
    using LifecycleInput=PopulationLifecycleInput<Tag>;
    using BirthInitializer=std::function<typename Async::Birth(typename Store::Reference,const Store&,double)>;
    using TopicInput=PopulationTopicInput<Tag>; using Delivery=PopulationDelivery<Tag>;
    using Reference=typename Store::Reference;
    using Emission=PopulationEmission<Tag>;
    struct TopicEffects { Effects effects; std::vector<Emission> publications; };
    struct TopicConfig {
        std::map<std::string,std::size_t> capacities;
        std::function<TopicEffects(const Delivery&,const Store&)> handler;
        std::size_t budget=100000;
    };
    using InputRule=std::function<Effects(const Command&,const Store&)>;
    explicit PopulationAtomic(Async population,InputRule input={})
        :runtime_(std::move(population)),input_(std::move(input)),clock_(async().now()) { require_empty_outbox(); }
    PopulationAtomic(Sync population,double dt,InputRule input={})
        :runtime_(std::move(population)),input_(std::move(input)),dt_(dt),next_tick_(dt) {
        require_empty_outbox();
        if(!std::isfinite(dt) || dt<=0) throw std::invalid_argument("invalid synchronous tick interval");
    }
    void configure_births(BirthInitializer initializer) {
        if(started_ || clock_!=0 || pending_) throw std::logic_error("births must be configured before execution");
        birth_initializer_=std::move(initializer);
    }
    void configure_topics(TopicConfig config) {
        if(started_ || clock_!=0 || pending_) throw std::logic_error("topics must be configured before execution");
        if(config.budget==0 || (!config.capacities.empty() && !config.handler)) throw std::invalid_argument("topics need a handler and positive budget");
        TopicBroker<double> broker;
        for(const auto& [name,capacity]:config.capacities) broker.declare_topic(name,capacity);
        topic_config_=std::move(config); broker_=std::move(broker);
    }
    std::uint64_t delivered_count()const noexcept { return delivered_; }
    std::uint64_t next_message_sequence(std::uint64_t agent)const {
        const auto found=sequences_.find(agent);return found==sequences_.end() ? 0 : found->second;
    }
    const Async& async()const { return std::get<Async>(runtime_); }
    const Sync& sync()const { return std::get<Sync>(runtime_); }
    const Store& store()const { return std::visit([](const auto& p)->const Store& { return p.store(); },runtime_); }
    double now()const noexcept { return clock_; }
    std::unique_ptr<devs::Atomic<Payload>> clone()const override { return std::make_unique<PopulationAtomic>(*this); }
    double time_advance()const override { return *next_event_time()-clock_; }
    std::optional<double> next_event_time()const override { return pending_ ? clock_ : deadline(); }
    std::vector<devs::PortValue<Payload>> output()const override {
        if(!pending_) return {};
        return {{1,Payload{*pending_}}};
    }
    void internal_transition()override {
        if(pending_) { pending_.reset(); return; }
        const auto time=deadline();
        if(!std::isfinite(time)) throw std::logic_error("population is not imminent");
        commit(time,{});
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Payload>>& bag)override {
        if(bag.empty()) throw std::invalid_argument("empty population input bag");
        const auto* command=std::get_if<Command>(&bag.front().value);
        const auto* publication=topic_input(bag.front().value);
        const auto* lifecycle=lifecycle_input(bag.front().value);
        const auto* network=network_input(bag.front().value);
        if(!command && !publication && !lifecycle && !network) throw std::invalid_argument("invalid population input payload");
        external_transition_at(command ? command->time : publication ? publication->time : lifecycle ? lifecycle->time : network->time,elapsed,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Payload>>& bag)override {
        if(pending_ || bag.empty() || !std::isfinite(elapsed) || elapsed<0 || time-clock_!=elapsed)
            throw std::invalid_argument("invalid population input elapsed time or pending publication");
        commit(time,bag);
    }
    void confluent_transition(const std::vector<devs::Input<Payload>>& bag)override {
        if(bag.empty()) throw std::invalid_argument("empty population confluent bag");
        commit(pending_ ? clock_ : deadline(),bag);
    }
private:
    void require_empty_outbox()const {
        if(std::visit([](const auto& p) { return !p.pending_publications().empty(); },runtime_))
            throw std::invalid_argument("population has undrained publications at insertion");
    }
    static const TopicInput* topic_input(const Payload& value) {
        return std::visit([](const auto& item)->const TopicInput* {
            if constexpr(std::is_same_v<std::decay_t<decltype(item)>,TopicInput>) return &item;
            else return nullptr;
        },value);
    }
    static const LifecycleInput* lifecycle_input(const Payload& value) {
        return std::visit([](const auto& item)->const LifecycleInput* {
            if constexpr(std::is_same_v<std::decay_t<decltype(item)>,LifecycleInput>) return &item;
            else return nullptr;
        },value);
    }
    static const NetworkInput* network_input(const Payload& value) {
        return std::visit([](const auto& item)->const NetworkInput* {
            if constexpr(std::is_same_v<std::decay_t<decltype(item)>,NetworkInput>) return &item;
            else return nullptr;
        },value);
    }
    double deadline()const { return std::holds_alternative<Async>(runtime_) ? async().next_time() : next_tick_; }
    static void sync_apply(Sync& population,const Effects& effects) {
        if(!effects.cancellations.empty() || !effects.schedules.empty()) throw std::invalid_argument("sync population cannot own timers");
        std::vector<typename Sync::Record> births;
        for(const auto& birth:effects.births) {
            if(!birth.timers.empty()) throw std::invalid_argument("sync birth cannot arm timers");
            births.push_back(birth.value);
        }
        (void)population.apply(effects.updates,effects.retirements,births,effects.publications);
    }
    void commit(double time,const std::vector<devs::Input<Payload>>& bag) {
        if(!std::isfinite(time) || time<clock_ || deadline()<time) throw std::invalid_argument("invalid or skipped population deadline");
        std::vector<Command> commands;
        std::vector<LifecycleInput> lifecycle;
        std::vector<NetworkInput> network;
        std::vector<TopicInput> publications;
        auto lifecycle_keys=lifecycle_keys_;
        for(const auto& input:bag) {
            const auto* c=std::get_if<Command>(&input.value);
            if(const auto* publication=topic_input(input.value)) {
                if(input.port!=2 || publication->time!=time) throw std::invalid_argument("invalid topic input port/time");
                publications.push_back(*publication);
            } else if(const auto* change=lifecycle_input(input.value)) {
                if(input.port!=3 || change->time!=time) throw std::invalid_argument("invalid lifecycle input port/time");
                if(change->target && (change->target->store!=store().store_id() || change->target->schema!=store().schema() ||
                   !lifecycle_keys.insert(change->sequence).second))
                    throw std::invalid_argument("invalid lifecycle target or replayed bridge sequence");
                lifecycle.push_back(*change);
            } else if(const auto* edit=network_input(input.value)) {
                if(input.port!=4 || edit->time!=time) throw std::invalid_argument("invalid network input port/time");
                network.push_back(*edit);
            } else {
                if(input.port!=0 || !c || c->time!=time || c->kind.empty()) throw std::invalid_argument("invalid population command");
                commands.push_back(*c);
            }
        }
        std::sort(commands.begin(),commands.end(),[](const auto& a,const auto& b) { return std::tie(a.agent.id,a.sequence)<std::tie(b.agent.id,b.sequence); });
        for(std::size_t i=1;i<commands.size();++i)
            if(commands[i].agent.id==commands[i-1].agent.id && commands[i].sequence==commands[i-1].sequence)
                throw std::invalid_argument("duplicate population command sequence");
        std::sort(lifecycle.begin(),lifecycle.end(),[](const auto& a,const auto& b) { return a.sequence<b.sequence; });
        for(std::size_t i=1;i<lifecycle.size();++i)
            if(lifecycle[i].sequence==lifecycle[i-1].sequence) throw std::invalid_argument("duplicate lifecycle sequence");
        std::sort(network.begin(),network.end(),[](const auto& a,const auto& b) { return a.sequence<b.sequence; });
        for(std::size_t i=1;i<network.size();++i)
            if(network[i].sequence==network[i-1].sequence) throw std::invalid_argument("duplicate network sequence");
        auto candidate=runtime_;
        auto broker=broker_; auto sequences=sequences_; auto delivered=delivered_;
        std::vector<Delivery> deliveries;
        auto tick=tick_index_; auto next=next_tick_;
        std::vector<typename Async::Timer> timers;
        auto* asynchronous=std::get_if<Async>(&candidate);
        const auto snapshot=[&]()->const Store& { return std::visit([](const auto& p)->const Store& { return p.store(); },candidate); };
        const auto take_outbox=[&] { return std::visit([](auto& p) { return p.take_publications(); },candidate); };
        const auto apply=[&](const Effects& effects) {
            if(asynchronous) asynchronous->apply(effects); else sync_apply(std::get<Sync>(candidate),effects);
        };
        const auto drain_timers=[&] {
            if(asynchronous && asynchronous->next_time()==time) {
                auto more=asynchronous->step(); timers.insert(timers.end(),more.begin(),more.end());
                if(timers.size()>asynchronous->timestamp_budget()) throw std::runtime_error("combined population timestamp budget exceeded");
            }
        };
        drain_timers();
        if(asynchronous) asynchronous->run_until(time);
        if(!lifecycle.empty()) {
            Effects effects;
            auto staged=snapshot();
            for(const auto& change:lifecycle) for(auto ref:change.retirements) {
                staged.retire(ref); effects.retirements.push_back(ref);
            }
            std::vector<Reference> born;
            for(const auto& change:lifecycle) for(const auto& record:change.births) born.push_back(staged.spawn(record));
            for(auto ref:born) effects.births.push_back(birth_initializer_ ? birth_initializer_(ref,staged,time) : typename Async::Birth{staged.record(ref),{}});
            apply(effects);
        }
        if(!network.empty()) {
            std::vector<typename Store::Edge> additions,removals;
            for(const auto& edit:network) {
                additions.insert(additions.end(),edit.additions.begin(),edit.additions.end());
                removals.insert(removals.end(),edit.removals.begin(),edit.removals.end());
            }
            std::visit([&](auto& p) { p.edit_network(additions,removals); },candidate);
        }
        for(const auto& command:commands) {
            if(!input_) throw std::invalid_argument("population has no input rule");
            if(!snapshot().alive(command.agent)) throw std::invalid_argument("input target is inactive");
            apply(input_(command,snapshot()));
        }
        const auto generated=take_outbox();
        const auto make_publication=[&](const TopicInput& input) {
            const auto& s=snapshot();
            if(!std::isfinite(input.value) || !s.alive(input.sender) || (input.receiver && !s.alive(*input.receiver)))
                throw std::invalid_argument("topic publication needs finite payload and live endpoints");
            return typename TopicBroker<double>::Publication{input.topic,{input.sender.id,input.receiver ? std::optional<std::uint64_t>(input.receiver->id) : std::nullopt,input.sequence,input.value}};
        };
        const auto enqueue=[&](const std::vector<PopulationPublication<Tag>>& batch) {
            std::vector<typename TopicBroker<double>::Publication> entries;
            for(const auto& entry:batch) {
                auto& sequence=sequences[entry.sender.id];
                if(sequence==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("topic sequence exhausted");
                entries.push_back(make_publication({time,entry.topic,entry.sender,entry.receiver,sequence++,entry.value}));
            }
            broker.publish_many(entries);
        };
        std::size_t round=0;
        const auto drain_topics=[&] {
            while(true) {
                broker.flush();
                bool any=false;
                for(const auto& [topic,capacity]:topic_config_.capacities) {
                    (void)capacity;
                    for(const auto& message:broker.visible(topic)) {
                        any=true;
                        const auto store_id=snapshot().store_id();
                        std::vector<Reference> receivers;
                        if(message.receiver) receivers.push_back({store_id,*message.receiver});
                        else for(auto id=snapshot().first_id();id<snapshot().next_id();++id)
                            if(snapshot().alive({store_id,id})) receivers.push_back({store_id,id});
                        for(auto receiver:receivers) {
                            if(deliveries.size()>=topic_config_.budget) throw std::runtime_error("topic delivery budget exceeded");
                            Delivery d{time,round,topic,{store_id,message.sender},receiver,message.sequence,message.payload};
                            auto result=topic_config_.handler(d,snapshot());
                            if(!result.effects.births.empty() || !result.effects.retirements.empty()) throw std::invalid_argument("topic lifecycle effects unsupported");
                            apply(result.effects);
                            auto replies=take_outbox();
                            for(const auto& reply:result.publications) replies.push_back({reply.topic,receiver,reply.receiver,reply.value});
                            enqueue(replies);
                            if(delivered==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("topic delivery count exhausted");
                            ++delivered; deliveries.push_back(std::move(d));
                        }
                    }
                }
                if(!any) break;
                ++round;
            }
        };
        std::vector<typename TopicBroker<double>::Publication> first;
        for(const auto& input:publications) first.push_back(make_publication(input));
        broker.publish_many(first); drain_topics();
        enqueue(generated); drain_topics();
        while(asynchronous && asynchronous->next_time()==time) {
            drain_timers(); enqueue(take_outbox()); drain_topics();
        }
        if(!asynchronous && next_tick_==time) {
            std::get<Sync>(candidate).step();
            enqueue(take_outbox()); drain_topics();
            if(tick==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("sync tick exhausted");
            next=static_cast<double>(++tick)*dt_;
            if(!std::isfinite(next) || next<=time) throw std::overflow_error("sync tick cannot advance");
        }
        std::optional<Result> publication=Result{time,std::visit([](const auto& p) { return p.store(); },candidate),std::move(timers),std::move(commands),std::move(deliveries)};
        broker_=std::move(broker); sequences_=std::move(sequences); delivered_=delivered; started_=true;
        runtime_=std::move(candidate); pending_=std::move(publication); clock_=time; tick_index_=tick; next_tick_=next;
        lifecycle_keys_=std::move(lifecycle_keys);
    }
    TopicConfig topic_config_;
    TopicBroker<double> broker_;
    std::map<std::uint64_t,std::uint64_t> sequences_;
    std::uint64_t delivered_=0;
    std::set<std::uint64_t> lifecycle_keys_;
    bool started_=false;
    std::variant<Sync,Async> runtime_;
    InputRule input_;
    BirthInitializer birth_initializer_;
    double clock_=0,dt_=0,next_tick_=0;
    std::uint64_t tick_index_=1;
    std::optional<Result> pending_;
};
} // namespace ankurafathom::abm
