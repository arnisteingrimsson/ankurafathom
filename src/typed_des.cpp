#include "typed_des.hpp"
#include "ankurafathom/rng/categorical.hpp"
#include <algorithm>
#include <functional>

namespace ankurafathom::ir {
namespace {
using Json=nlohmann::json;
std::string escape(const std::string& name) {
    std::string result;
    for(char c:name) result+=c=='~' ? "~0" : c=='/' ? "~1" : std::string(1,c);
    return result;
}
[[noreturn]] void fail(const std::string& pointer,const std::string& message,const std::string& code="IR_TYPED_DES") {
    throw Error(code,pointer,message);
}
void keys(const Json& value,std::initializer_list<const char*> allowed,const std::string& pointer) {
    if(!value.is_object()) fail(pointer,"expected an object","IR_TYPE");
    for(auto it=value.begin();it!=value.end();++it)
        if(std::none_of(allowed.begin(),allowed.end(),[&](const char* key) { return it.key()==key; }))
            fail(pointer+"/"+escape(it.key()),"unknown field","IR_FIELD");
}
const Json& required(const Json& value,const char* key,const std::string& pointer) {
    if(!value.is_object() || !value.contains(key)) fail(pointer+"/"+key,"required field is missing","IR_MISSING");
    return value.at(key);
}
std::string text(const Json& value,const char* key,const std::string& pointer) {
    const auto& item=required(value,key,pointer);
    if(!item.is_string() || item.get<std::string>().empty()) fail(pointer+"/"+key,"expected a nonempty string","IR_TYPE");
    return item.get<std::string>();
}
std::string identifier(const Json& value,const char* key,const std::string& pointer) {
    const auto name=text(value,key,pointer);
    if(std::isdigit(static_cast<unsigned char>(name.front())) || name=="t" || name=="dt" || Expression::builtin(name) ||
        std::any_of(name.begin(),name.end(),[](unsigned char c) { return !std::isalnum(c) && c!='_'; }))
        fail(pointer+"/"+key,"invalid or reserved identifier","IR_ID");
    return name;
}
std::uint64_t integer(const Json& value,const char* key,const std::string& pointer,std::uint64_t limit=1000000) {
    const auto& item=required(value,key,pointer);
    if(!item.is_number_unsigned() || item.get<std::uint64_t>()>limit)
        fail(pointer+"/"+key,"expected a bounded nonnegative integer","IR_TYPE");
    return item.get<std::uint64_t>();
}
double number(const Json& value,const char* key,const std::string& pointer) {
    const auto& item=required(value,key,pointer);
    if(!item.is_number() || !std::isfinite(item.get<double>())) fail(pointer+"/"+key,"expected a finite number","IR_TYPE");
    return item.get<double>();
}
const Json& array(const Json& value,const char* key,const std::string& pointer) {
    const auto& items=required(value,key,pointer);
    if(!items.is_array()) fail(pointer+"/"+key,"expected an array","IR_TYPE");
    return items;
}
Dimension dimension(const Json& value,const char* key,const std::string& pointer) {
    try { return Dimension::parse(text(value,key,pointer)); }
    catch(const std::invalid_argument& error) { fail(pointer+"/"+key,error.what(),"IR_UNIT"); }
}
Expression expression(const Json& value,const char* key,const std::string& pointer,
                      const std::map<std::string,Dimension>& symbols,std::optional<Dimension> expected=std::nullopt) {
    try {
        Expression result(text(value,key,pointer));
        for(const auto& symbol:result.symbols())
            if(!symbols.contains(symbol)) fail(pointer+"/"+key,"unknown or nonnumeric symbol: "+symbol,"IR_SYMBOL");
        for(const auto& function:result.functions())
            if(function!="NONNEGATIVE") fail(pointer+"/"+key,"unsupported process function: "+function,"IR_FUNCTION");
        const auto unit=result.infer_unit(symbols,{});
        if(expected && unit!=*expected) fail(pointer+"/"+key,"expression has incompatible dimension","IR_UNIT");
        return result;
    } catch(const std::invalid_argument& error) { fail(pointer+"/"+key,error.what(),"IR_EXPRESSION"); }
}
des::QueueDiscipline discipline(const Json& value,const std::string& pointer) {
    const auto name=value.contains("discipline") ? text(value,"discipline",pointer) : "fifo";
    if(name=="fifo") return des::QueueDiscipline::fifo;
    if(name=="lifo") return des::QueueDiscipline::lifo;
    if(name=="priority") return des::QueueDiscipline::priority;
    fail(pointer+"/discipline","expected fifo, lifo, or priority");
}
ProcessEntityStore::Record record(const Json& values,const TypedEntityType& type,const std::string& pointer) {
    if(!values.is_object() || values.size()!=type.fields.size()) fail(pointer,"record must contain exactly the declared fields");
    ProcessEntityStore::Record result;
    for(const auto& field:type.fields) {
        const auto& value=required(values,field.name.c_str(),pointer);
        const auto field_pointer=pointer+"/"+escape(field.name);
        switch(field.kind) {
            case des::FieldKind::real:
                if(!value.is_number() || !std::isfinite(value.get<double>())) fail(field_pointer,"expected a finite real value");
                result.emplace_back(value.get<double>()); break;
            case des::FieldKind::integer:
                if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>9223372036854775807ULL))
                    fail(field_pointer,"expected a signed 64-bit integer");
                result.emplace_back(value.get<std::int64_t>()); break;
            case des::FieldKind::boolean:
                if(!value.is_boolean()) fail(field_pointer,"expected a boolean");
                result.emplace_back(value.get<bool>()); break;
            case des::FieldKind::string:
                if(!value.is_string()) fail(field_pointer,"expected a string");
                result.emplace_back(value.get<std::string>()); break;
        }
    }
    return result;
}

