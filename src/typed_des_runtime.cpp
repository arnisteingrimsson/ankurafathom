#include "typed_des.hpp"
#include "ankurafathom/des/reference_flow.hpp"
#include "ankurafathom/des/reference_queue.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include "ankurafathom/des/reference_resource.hpp"

namespace ankurafathom::ir {
namespace {
using Tag=ProcessEntityTag;
using Message=des::ResourceProcessMessage<Tag>;
using Token=des::EntityToken<Tag>;
using Source=des::ReferenceSource<Tag,Message>;
using Sink=des::ReferenceSink<Tag,Message>;
using Queue=des::ReferenceQueue<Tag,Message>;
using DelayBlock=des::ReferenceDelay<Tag,Message>;
using Seize=des::ReferenceSeize<Tag,Message>;
using Release=des::ReferenceRelease<Tag,Message>;
using Pool=des::TypedResourcePool<Message>;
using Select=des::ReferenceSelect<Tag,Message>;
using Node=std::variant<Source*,Sink*,Queue*,DelayBlock*,Seize*,Release*,Pool*,Select*>;

std::size_t capacity_value(const Expression& expression,const std::map<std::string,double>& parameters) {
    const double value=expression.evaluate(parameters);
    if(value<0 || value>1000000 || value!=std::trunc(value))
        throw std::invalid_argument("capacity must be an integer from zero to one million");
    return static_cast<std::size_t>(value);
}

double evaluate(const Expression& expression,const Token& token,const ProcessEntityStore& store,
                const std::map<std::string,double>& parameters) {
    auto values=parameters;
    for(const auto& name:expression.symbols()) {
        if(values.contains(name)) continue;
        const auto value=store.field(token.entity,name);
        if(const auto* real=std::get_if<double>(&value)) values.emplace(name,*real);
        else if(const auto* integer=std::get_if<std::int64_t>(&value)) {
            if(*integer < -9007199254740992LL || *integer > 9007199254740992LL)
                throw std::invalid_argument("integer expression input exceeds exact ±2^53 range: "+name);
            values.emplace(name,static_cast<double>(*integer));
        } else throw std::invalid_argument("nonnumeric process expression field: "+name);
    }
    return expression.evaluate(values);
}
bool compare(double left,const std::string& op,double right) {
    if(op=="lt") return left<right;
    if(op=="le") return left<=right;
    if(op=="eq") return left==right;
    if(op=="ne") return left!=right;
    if(op=="ge") return left>=right;
    if(op=="gt") return left>right;
    throw std::logic_error("invalid compiled comparison");
}
double observe(const Node& node,const Output& output,double time,const TypedProcess& process) {
    const auto& metric=output.metric;
    return std::visit([&](const auto* block)->double {
        using T=std::remove_cv_t<std::remove_pointer_t<decltype(block)>>;
        if constexpr(std::is_same_v<T,Source>) return static_cast<double>(block->emitted_count());
        else if constexpr(std::is_same_v<T,Sink>) {
            if(metric=="completed") return static_cast<double>(block->completed_count());
            if(metric=="cycle_total") return block->total_cycle_time();
            if(metric=="cycle_mean") return block->completed_count() ? block->mean_cycle_time() : 0;
        } else if constexpr(std::is_same_v<T,Queue>) {
            if(metric=="waiting") return static_cast<double>(block->waiting());
            if(metric=="accepted") return static_cast<double>(block->accepted_count());
            if(metric=="released") return static_cast<double>(block->released_count());
            if(metric=="rejected") return static_cast<double>(block->rejected_count());
            if(metric=="wait_total") return block->total_waiting_time();
            if(metric=="queue_mean") return time ? block->mean_queue_length(time) : 0;
        } else if constexpr(std::is_same_v<T,DelayBlock>) {
            if(metric=="active") return static_cast<double>(block->active_count());
            if(metric=="accepted") return static_cast<double>(block->accepted_count());
            if(metric=="completed") return static_cast<double>(block->completed_count());
            if(metric=="rejected") return static_cast<double>(block->rejected_count());
            if(metric=="in_process_mean") return time ? block->mean_in_process(time) : 0;
            if(metric=="utilization") return time ? block->utilization(time) : 0;
        } else if constexpr(std::is_same_v<T,Seize>) {
            if(metric=="received") return static_cast<double>(block->received_count());
            if(metric=="waiting") return static_cast<double>(block->waiting());
            if(metric=="granted") return static_cast<double>(block->granted_count());
        } else if constexpr(std::is_same_v<T,Release>) return static_cast<double>(block->released_count());
        else if constexpr(std::is_same_v<T,Pool>) {
            if(metric=="capacity") return static_cast<double>(block->pool().capacity());
            if(metric=="allocated") return static_cast<double>(block->pool().allocated_units());
            if(metric=="waiting") return static_cast<double>(block->pool().waiting());
            if(metric=="utilization") return time ? block->utilization(time) : 0;
            if(metric=="queue_mean") return time ? block->mean_waiting(time) : 0;
            if(metric=="allocated_mean") return time ? block->mean_allocated(time) : 0;
        } else if constexpr(std::is_same_v<T,Select>) {
            if(metric=="received") return static_cast<double>(block->received_count());
            if(metric=="routed") return static_cast<double>(block->routed_count(process.output_ports.at(output.id)));
        }
        throw std::logic_error("invalid compiled process metric");
    },node);
}
}

std::vector<Row> run_typed_des(const Model& model,const std::map<std::string,double>& parameters,
    std::uint64_t seed,std::uint32_t scenario,std::uint32_t replication) {
    const auto& process=*model.typed_process;
    std::map<std::string,ProcessEntityStore> stores;
    for(const auto& [id,type]:process.types) stores.emplace(id,ProcessEntityStore(type.store,type.fields));
    std::map<ProcessEntityStore::Reference,double> origins;
    std::map<std::string,std::vector<Source::Entry>> schedules;
    for(const auto& [id,node]:process.nodes) {
        if(node.kind!="source") continue;
        try {
            auto& store=stores.at(node.entity_type);
            std::vector<ProcessEntityStore::Record> records;
            double generated_time=0,interval_or_rate=0;
            if(node.generator) {
                generated_time=node.generator->start;
                interval_or_rate=node.generator->interval_or_rate.evaluate(parameters);
                if(interval_or_rate<=0) throw std::invalid_argument("interarrival interval or rate must be positive");
                if(node.stream) (void)rng::pack_counter({scenario,replication,store.next_id(),0,*node.stream,0});
                records.assign(node.generator->count,node.generator->values);
            } else for(const auto& entry:node.schedule) records.push_back(entry.values);
            const auto references=store.spawn_many(records);
            auto& schedule=schedules[id];
            for(std::size_t i=0;i<references.size();++i) {
                Token token{references[i],0};
                if(node.expression) {
                    const double priority=evaluate(*node.expression,token,store,parameters);
                    if(priority!=std::trunc(priority) || priority < -2147483648. || priority>2147483647.)
                        throw std::invalid_argument("source priority must be a signed 32-bit integer");
                    token.priority=static_cast<std::int32_t>(priority);
                }
                double arrival=0;
                if(node.generator) {
                    const double interval=node.stream ? rng::exponential(interval_or_rate,rng::draw(seed,
                        {scenario,replication,token.entity.id,0,*node.stream,0})[0]) : interval_or_rate;
                    arrival=generated_time+interval;
                    if(!std::isfinite(arrival) || arrival<=generated_time) throw std::overflow_error("generated arrival did not advance finite clock");
                    generated_time=arrival;
                } else arrival=node.schedule[i].time;
                origins.emplace(references[i],arrival);
                schedule.push_back({arrival,std::move(token)});
            }
        } catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME",node.pointer,error.what()); }
    }
    // All stores are read-only after this point. Atomics own every mutable process state.
    devs::Simulator<Message> simulator;
    std::map<std::string,std::size_t> ids;
    std::map<std::string,Node> nodes;
    for(const auto& [id,node]:process.nodes) {
        auto add=[&](auto block) {
            nodes.emplace(id,block.get()); ids.emplace(id,simulator.add(std::move(block)));
        };
        try {
            if(node.stream) (void)rng::pack_counter({scenario,replication,0,0,*node.stream,0});
            if(node.kind=="resource_pool") {
                add(std::make_unique<Pool>(capacity_value(*node.expression,parameters),node.max_request_units,node.discipline)); continue;
            }
            const auto& store=stores.at(node.entity_type);
            const auto store_id=store.store_id();
            const auto numeric=[&,expression=node.expression,stream=node.kind=="delay_block" ? node.stream : std::nullopt,
                                pointer=node.pointer](const Token& token) {
                try {
                    const double value=evaluate(*expression,token,store,parameters);
                    return stream ? rng::exponential(value,rng::draw(seed,{scenario,replication,token.entity.id,0,*stream,0})[0]) : value;
                }
                catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME",pointer,error.what()); }
            };
            Queue::Priority priority;
            if(node.priority) priority=[&,expression=*node.priority,pointer=node.pointer](const Token& token) {
                try {
                    const double value=evaluate(expression,token,store,parameters);
                    if(value!=std::trunc(value) || value < -2147483648. || value>2147483647.)
                        throw std::invalid_argument("priority must be a signed 32-bit integer");
                    return static_cast<std::int32_t>(value);
                } catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME",pointer+"/priority",error.what()); }
            };
            if(node.kind=="source") add(std::make_unique<Source>(store_id,std::move(schedules.at(id))));
            else if(node.kind=="sink") add(std::make_unique<Sink>(store_id,[&origins](const Token& token) { return origins.at(token.entity); }));
            else if(node.kind=="queue") add(std::make_unique<Queue>(store_id,node.capacity,node.discipline,priority));
            else if(node.kind=="delay_block") add(std::make_unique<DelayBlock>(store_id,numeric,node.capacity));
            else if(node.kind=="seize") add(std::make_unique<Seize>(store_id,node.pool_namespace,node.block,
                [numeric,maximum=process.nodes.at(node.pool).max_request_units,pointer=node.pointer](const Token& token) {
                    const double units=numeric(token);
                    if(units<1 || units>static_cast<double>(maximum) || units!=std::trunc(units))
                        throw Error("IR_TYPED_DES_RUNTIME",pointer+"/units","units must be a positive integer within the pool maximum");
                    return static_cast<std::size_t>(units);
                },false,priority));
            else if(node.kind=="release") add(std::make_unique<Release>(store_id,node.pool_namespace));
            else if(node.kind=="select_output") {
                if(node.stream) add(std::make_unique<Select>(store_id,des::CategoricalRouting{node.probabilities,seed,scenario,replication,*node.stream}));
                else {
                    std::vector<Select::Condition> conditions;
                    for(const auto& condition:node.conditions) conditions.emplace_back([&,condition,pointer=node.pointer](const Token& token) {
                        try { return compare(evaluate(condition.left,token,store,parameters),condition.op,
                                             evaluate(condition.right,token,store,parameters)); }
                        catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME",pointer,error.what()); }
                    });
                    add(std::make_unique<Select>(store_id,std::move(conditions)));
                }
            } else throw std::logic_error("unknown compiled process kind");
        } catch(const Error&) { throw; }
        catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME",node.pointer,error.what()); }
    }
    for(const auto& link:process.links) simulator.connect(ids.at(link.from),link.output_port,ids.at(link.to),0);
    for(const auto& [id,node]:process.nodes) {
        if(node.kind=="resource_pool") {
            for(const auto& [time,expression]:node.capacity_schedule) {
                try { simulator.inject(time,ids.at(id),2,des::SetCapacity{capacity_value(expression,parameters)}); }
                catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME",node.pointer+"/schedule",error.what()); }
            }
        } else if(node.kind=="queue") {
            const auto& link=*std::find_if(process.links.begin(),process.links.end(),[&](const auto& item) { return item.from==id && item.port=="out"; });
            simulator.connect(ids.at(link.to),3,ids.at(id),2);
        } else if(node.kind=="seize") {
            simulator.connect(ids.at(id),3,ids.at(node.pool),0);
            simulator.connect(ids.at(node.pool),static_cast<std::uint32_t>(node.block)+1,ids.at(id),2);
        } else if(node.kind=="release") simulator.connect(ids.at(id),3,ids.at(node.pool),0);
    }
    std::vector<Row> rows;
    const auto steps=static_cast<std::size_t>(std::llround(model.horizon/model.dt));
    rows.reserve((steps+1)*model.outputs.size());
    for(std::size_t step=0;step<=steps;++step) {
        const double time=step*model.dt;
        try { (void)simulator.run_until_transactional(time); }
        catch(const Error&) { throw; }
        catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME","/components",error.what()); }
        for(const auto& output:model.outputs) {
            try { rows.push_back({time,output.id,observe(nodes.at(output.source),output,time,process)}); }
            catch(const std::exception& error) { throw Error("IR_TYPED_DES_RUNTIME","/outputs",error.what()); }
        }
    }
    return rows;
}
} // namespace ankurafathom::ir