void validate_graph(TypedProcess& process) {
    std::map<std::string,std::vector<const TypedProcess::Link*>> incoming,outgoing;
    std::map<std::string,std::size_t> degree;
    std::set<std::string> ready;
    for(const auto& [id,node]:process.nodes) if(node.kind!="resource_pool") degree[id]=0;
    for(const auto& link:process.links) {
        incoming[link.to].push_back(&link); outgoing[link.from].push_back(&link); ++degree.at(link.to);
    }
    for(const auto& [id,node]:process.nodes) {
        if(node.kind=="resource_pool") continue;
        if(node.kind=="source") {
            if(degree[id]!=0) fail(node.pointer,"source cannot receive entity links");
            ready.insert(id);
        } else if(degree[id]==0) fail(node.pointer,"process block is unreachable from a source");
        std::set<std::string> exits;
        if(node.kind=="select_output") exits.insert(node.ports.begin(),node.ports.end());
        else if(node.kind!="sink") exits.insert("out");
        const bool controlled=node.kind=="delay_block" && incoming[id].size()==1 &&
            process.nodes.at(incoming[id][0]->from).kind=="queue" && incoming[id][0]->port=="out";
        if((node.kind=="queue" && node.capacity) || (node.kind=="delay_block" && node.capacity && !controlled)) exits.insert("rejected");
        std::set<std::string> connected;
        for(const auto* link:outgoing[id]) {
            if(!connected.insert(link->port).second) fail(node.pointer,"entity output cannot fan out");
            if(!exits.contains(link->port)) fail(node.pointer,"unknown or unnecessary entity output: "+link->port);
        }
        if(connected!=exits) fail(node.pointer,"every entity exit needs exactly one link");
        if(node.kind=="queue") {
            const auto* link=*std::find_if(outgoing[id].begin(),outgoing[id].end(),[](const auto* item) { return item->port=="out"; });
            const auto& target=process.nodes.at(link->to);
            if(target.kind!="delay_block" || !target.capacity || incoming[link->to].size()!=1)
                fail(node.pointer,"queue must be the sole input of a bounded delay");
        }
    }
    std::map<std::string,std::set<std::string>> held_after;
    std::size_t visited=0;
    while(!ready.empty()) {
        const auto id=*ready.begin(); ready.erase(ready.begin()); ++visited;
        const auto& node=process.nodes.at(id);
        std::set<std::string> held;
        bool first=true;
        for(const auto* link:incoming[id]) {
            if(first) { held=held_after.at(link->from); first=false; }
            else if(held!=held_after.at(link->from)) fail(node.pointer,"merged paths have inconsistent held resources");
        }
        if(node.kind=="seize") {
            if(!held.empty() && *held.rbegin()>=node.pool) fail(node.pointer,"nested resource acquisition must follow increasing pool IDs");
            held.insert(node.pool);
        } else if(node.kind=="release") {
            if(!held.erase(node.pool)) fail(node.pointer,"release has no matching held resource on every incoming path");
        } else if(node.kind=="sink" && !held.empty()) fail(node.pointer,"sink path retains a resource lease");
        held_after[id]=std::move(held);
        for(const auto* link:outgoing[id]) if(--degree.at(link->to)==0) ready.insert(link->to);
    }
    if(visited!=degree.size()) fail("/links","entity flow contains a cycle or unreachable cycle");
}
}

void parse_typed_des(const Json& document,Model& model,std::set<std::string>& ids) {
    TypedProcess process;
    const auto& components=array(document,"components","");
    if(components.size()>10000) fail("/components","at most 10,000 components are supported");
    std::map<std::string,std::size_t> positions;
    for(std::size_t i=0;i<components.size();++i) {
        const auto pointer="/components/"+std::to_string(i);
        const auto id=identifier(components[i],"id",pointer);
        if(!ids.insert(id).second) fail(pointer+"/id","duplicate identifier","IR_ID");
        positions.emplace(id,i);
    }
    for(const auto& [id,index]:positions) {
        const auto pointer="/components/"+std::to_string(index);
        const auto& item=components[index];
        if(text(item,"kind",pointer)!="entity_type") continue;
        keys(item,{"id","kind","fields"},pointer);
        TypedEntityType type;
        type.store=static_cast<std::uint32_t>(process.types.size());
        const auto& fields=array(item,"fields",pointer);
        std::set<std::string> names;
        for(std::size_t j=0;j<fields.size();++j) {
            const auto field_pointer=pointer+"/fields/"+std::to_string(j);
            const auto name=identifier(fields[j],"id",field_pointer);
            if(!names.insert(name).second || model.parameters.contains(name)) fail(field_pointer+"/id","duplicate field or parameter shadowing","IR_ID");
            const auto kind=text(fields[j],"type",field_pointer);
            des::FieldKind storage;
            if(kind=="real" || kind=="integer") {
                keys(fields[j],{"id","type","unit"},field_pointer);
                type.units.emplace(name,dimension(fields[j],"unit",field_pointer));
                storage=kind=="real" ? des::FieldKind::real : des::FieldKind::integer;
            } else {
                keys(fields[j],{"id","type"},field_pointer);
                if(kind=="boolean") storage=des::FieldKind::boolean;
                else if(kind=="string") storage=des::FieldKind::string;
                else fail(field_pointer+"/type","unknown entity field type");
            }
            type.fields.push_back({name,storage});
        }
        process.types.emplace(id,std::move(type));
    }
    std::set<std::uint32_t> streams;
    const auto read_stream=[&](const Json& item,const std::string& pointer) {
        const auto stream=static_cast<std::uint32_t>(integer(item,"stream",pointer,65535));
        if(!streams.insert(stream).second) fail(pointer+"/stream","draw stream is already owned by another process block");
        return stream;
    };
    const auto capacity_expression=[&](const Json& item,const char* key,const std::string& pointer) {
        if(required(item,key,pointer).is_string()) return expression(item,key,pointer,model.parameter_units,Dimension{});
        return Expression(std::to_string(integer(item,key,pointer)));
    };
    std::uint32_t pool_namespace=0;
    std::size_t sources=0,sinks=0;
    for(const auto& [id,index]:positions) {
        const auto pointer="/components/"+std::to_string(index);
        const auto& item=components[index];
        const auto kind=text(item,"kind",pointer);
        if(kind=="entity_type") continue;
        TypedProcessNode node;
        node.kind=kind; node.pointer=pointer;
        if(kind=="resource_pool") {
            keys(item,{"id","kind","capacity","max_request_units","discipline","schedule"},pointer);
            node.expression=capacity_expression(item,"capacity",pointer);
            node.max_request_units=integer(item,"max_request_units",pointer);
            if(node.max_request_units==0) fail(pointer+"/max_request_units","maximum request units must be positive");
            node.pool_namespace=pool_namespace++;
            node.discipline=discipline(item,pointer);
            if(item.contains("schedule")) {
                const auto& entries=array(item,"schedule",pointer);
                double previous=-1;
                for(std::size_t j=0;j<entries.size();++j) {
                    const auto entry_pointer=pointer+"/schedule/"+std::to_string(j);
                    keys(entries[j],{"time","capacity"},entry_pointer);
                    const double time=number(entries[j],"time",entry_pointer);
                    if(time<0 || time<=previous || time>model.horizon) fail(entry_pointer+"/time","capacity changes must be strictly ordered inside the horizon");
                    previous=time;
                    node.capacity_schedule.emplace_back(time,capacity_expression(entries[j],"capacity",entry_pointer));
                }
            }
            process.nodes.emplace(id,std::move(node)); continue;
        }
        node.entity_type=text(item,"entity_type",pointer);
        if(!process.types.contains(node.entity_type)) fail(pointer+"/entity_type","unknown entity type");
        const auto& type=process.types.at(node.entity_type);
        auto symbols=model.parameter_units;
        symbols.insert(type.units.begin(),type.units.end());
        if(kind=="source") {
            ++sources;
            keys(item,{"id","kind","entity_type","schedule","generator","priority"},pointer);
            if(item.contains("priority")) node.expression=expression(item,"priority",pointer,symbols,Dimension{});
            if(item.contains("schedule")==item.contains("generator")) fail(pointer,"source requires exactly one schedule or generator");
            if(item.contains("generator")) {
                const auto generator_pointer=pointer+"/generator";
                const auto& generator=item["generator"];
                keys(generator,{"count","start","values","interarrival"},generator_pointer);
                const auto count=static_cast<std::size_t>(integer(generator,"count",generator_pointer));
                const double start=generator.contains("start") ? number(generator,"start",generator_pointer) : 0;
                if(start<0) fail(generator_pointer+"/start","generator start must be nonnegative");
                auto values=record(required(generator,"values",generator_pointer),type,generator_pointer+"/values");
                const auto& policy=required(generator,"interarrival",generator_pointer);
                const auto policy_pointer=generator_pointer+"/interarrival";
                const auto method=text(policy,"kind",policy_pointer);
                if(method=="exponential") {
                    keys(policy,{"kind","rate","stream"},policy_pointer);
                    node.stream=read_stream(policy,policy_pointer);
                    node.generator=TypedProcessNode::Generator{count,start,std::move(values),
                        expression(policy,"rate",policy_pointer,model.parameter_units,Dimension{}.divided(Dimension::parse(model.time_unit)))};
                } else if(method=="constant") {
                    keys(policy,{"kind","interval"},policy_pointer);
                    node.generator=TypedProcessNode::Generator{count,start,std::move(values),
                        expression(policy,"interval",policy_pointer,model.parameter_units,Dimension::parse(model.time_unit))};
                } else fail(policy_pointer+"/kind","expected constant or exponential interarrival");
            } else {
            const auto& entries=array(item,"schedule",pointer);
            if(entries.size()>1000000) fail(pointer+"/schedule","at most one million scheduled records per source");
            double previous=0;
            for(std::size_t j=0;j<entries.size();++j) {
                const auto entry_pointer=pointer+"/schedule/"+std::to_string(j);
                keys(entries[j],{"arrival","values"},entry_pointer);
                const double time=number(entries[j],"arrival",entry_pointer);
                if(time<previous || time>model.horizon) fail(entry_pointer+"/arrival","arrival must be ordered and inside the horizon");
                previous=time;
                node.schedule.push_back({time,record(required(entries[j],"values",entry_pointer),type,entry_pointer+"/values")});
            }
            }
        } else if(kind=="sink") { ++sinks; keys(item,{"id","kind","entity_type"},pointer); }
        else if(kind=="queue") {
            keys(item,{"id","kind","entity_type","capacity","discipline","priority"},pointer);
            if(item.contains("capacity")) node.capacity=integer(item,"capacity",pointer);
            node.discipline=discipline(item,pointer);
            if(item.contains("priority")) {
                if(node.discipline!=des::QueueDiscipline::priority) fail(pointer+"/priority","priority expression requires priority discipline");
                node.priority=expression(item,"priority",pointer,symbols,Dimension{});
            }
        } else if(kind=="delay_block") {
            keys(item,{"id","kind","entity_type","duration","capacity"},pointer);
            if(required(item,"duration",pointer).is_object()) {
                const auto& policy=item["duration"];
                const auto policy_pointer=pointer+"/duration";
                keys(policy,{"kind","rate","stream"},policy_pointer);
                if(text(policy,"kind",policy_pointer)!="exponential") fail(policy_pointer+"/kind","only exponential duration policies are supported");
                node.expression=expression(policy,"rate",policy_pointer,symbols,Dimension{}.divided(Dimension::parse(model.time_unit)));
                node.stream=read_stream(policy,policy_pointer);
            } else node.expression=expression(item,"duration",pointer,symbols,Dimension::parse(model.time_unit));
            if(item.contains("capacity")) {
                node.capacity=integer(item,"capacity",pointer);
                if(*node.capacity==0) fail(pointer+"/capacity","delay capacity must be positive");
            }
        } else if(kind=="seize" || kind=="release") {
            if(kind=="seize") {
                keys(item,{"id","kind","entity_type","pool","units","preempt","priority"},pointer);
                node.expression=expression(item,"units",pointer,symbols,Dimension{});
                if(item.contains("priority")) node.priority=expression(item,"priority",pointer,symbols,Dimension{});
                if(item.contains("preempt") && (!item["preempt"].is_boolean() || item["preempt"].get<bool>()))
                    fail(pointer+"/preempt","preemption is unsupported; only false is allowed");
            } else keys(item,{"id","kind","entity_type","pool"},pointer);
            node.pool=text(item,"pool",pointer);
        } else if(kind=="select_output") {
            keys(item,{"id","kind","entity_type","branches","otherwise","stream"},pointer);
            const auto& branches=array(item,"branches",pointer);
            if(branches.empty() || branches.size()>65534) fail(pointer+"/branches","selection requires 1–65,534 branches");
            const bool probabilistic=item.contains("stream");
            if(probabilistic) {
                if(item.contains("otherwise")) fail(pointer+"/otherwise","probability selection cannot have otherwise");
                node.stream=read_stream(item,pointer);
            }
            std::set<std::string> ports;
            for(std::size_t j=0;j<branches.size();++j) {
                const auto branch_pointer=pointer+"/branches/"+std::to_string(j);
                const auto& branch=branches[j];
                keys(branch,probabilistic ? std::initializer_list<const char*>{"port","probability"} :
                    std::initializer_list<const char*>{"port","condition"},branch_pointer);
                const auto port=identifier(branch,"port",branch_pointer);
                if(!ports.insert(port).second) fail(branch_pointer+"/port","duplicate branch port");
                node.ports.push_back(port);
                if(probabilistic) node.probabilities.push_back(number(branch,"probability",branch_pointer));
                else {
                    const auto& guard=required(branch,"condition",branch_pointer);
                    const auto guard_pointer=branch_pointer+"/condition";
                    keys(guard,{"left","op","right"},guard_pointer);
                    auto left=expression(guard,"left",guard_pointer,symbols);
                    auto right=expression(guard,"right",guard_pointer,symbols,left.infer_unit(symbols,{}));
                    const auto op=text(guard,"op",guard_pointer);
                    if(!std::set<std::string>{"lt","le","eq","ne","ge","gt"}.contains(op)) fail(guard_pointer+"/op","unknown numeric comparison");
                    node.conditions.push_back({std::move(left),op,std::move(right)});
                }
            }
            if(probabilistic) {
                try { (void)rng::Categorical(node.probabilities); }
                catch(const std::invalid_argument& error) { fail(pointer+"/branches",error.what()); }
            } else {
                const auto port=identifier(item,"otherwise",pointer);
                if(!ports.insert(port).second) fail(pointer+"/otherwise","duplicate otherwise port");
                node.ports.push_back(port);
            }
        } else fail(pointer+"/kind","unsupported typed process kind");
        process.nodes.emplace(id,std::move(node));
    }
    if(sources==0 || sinks==0) fail("/components","typed process requires a source and a sink");
    std::map<std::string,std::uint32_t> blocks;
    for(auto& [id,node]:process.nodes) {
        (void)id;
        if(node.kind!="seize" && node.kind!="release") continue;
        if(!process.nodes.contains(node.pool) || process.nodes.at(node.pool).kind!="resource_pool") fail(node.pointer+"/pool","unknown resource pool");
        node.pool_namespace=process.nodes.at(node.pool).pool_namespace;
        if(node.kind=="seize") {
            auto& next=blocks[node.pool];
            if(next>65535) fail(node.pointer,"too many seize blocks for pool");
            node.block=static_cast<std::uint16_t>(next++);
        }
    }
    for(const auto& [id,node]:process.nodes)
        if(node.kind=="resource_pool" && !blocks.contains(id)) fail(node.pointer,"resource pool has no seize block");
    const auto& links=array(document,"links","");
    for(std::size_t i=0;i<links.size();++i) {
        const auto pointer="/links/"+std::to_string(i);
        keys(links[i],{"from","to","port"},pointer);
        const auto from=text(links[i],"from",pointer),to=text(links[i],"to",pointer);
        if(!process.nodes.contains(from) || !process.nodes.contains(to)) fail(pointer,"unknown process endpoint");
        const auto& source=process.nodes.at(from); const auto& target=process.nodes.at(to);
        if(source.kind=="resource_pool" || target.kind=="resource_pool") fail(pointer,"resource protocol links are wired automatically");
        if(source.entity_type!=target.entity_type) fail(pointer,"linked entity types differ");
        const auto port=links[i].contains("port") ? text(links[i],"port",pointer) : "out";
        std::uint32_t output_port=source.kind=="source" ? 0 : 1;
        if(source.kind=="select_output") {
            const auto found=std::find(source.ports.begin(),source.ports.end(),port);
            if(found==source.ports.end()) fail(pointer+"/port","unknown selection output");
            output_port=static_cast<std::uint32_t>(found-source.ports.begin()+1);
        } else if(port=="rejected") output_port=source.kind=="queue" ? 3 : 2;
        process.links.push_back({from,to,port,output_port});
    }
    validate_graph(process);
    std::sort(process.links.begin(),process.links.end(),[](const auto& a,const auto& b) {
        return std::tie(a.from,a.port,a.to)<std::tie(b.from,b.port,b.to);
    });
    const std::map<std::string,std::set<std::string>> metrics{
        {"source",{"emitted"}}, {"sink",{"completed","cycle_total","cycle_mean"}},
        {"queue",{"waiting","accepted","released","rejected","queue_mean","wait_total"}},
        {"delay_block",{"active","accepted","completed","rejected","in_process_mean","utilization"}},
        {"seize",{"received","waiting","granted"}}, {"release",{"released"}},
        {"resource_pool",{"capacity","allocated","waiting","utilization","queue_mean","allocated_mean"}},
        {"select_output",{"received","routed"}}
    };
    const auto& outputs=array(document,"outputs","");
    if(outputs.empty()) fail("/outputs","at least one output is required");
    for(std::size_t i=0;i<outputs.size();++i) {
        const auto pointer="/outputs/"+std::to_string(i);
        keys(outputs[i],{"id","component","metric","port"},pointer);
        const auto id=identifier(outputs[i],"id",pointer);
        if(!ids.insert(id).second) fail(pointer+"/id","duplicate output identifier","IR_ID");
        const auto component=text(outputs[i],"component",pointer),metric=text(outputs[i],"metric",pointer);
        if(!process.nodes.contains(component)) fail(pointer+"/component","unknown output component");
        const auto& node=process.nodes.at(component);
        if(!metrics.at(node.kind).contains(metric)) fail(pointer+"/metric","metric is unavailable for this component");
        if(metric=="utilization" && node.kind=="delay_block" && !node.capacity) fail(pointer+"/metric","unbounded delay has no utilization denominator");
        if(metric=="routed") {
            const auto port=text(outputs[i],"port",pointer);
            const auto found=std::find(node.ports.begin(),node.ports.end(),port);
            if(found==node.ports.end()) fail(pointer+"/port","unknown selector port");
            process.output_ports.emplace(id,static_cast<std::uint32_t>(found-node.ports.begin()+1));
        } else if(outputs[i].contains("port")) fail(pointer+"/port","port is only valid for a routed metric");
        model.outputs.push_back({id,component,false,metric});
    }
    model.typed_process=std::move(process);
}
} // namespace ankurafathom::ir
