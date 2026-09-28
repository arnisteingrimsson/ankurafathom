#include "typed_abm.hpp"
#include "data_bindings.hpp"
#include "ankurafathom/abm/population_atomic.hpp"
#include "ankurafathom/abm/statechart.hpp"

namespace ankurafathom::ir {
namespace {
using Json=nlohmann::json;
using Chart=abm::Statechart<AgentTag>;
using Atomic=abm::PopulationAtomic<AgentTag>;
constexpr double max_integer=9007199254740991.;
[[noreturn]] void fail(const std::string& p,const std::string& m,const std::string& code="IR_ABM") { throw Error(code,p,m); }
std::string escape(const std::string& s) { std::string v; for(char c:s) v+=c=='~' ? "~0" : c=='/' ? "~1" : std::string(1,c); return v; }
void keys(const Json& j,std::initializer_list<const char*> allowed,const std::string& p) {
    if(!j.is_object()) fail(p,"expected object","IR_TYPE");
    for(auto i=j.begin();i!=j.end();++i)
        if(std::none_of(allowed.begin(),allowed.end(),[&](const char* a) { return i.key()==a; })) fail(p+"/"+escape(i.key()),"unknown field","IR_FIELD");
}
const Json& get(const Json& j,const std::string& name,const std::string& p) {
    if(!j.contains(name)) fail(p+"/"+escape(name),"missing required field","IR_MISSING");
    return j.at(name);
}
std::string text(const Json& j,const std::string& name,const std::string& p) {
    const auto& v=get(j,name,p); if(!v.is_string() || v.get<std::string>().empty()) fail(p+"/"+name,"expected nonempty string","IR_TYPE");
    return v.get<std::string>();
}
std::string identifier(const Json& j,const std::string& name,const std::string& p) {
    auto s=text(j,name,p);
    if(std::isdigit(static_cast<unsigned char>(s[0])) || s=="t" || s=="dt" || Expression::builtin(s) ||
       std::any_of(s.begin(),s.end(),[](unsigned char c) { return !std::isalnum(c) && c!='_'; })) fail(p+"/"+name,"invalid identifier","IR_ID");
    return s;
}
double number(const Json& v,const std::string& p) {
    if(!v.is_number() || !std::isfinite(v.get<double>())) fail(p,"expected finite number","IR_TYPE");
    return v.get<double>();
}
std::int64_t integer(const Json& v,const std::string& p,double low=0,double high=1000000) {
    auto n=number(v,p); if(n<low || n>high || std::floor(n)!=n) fail(p,"integer out of range","IR_TYPE");
    return static_cast<std::int64_t>(n);
}
const Json& array(const Json& j,const std::string& name,const std::string& p) {
    const auto& v=get(j,name,p); if(!v.is_array()) fail(p+"/"+name,"expected array","IR_TYPE"); return v;
}
Dimension unit(const Json& j,const std::string& p) {
    try { return Dimension::parse(text(j,"unit",p)); } catch(const std::invalid_argument& e) { fail(p+"/unit",e.what(),"IR_UNIT"); }
}
Expression expression(const Json& j,const std::string& name,const std::string& p,const std::map<std::string,Dimension>& symbols,Dimension expected) {
    try {
        Expression e(text(j,name,p));
        for(const auto& s:e.symbols()) if(!symbols.contains(s)) fail(p+"/"+name,"unknown or nonnumeric symbol: "+s,"IR_SYMBOL");
        for(const auto& f:e.functions()) if(f!="NONNEGATIVE") fail(p+"/"+name,"unsupported function","IR_FUNCTION");
        if(e.infer_unit(symbols)!=expected) fail(p+"/"+name,"expression dimension differs","IR_UNIT");
        return e;
    } catch(const std::invalid_argument& e) { fail(p+"/"+name,e.what(),"IR_EXPRESSION"); }
}
double numeric(const AgentStore::Value& v) {
    if(auto p=std::get_if<double>(&v)) return *p;
    if(auto p=std::get_if<std::int64_t>(&v)) {
        if(*p < -9007199254740991LL || *p > 9007199254740991LL) throw std::overflow_error("integer exceeds exact numeric range");
        return static_cast<double>(*p);
    }
    if(auto p=std::get_if<bool>(&v)) return *p ? 1 : 0;
    throw std::invalid_argument("string is not numeric");
}
struct Queries {
    std::optional<abm::SpatialSnapshot<AgentTag>::Binding> space;
    std::vector<TypedAbm::Query> entries;
    explicit Queries(const TypedAbm& spec):space(spec.space),entries(spec.queries) {}
    double evaluate(const TypedAbm::Query& q,const AgentStore& s,AgentStore::Reference agent)const {
        if(!s.alive(agent)) throw std::invalid_argument("inactive query agent");
        std::vector<std::uint64_t> neighbors;
        if(q.source=="space") neighbors=abm::SpatialSnapshot<AgentTag>(s,*space).neighbors(agent,q.radius,q.moore,q.include_self);
        else if(q.source=="network") {
            if(!s.network()) throw std::invalid_argument("network query requires population topology");
            for(auto id:s.network()->neighbors(agent.id)) neighbors.push_back(id);
            if(q.include_self) { neighbors.push_back(agent.id); std::sort(neighbors.begin(),neighbors.end()); }
        } else for(auto id=s.first_id();id<s.next_id();++id)
            if(s.alive({s.store_id(),id}) && (q.include_self || id!=agent.id)) neighbors.push_back(id);
        if(q.op=="count") return static_cast<double>(neighbors.size());
        double total=0;
        for(auto id:neighbors) {
            const auto v=s.field({s.store_id(),id},q.field);
            total+=q.op=="count_same" ? static_cast<double>(v==s.field(agent,q.field)) : numeric(v);
            if(!std::isfinite(total)) throw std::overflow_error("nonfinite neighborhood aggregate");
            if(s.schema()[q.field].kind!=des::FieldKind::real && std::abs(total)>max_integer)
                throw std::overflow_error("integer neighborhood aggregate exceeds exact numeric range");
        }
        return q.op=="mean" && !neighbors.empty() ? total/static_cast<double>(neighbors.size()) : total;
    }
};
std::map<std::string,double> values(const AgentStore& s,AgentStore::Reference agent,const std::map<std::string,double>& parameters,const Queries& queries) {
    auto result=parameters;
    for(std::size_t i=0;i<s.schema().size();++i) if(s.schema()[i].kind!=des::FieldKind::string) result.emplace(s.schema()[i].name,numeric(s.field(agent,i)));
    for(const auto& q:queries.entries) result.emplace(q.id,queries.evaluate(q,s,agent));
    return result;
}
AgentStore::Record assign_record(AgentStore::Record record,const AgentAssignments& assignments,const AgentStore& s,const std::map<std::string,double>& symbols) {
    for(const auto& [index,e]:assignments) {
        const auto v=e.evaluate(symbols);
        switch(s.schema()[index].kind) {
            case des::FieldKind::real: record[index]=v; break;
            case des::FieldKind::integer:
                if(std::abs(v)>max_integer || std::floor(v)!=v) throw std::domain_error("integer assignment is not exact");
                record[index]=static_cast<std::int64_t>(v); break;
            case des::FieldKind::boolean:
                if(v!=0 && v!=1) throw std::domain_error("boolean assignment must equal zero or one");
                record[index]=(v==1); break;
            case des::FieldKind::string: throw std::invalid_argument("string assignment unsupported");
        }
    }
    return record;
}
AgentStore::Record assign(const AgentAssignments& assignments,AgentStore::Reference agent,const AgentStore& s,const std::map<std::string,double>& parameters,const Queries& queries) {
    return assign_record(s.record(agent),assignments,s,values(s,agent,parameters,queries));
}
Atomic::Sync::Lifecycle behavior(const TypedAbm::Behavior& spec,AgentStore::Reference agent,const AgentStore& s,const std::map<std::string,double>& parameters,const Queries& queries) {
    Atomic::Sync::Lifecycle result;
    const auto environment=values(s,agent,parameters,queries);
    result.retire=spec.retire && spec.retire->evaluate(environment)!=0;
    for(const auto& birth:spec.births) {
        if(birth.guard && birth.guard->evaluate(environment)==0) continue;
        result.births.push_back(assign_record(birth.record,birth.assignments,s,environment));
    }
    return result;
}
std::vector<Atomic::Emission> emissions(const std::vector<TypedAbm::Publish>& entries,AgentStore::Reference agent,
                                      const AgentStore& store,const std::map<std::string,double>& environment,
                                      std::optional<AgentStore::Reference> sender={}) {
    std::vector<Atomic::Emission> result;
    for(const auto& entry:entries) {
        std::optional<AgentStore::Reference> receiver;
        if(entry.target=="agent") receiver=AgentStore::Reference{store.store_id(),entry.receiver};
        else if(entry.target=="self") receiver=agent;
        else if(entry.target=="sender") {
            if(!sender) throw std::invalid_argument("publication lacks sender context");
            receiver=sender;
        }
        result.push_back({entry.topic,receiver,entry.value.evaluate(environment)});
    }
    return result;
}
struct GraphParameters { double probability=0; std::size_t degree=0,m=0; };
GraphParameters graph_parameters(const TypedAbm& spec,const std::map<std::string,double>& parameters) {
    const auto& generator=*spec.network_generator; const auto n=spec.agents.size();
    GraphParameters result;
    if(generator.probability) {
        result.probability=generator.probability->evaluate(parameters);
        if(!std::isfinite(result.probability) || result.probability<0 || result.probability>1) throw std::invalid_argument("graph probability must be in [0,1]");
    }
    const auto count=[&](const Expression& expression) {
        const double value=expression.evaluate(parameters);
        if(!std::isfinite(value) || value<0 || value>1000000 || std::floor(value)!=value) throw std::invalid_argument("graph count must be an exact bounded integer");
        return static_cast<std::size_t>(value);
    };
    if(generator.degree) {
        result.degree=count(*generator.degree);
        if(n==0 || result.degree>=n || result.degree%2) throw std::invalid_argument("WS degree must be even and smaller than positive population size");
    }
    if(generator.m) {
        result.m=count(*generator.m);
        if(result.m==0 || result.m>=n) throw std::invalid_argument("BA requires 1 <= m < population size");
    }
    return result;
}
abm::CsrNetwork generate_network(const TypedAbm& spec,const std::map<std::string,double>& parameters,abm::NetworkDraws draws) {
    const auto options=graph_parameters(spec,parameters); const auto& generator=*spec.network_generator;
    std::vector<std::uint64_t> vertices; for(std::size_t i=0;i<spec.agents.size();++i) vertices.push_back(i);
    draws.stream=generator.stream;
    if(generator.kind=="erdos_renyi") return abm::erdos_renyi(std::move(vertices),options.probability,draws);
    if(generator.kind=="watts_strogatz") return abm::watts_strogatz(std::move(vertices),options.degree,options.probability,draws);
    return abm::barabasi_albert(std::move(vertices),options.m,draws);
}
Chart chart(const TypedAbm& spec,const AgentStore& store,const std::map<std::string,double>& parameters) {
    const Queries queries(spec);
    std::vector<Chart::Transition> transitions;
    for(const auto& t:spec.transitions) {
        Chart::Transition rule;
        rule.name=t.id; rule.source=t.source; rule.target=t.target; rule.priority=t.priority; rule.stream=t.stream;
        rule.trigger=t.trigger=="message" ? Chart::Trigger::message : t.trigger=="timeout" ? Chart::Trigger::timeout : Chart::Trigger::rate;
        rule.message=t.event;
        if(t.value) {
            try { rule.value=t.value->evaluate(parameters); if(rule.value<0) fail(t.pointer,"negative timeout or rate"); }
            catch(const Error&) { throw; } catch(const std::exception& e) { fail(t.pointer,e.what()); }
        }
        if(t.guard) rule.guard=[e=*t.guard,parameters,queries](auto r,const AgentStore& s) { return e.evaluate(values(s,r,parameters,queries))!=0; };
        if(!t.assignments.empty()) rule.action=[assignments=t.assignments,parameters,queries](auto r,const AgentStore& s) { return assign(assignments,r,s,parameters,queries); };
        if(!t.publish.empty()) rule.publish=[entries=t.publish,parameters,queries](auto r,const AgentStore& s) {
            return emissions(entries,r,s,values(s,r,parameters,queries));
        };
        if(t.lifecycle.retire || !t.lifecycle.births.empty()) rule.lifecycle=[life=t.lifecycle,parameters,queries](auto r,const AgentStore& s) { return behavior(life,r,s,parameters,queries); };
        transitions.push_back(std::move(rule));
    }
    return Chart(store,{spec.chart_fields[0],spec.chart_fields[1],spec.chart_fields[2]},spec.states,std::move(transitions),"statechart/",spec.initial);
}
}

void parse_typed_abm(const Json& document,Model& model,std::set<std::string>& ids,
                     const std::filesystem::path* base,std::optional<std::size_t> population_binding) {
    if(document.contains("links")) fail("/links","ABM links are not supported","IR_FIELD");
    const auto& components=array(document,"components","");
    if(components.size()!=1) fail("/components","expected exactly one population");
    const std::string p="/components/0"; const auto& item=components[0];
    keys(item,{"id","kind","execution","fields","agents","phases","chart","messages","space","network","queries","topics","publications","delivery_budget","lifecycle","agent_limit","network_updates"},p);
    if(!ids.insert(identifier(item,"id",p)).second) fail(p+"/id","duplicate identifier","IR_ID");
    if(text(item,"kind",p)!="population") fail(p+"/kind","expected population");
    const auto execution=text(item,"execution",p);
    if(execution!="sync" && execution!="async") fail(p+"/execution","expected sync or async");
    TypedAbm spec; spec.asynchronous=execution=="async";
    if(spec.asynchronous ? item.contains("phases") : (item.contains("chart") || item.contains("messages"))) fail(p,"fields for wrong execution mode");
    auto symbols=model.parameter_units;
    const auto& fields=array(item,"fields",p);
    if(fields.empty()) fail(p+"/fields","agent needs at least one field");
    for(std::size_t i=0;i<fields.size();++i) {
        const auto fp=p+"/fields/"+std::to_string(i); const auto& f=fields[i]; keys(f,{"name","type","unit"},fp);
        const auto name=identifier(f,"name",fp),type=text(f,"type",fp); const auto dimension=unit(f,fp);
        if(spec.units.contains(name) || model.parameters.contains(name)) fail(fp+"/name","duplicate or parameter-colliding field","IR_ID");
        des::FieldKind kind;
        if(type=="real") kind=des::FieldKind::real;
        else if(type=="integer") kind=des::FieldKind::integer;
        else if(type=="boolean") kind=des::FieldKind::boolean;
        else if(type=="string") kind=des::FieldKind::string;
        else fail(fp+"/type","unsupported field type");
        if((kind==des::FieldKind::boolean || kind==des::FieldKind::string) && dimension!=Dimension{}) fail(fp+"/unit","boolean/string fields must be dimensionless","IR_UNIT");
        spec.fields.push_back({name,kind}); spec.units.emplace(name,dimension);
        if(kind!=des::FieldKind::string) symbols.emplace(name,dimension);
    }
    if(item.contains("agent_limit")) spec.agent_limit=static_cast<std::uint64_t>(integer(item.at("agent_limit"),p+"/agent_limit",1));
    AgentStore store(0,spec.fields);
    const auto field_index=[&](const std::string& name,const std::string& pointer) {
        try { return store.field_index(name); } catch(const std::out_of_range&) { fail(pointer,"unknown agent field","IR_REF"); }
    };
    const auto parse_record=[&](const Json& agent,const std::string& ap) {
        if(!agent.is_object() || agent.size()!=fields.size()) fail(ap,"record must contain exactly the declared fields");
        AgentStore::Record record;
        for(const auto& f:spec.fields) {
            const auto& v=get(agent,f.name,ap); const auto fp=ap+"/"+f.name;
            switch(f.kind) {
                case des::FieldKind::real: record.emplace_back(number(v,fp)); break;
                case des::FieldKind::integer: record.emplace_back(integer(v,fp,-max_integer,max_integer)); break;
                case des::FieldKind::boolean: if(!v.is_boolean()) fail(fp,"expected boolean"); record.emplace_back(v.get<bool>()); break;
                case des::FieldKind::string: if(!v.is_string()) fail(fp,"expected string"); record.emplace_back(v.get<std::string>()); break;
            }
        }
        return record;
    };
    if(population_binding) {
        if(!base) fail("/data","data bindings require a model file","IR_DATA");
        if(item.contains("agents")) fail(p+"/agents","choose inline agents or a population binding","IR_DATA");
        model.population_data=resolve_population_data(document.at("data").at(*population_binding),
            *population_binding,*base,text(item,"id",p),spec);
    } else {
        const auto& agents=array(item,"agents",p);
        if(agents.size()>spec.agent_limit) fail(p+"/agents","too many agents");
        for(std::size_t i=0;i<agents.size();++i) spec.agents.push_back(parse_record(agents[i],p+"/agents/"+std::to_string(i)));
    }
    // Lifetimes also validate forward references to scheduled newborn IDs.
    std::vector<double> born_at(spec.agents.size(),0),retired_at(spec.agents.size(),std::numeric_limits<double>::infinity());
    if(item.contains("lifecycle")) {
        const auto& entries=array(item,"lifecycle",p);
        for(std::size_t i=0;i<entries.size();++i) {
            const auto lp=p+"/lifecycle/"+std::to_string(i); const auto& entry=entries[i];
            keys(entry,{"time","sequence","retire","births"},lp);
            TypedAbm::Lifecycle change;
            change.time=number(get(entry,"time",lp),lp+"/time");
            if(change.time<0 || change.time>model.horizon) fail(lp+"/time","lifecycle outside horizon");
            change.sequence=static_cast<std::uint64_t>(integer(get(entry,"sequence",lp),lp+"/sequence",0,max_integer));
            for(const auto& ref:array(entry,"retire",lp)) change.retire.push_back(static_cast<std::uint64_t>(integer(ref,lp+"/retire")));
            const auto& records=array(entry,"births",lp);
            for(std::size_t j=0;j<records.size();++j) change.births.push_back(parse_record(records[j],lp+"/births/"+std::to_string(j)));
            spec.lifecycle.push_back(std::move(change));
        }
        std::sort(spec.lifecycle.begin(),spec.lifecycle.end(),[](const auto& a,const auto& b) { return std::tie(a.time,a.sequence)<std::tie(b.time,b.sequence); });
        for(std::size_t begin=0;begin<spec.lifecycle.size();) {
            auto end=begin+1; const auto time=spec.lifecycle[begin].time;
            while(end<spec.lifecycle.size() && spec.lifecycle[end].time==time) ++end;
            for(auto i=begin;i<end;++i) {
                if(i>begin && spec.lifecycle[i].sequence==spec.lifecycle[i-1].sequence) fail(p+"/lifecycle","duplicate lifecycle sequence");
                for(auto ref:spec.lifecycle[i].retire) {
                    if(ref>=born_at.size() || retired_at[ref]<=time) fail(p+"/lifecycle","retirement requires a live agent from before this batch","IR_REF");
                    retired_at[ref]=time;
                }
            }
            for(auto i=begin;i<end;++i) for(std::size_t j=0;j<spec.lifecycle[i].births.size();++j) {
                if(born_at.size()>=spec.agent_limit) fail(p+"/lifecycle","too many total agents");
                born_at.push_back(time); retired_at.push_back(std::numeric_limits<double>::infinity());
            }
            begin=end;
        }
    }
    const auto live_at=[&](std::uint64_t id,double time) { return id<born_at.size() && born_at[id]<=time && time<retired_at[id]; };
    const auto boolean=[&](const Json& j,const char* name,const std::string& pointer,bool fallback=false) {
        if(!j.contains(name)) return fallback;
        if(!j.at(name).is_boolean()) fail(pointer+"/"+name,"expected boolean","IR_TYPE");
        return j.at(name).get<bool>();
    };
    Dimension space_unit;
    if(item.contains("space")) {
        const auto sp=p+"/space"; const auto& space=item.at("space");
        const auto kind=text(space,"kind",sp);
        using Snapshot=abm::SpatialSnapshot<AgentTag>;
        if(kind=="grid") {
            keys(space,{"kind","x","y","width","height","wrap"},sp);
            const auto x=field_index(text(space,"x",sp),sp+"/x"),y=field_index(text(space,"y",sp),sp+"/y");
            if(spec.units.at(spec.fields[x].name)!=Dimension{} || spec.units.at(spec.fields[y].name)!=Dimension{}) fail(sp,"grid coordinates must be dimensionless","IR_UNIT");
            const auto width=integer(get(space,"width",sp),sp+"/width",1),height=integer(get(space,"height",sp),sp+"/height",1);
            if(width*height>1000000) fail(sp,"declarative grid exceeds one million cells");
            spec.space=Snapshot::Grid{x,y,width,height,boolean(space,"wrap",sp)};
        } else if(kind=="continuous") {
            keys(space,{"kind","fields","lower","upper","bin_width","wrap","unit"},sp);
            space_unit=unit(space,sp);
            Snapshot::Continuous b; b.bin_width=number(get(space,"bin_width",sp),sp+"/bin_width"); b.wrap=boolean(space,"wrap",sp);
            for(const auto& name:array(space,"fields",sp)) {
                if(!name.is_string()) fail(sp+"/fields","expected field names","IR_TYPE");
                const auto index=field_index(name.get<std::string>(),sp+"/fields");
                if(spec.units.at(spec.fields[index].name)!=space_unit) fail(sp+"/fields","coordinate unit differs","IR_UNIT");
                b.fields.push_back(index);
            }
            for(const auto& v:array(space,"lower",sp)) b.lower.push_back(number(v,sp+"/lower"));
            for(const auto& v:array(space,"upper",sp)) b.upper.push_back(number(v,sp+"/upper"));
            spec.space=std::move(b);
        } else fail(sp+"/kind","unknown space kind");
        try { auto initial=store; initial.spawn_many(spec.agents); (void)Snapshot(initial,*spec.space); }
        catch(const std::exception& e) { fail(sp,e.what()); }
    }
    if(item.contains("network")) {
        const auto np=p+"/network"; const auto& network=item.at("network");
        if(network.contains("generator")) {
            keys(network,{"generator"},np);
            const auto gp=np+"/generator"; const auto& g=network.at("generator");
            TypedAbm::NetworkGenerator generator; generator.kind=text(g,"kind",gp);
            if(generator.kind=="erdos_renyi") keys(g,{"kind","probability","stream"},gp);
            else if(generator.kind=="watts_strogatz") keys(g,{"kind","degree","probability","stream"},gp);
            else if(generator.kind=="barabasi_albert") keys(g,{"kind","m","stream"},gp);
            else fail(gp+"/kind","unknown graph generator");
            generator.stream=static_cast<std::uint32_t>(integer(get(g,"stream",gp),gp+"/stream",0,65535));
            if(generator.kind!="barabasi_albert") generator.probability=expression(g,"probability",gp,model.parameter_units,Dimension{});
            if(generator.kind=="watts_strogatz") generator.degree=expression(g,"degree",gp,model.parameter_units,Dimension{});
            if(generator.kind=="barabasi_albert") generator.m=expression(g,"m",gp,model.parameter_units,Dimension{});
            spec.network_generator=std::move(generator);
            try { (void)graph_parameters(spec,model.parameters); }
            catch(const std::exception& e) { fail(gp,e.what()); }
        } else {
            keys(network,{"directed","edges"},np);
            std::vector<std::uint64_t> vertices; for(std::size_t i=0;i<spec.agents.size();++i) vertices.push_back(i);
            std::vector<std::pair<std::uint64_t,std::uint64_t>> edges;
            for(const auto& edge:array(network,"edges",np)) {
                if(!edge.is_array() || edge.size()!=2) fail(np+"/edges","edge requires two agent IDs");
                edges.emplace_back(integer(edge[0],np+"/edges"),integer(edge[1],np+"/edges"));
            }
            try { spec.network.emplace(vertices,edges,boolean(network,"directed",np)); }
            catch(const Error&) { throw; } catch(const std::exception& e) { fail(np,e.what()); }
        }
    }
    if(item.contains("network_updates")) {
        if(!spec.network && !spec.network_generator) fail(p+"/network_updates","network updates require a network","IR_REF");
        const auto& entries=array(item,"network_updates",p);
        std::set<std::pair<double,std::uint64_t>> seen;
        for(std::size_t i=0;i<entries.size();++i) {
            const auto np=p+"/network_updates/"+std::to_string(i); const auto& entry=entries[i];
            keys(entry,{"time","sequence","add","remove"},np);
            TypedAbm::NetworkUpdate update;
            update.time=number(get(entry,"time",np),np+"/time");
            update.sequence=static_cast<std::uint64_t>(integer(get(entry,"sequence",np),np+"/sequence",0,max_integer));
            if(update.time<0 || update.time>model.horizon) fail(np+"/time","network update outside horizon");
            if(!seen.emplace(update.time,update.sequence).second) fail(np,"duplicate network update sequence");
            const auto edges=[&](const char* name) {
                std::vector<abm::CsrNetwork::Edge> result;
                for(const auto& edge:array(entry,name,np)) {
                    if(!edge.is_array() || edge.size()!=2) fail(np+"/"+name,"edge requires two agent IDs");
                    const auto a=static_cast<std::uint64_t>(integer(edge[0],np+"/"+name));
                    const auto b=static_cast<std::uint64_t>(integer(edge[1],np+"/"+name));
                    if(a==b || !live_at(a,update.time) || !live_at(b,update.time)) fail(np+"/"+name,"edge endpoints must be distinct and live at update time","IR_REF");
                    result.emplace_back(a,b);
                }
                return result;
            };
            update.add=edges("add"); update.remove=edges("remove");
            spec.network_updates.push_back(std::move(update));
        }
    }
    std::set<std::string> query_names;
    if(item.contains("queries")) {
        const auto& queries=array(item,"queries",p);
        for(std::size_t i=0;i<queries.size();++i) {
            const auto qp=p+"/queries/"+std::to_string(i); const auto& q=queries[i];
            keys(q,{"id","source","op","field","radius","unit","moore","include_self"},qp);
            TypedAbm::Query query; query.id=identifier(q,"id",qp); query.source=text(q,"source",qp); query.op=text(q,"op",qp);
            if(spec.units.contains(query.id) || model.parameters.contains(query.id) || !query_names.insert(query.id).second) fail(qp+"/id","duplicate or colliding query name","IR_ID");
            query.include_self=boolean(q,"include_self",qp);
            if(query.source=="space") {
                if(!spec.space) fail(qp+"/source","query needs a space","IR_REF");
                query.radius=number(get(q,"radius",qp),qp+"/radius");
                if(query.radius<0) fail(qp+"/radius","negative query radius");
                if(unit(q,qp)!=space_unit) fail(qp+"/unit","radius unit differs","IR_UNIT");
                if(std::holds_alternative<abm::SpatialSnapshot<AgentTag>::Grid>(*spec.space)) {
                    (void)integer(q.at("radius"),qp+"/radius",0,max_integer);
                    query.moore=boolean(q,"moore",qp,true);
                } else if(q.contains("moore")) fail(qp+"/moore","Moore selector requires grid space");
            } else {
                if(query.source!="network" && query.source!="population") fail(qp+"/source","unknown query source");
                if(query.source=="network" && !spec.network && !spec.network_generator) fail(qp+"/source","query needs a network","IR_REF");
                if(q.contains("radius") || q.contains("unit") || q.contains("moore")) fail(qp,"spatial options require spatial query");
            }
            Dimension result_unit;
            if(query.op=="count") {
                if(q.contains("field")) fail(qp+"/field","count does not select a field");
            } else if(query.op=="sum" || query.op=="mean" || query.op=="count_same") {
                const auto name=text(q,"field",qp); query.field=field_index(name,qp+"/field");
                if(query.op!="count_same") {
                    if(spec.fields[query.field].kind==des::FieldKind::string) fail(qp+"/field","string aggregation unsupported");
                    result_unit=spec.units.at(name);
                }
            } else fail(qp+"/op","unknown query operation");
            symbols.emplace(query.id,result_unit); spec.queries.push_back(std::move(query));
        }
    }
    std::map<std::string,Dimension> topic_units;
    if(item.contains("topics")) {
        const auto& topics=array(item,"topics",p);
        for(std::size_t i=0;i<topics.size();++i) {
            const auto tp=p+"/topics/"+std::to_string(i); const auto& topic=topics[i];
            keys(topic,{"id","capacity","unit","guard","assign","publish"},tp);
            const auto name=identifier(topic,"id",tp);
            if(!topic_units.emplace(name,unit(topic,tp)).second) fail(tp+"/id","duplicate topic","IR_ID");
        }
    }
    const auto agent_id=[&](const Json& value,const std::string& pointer) {
        const auto id=static_cast<std::uint64_t>(integer(value,pointer));
        if(id>=born_at.size()) fail(pointer,"unknown publication agent","IR_REF");
        return id;
    };
    const auto assignments=[&](const Json& j,const std::string& pointer,const std::set<std::size_t>& reserved) {
        AgentAssignments result; std::set<std::size_t> seen;
        const auto& entries=array(j,"assign",pointer);
        for(std::size_t i=0;i<entries.size();++i) {
            const auto ap=pointer+"/assign/"+std::to_string(i); const auto& a=entries[i]; keys(a,{"field","expr"},ap);
            const auto name=text(a,"field",ap); const auto index=field_index(name,ap+"/field");
            if(!seen.insert(index).second || reserved.contains(index) || spec.fields[index].kind==des::FieldKind::string) fail(ap+"/field","duplicate, reserved or nonnumeric assignment target");
            result.emplace_back(index,expression(a,"expr",ap,symbols,spec.units.at(name)));
        }
        return result;
    };
    bool behavior_births=false;
    const auto parse_behavior=[&](const Json& j,const std::string& pointer,const std::set<std::size_t>& reserved) {
        TypedAbm::Behavior result;
        if(!j.contains("lifecycle")) return result;
        const auto lp=pointer+"/lifecycle"; const auto& life=j.at("lifecycle");
        keys(life,{"retire","births"},lp);
        if(life.empty()) fail(lp,"lifecycle needs retire or births");
        if(life.contains("retire")) result.retire=expression(life,"retire",lp,symbols,Dimension{});
        if(life.contains("births")) {
            const auto& births=array(life,"births",lp);
            if(births.size()>spec.agent_limit) fail(lp+"/births","birth list exceeds agent limit");
            for(std::size_t i=0;i<births.size();++i) {
                const auto bp=lp+"/births/"+std::to_string(i); const auto& entry=births[i];
                keys(entry,{"record","guard","assign"},bp);
                TypedAbm::Birth birth; birth.record=parse_record(get(entry,"record",bp),bp+"/record");
                if(spec.asynchronous && std::get<std::int64_t>(birth.record[field_index(spec.chart_fields[2],bp)])!=-1)
                    fail(bp+"/record","birth chart generation must be -1");
                if(entry.contains("guard")) birth.guard=expression(entry,"guard",bp,symbols,Dimension{});
                if(entry.contains("assign")) birth.assignments=assignments(entry,bp,reserved);
                result.births.push_back(std::move(birth)); behavior_births=true;
            }
        }
        return result;
    };
    const auto parse_publish=[&](const Json& j,const std::string& pointer,bool has_sender) {
        std::vector<TypedAbm::Publish> result;
        if(!j.contains("publish")) return result;
        const auto& entries=array(j,"publish",pointer);
        for(std::size_t i=0;i<entries.size();++i) {
            const auto rp=pointer+"/publish/"+std::to_string(i); const auto& entry=entries[i]; keys(entry,{"topic","receiver","value"},rp);
            const auto name=text(entry,"topic",rp);
            if(!topic_units.contains(name)) fail(rp+"/topic","unknown publication topic","IR_REF");
            const auto& receiver=get(entry,"receiver",rp); std::string target="agent"; std::uint64_t id=0;
            if(receiver.is_string()) {
                target=receiver.get<std::string>();
                if(target!="self" && target!="broadcast" && !(has_sender && target=="sender")) fail(rp+"/receiver","invalid publication target for context");
            } else id=agent_id(receiver,rp+"/receiver");
            result.push_back({name,target,id,expression(entry,"value",rp,symbols,topic_units.at(name))});
        }
        return result;
    };
    if(!spec.asynchronous) {
        const auto& phases=array(item,"phases",p);
        for(std::size_t i=0;i<phases.size();++i) { const auto pp=p+"/phases/"+std::to_string(i); keys(phases[i],{"assign","publish","lifecycle"},pp); spec.phases.push_back({assignments(phases[i],pp,{}),parse_publish(phases[i],pp,false),parse_behavior(phases[i],pp,{})}); }
    } else {
        const auto cp=p+"/chart"; const auto& c=get(item,"chart",p); keys(c,{"fields","states","initial","transitions"},cp);
        const auto& bindings=get(c,"fields",cp); keys(bindings,{"state","entered","generation"},cp+"/fields");
        spec.chart_fields={text(bindings,"state",cp+"/fields"),text(bindings,"entered",cp+"/fields"),text(bindings,"generation",cp+"/fields")};
        std::set<std::size_t> reserved;
        for(std::size_t i=0;i<3;++i) {
            const auto index=field_index(spec.chart_fields[i],cp+"/fields");
            if(!reserved.insert(index).second || spec.fields[index].kind!=(i==1 ? des::FieldKind::real : des::FieldKind::integer)) fail(cp+"/fields","chart field types/identities differ");
            if(spec.units.at(spec.chart_fields[i])!=(i==1 ? Dimension::parse(model.time_unit) : Dimension{})) fail(cp+"/fields","chart field unit differs","IR_UNIT");
        }
        std::set<std::int64_t> states;
        for(const auto& state:array(c,"states",cp)) {
            const auto value=integer(state,cp+"/states"); if(!states.insert(value).second) fail(cp+"/states","duplicate state"); spec.states.push_back(value);
        }
        spec.initial=integer(get(c,"initial",cp),cp+"/initial"); if(!states.contains(spec.initial)) fail(cp+"/initial","unknown initial state");
        const auto& transitions=array(c,"transitions",cp); std::set<std::string> names;
        for(std::size_t i=0;i<transitions.size();++i) {
            const auto tp=cp+"/transitions/"+std::to_string(i); const auto& t=transitions[i];
            keys(t,{"id","source","target","trigger","event","duration","rate","stream","priority","guard","assign","publish","lifecycle"},tp);
            TypedAbm::Transition rule; rule.pointer=tp; rule.id=identifier(t,"id",tp);
            if(!names.insert(rule.id).second) fail(tp+"/id","duplicate transition");
            rule.source=integer(get(t,"source",tp),tp+"/source"); rule.target=integer(get(t,"target",tp),tp+"/target");
            if(!states.contains(rule.source) || !states.contains(rule.target)) fail(tp,"unknown transition endpoint");
            rule.trigger=text(t,"trigger",tp);
            if(t.contains("priority")) rule.priority=static_cast<int>(integer(t.at("priority"),tp+"/priority",-1000000,1000000));
            if(rule.trigger=="message") {
                rule.event=text(t,"event",tp); if(t.contains("rate") || t.contains("duration") || t.contains("stream")) fail(tp,"mixed transition triggers");
            } else if(rule.trigger=="timeout") {
                if(t.contains("rate") || t.contains("event") || t.contains("stream")) fail(tp,"mixed transition triggers");
                rule.value=expression(t,"duration",tp,model.parameter_units,Dimension::parse(model.time_unit));
            } else if(rule.trigger=="rate") {
                if(t.contains("event") || t.contains("duration")) fail(tp,"mixed transition triggers");
                rule.value=expression(t,"rate",tp,model.parameter_units,Dimension{}.divided(Dimension::parse(model.time_unit)));
                rule.stream=static_cast<std::uint32_t>(integer(get(t,"stream",tp),tp+"/stream",0,65535));
            } else fail(tp+"/trigger","unknown trigger");
            if(t.contains("guard")) rule.guard=expression(t,"guard",tp,symbols,Dimension{});
            if(t.contains("assign")) rule.assignments=assignments(t,tp,reserved);
            rule.publish=parse_publish(t,tp,false);
            rule.lifecycle=parse_behavior(t,tp,reserved);
            spec.transitions.push_back(std::move(rule));
        }
        for(const auto& agent:spec.agents)
            if(std::get<std::int64_t>(agent[field_index(spec.chart_fields[2],cp)])!=-1) fail(document.contains("data") ? "/data/0/source" : p+"/agents","initial chart generation must be -1");
        for(const auto& change:spec.lifecycle) for(const auto& agent:change.births)
            if(std::get<std::int64_t>(agent[field_index(spec.chart_fields[2],cp)])!=-1) fail(p+"/lifecycle","birth chart generation must be -1");
        try { (void)chart(spec,store,model.parameters); } catch(const Error&) { throw; } catch(const std::exception& e) { fail(cp,e.what()); }
        std::set<std::tuple<double,std::uint64_t,std::uint64_t>> messages;
        if(item.contains("messages")) {
            const auto& entries=array(item,"messages",p);
            for(std::size_t i=0;i<entries.size();++i) {
                const auto mp=p+"/messages/"+std::to_string(i); const auto& m=entries[i]; keys(m,{"time","agent","sequence","event"},mp);
                const auto time=number(get(m,"time",mp),mp+"/time"); const auto agent=integer(get(m,"agent",mp),mp+"/agent");
                const auto sequence=integer(get(m,"sequence",mp),mp+"/sequence",0,max_integer);
                if(time<0 || time>model.horizon || !live_at(static_cast<std::uint64_t>(agent),time) || !messages.emplace(time,agent,sequence).second) fail(mp,"invalid/duplicate scheduled message");
                const auto event=text(m,"event",mp);
                if(std::none_of(spec.transitions.begin(),spec.transitions.end(),[&](const auto& t) { return t.trigger=="message" && t.event==event; })) fail(mp+"/event","undeclared message event");
                spec.messages.push_back({time,static_cast<std::uint64_t>(agent),static_cast<std::uint64_t>(sequence),event});
            }
        }
    }
    if(spec.network_generator) for(const auto& t:spec.transitions)
        if(t.trigger=="rate" && t.stream==spec.network_generator->stream) fail(t.pointer+"/stream","graph and statechart rate streams must be distinct");
    if(behavior_births && std::any_of(spec.lifecycle.begin(),spec.lifecycle.end(),[](const auto& c) { return !c.births.empty(); }))
        fail(p+"/lifecycle","scheduled and behavior births cannot share predicted IDs");
    if(item.contains("delivery_budget")) spec.delivery_budget=static_cast<std::size_t>(integer(item.at("delivery_budget"),p+"/delivery_budget",1));
    if((item.contains("publications") || item.contains("delivery_budget")) && !item.contains("topics")) fail(p,"topic options require topics");
    if(item.contains("topics")) {
        for(const auto* reserved:{"message_value","message_sender"})
            if(spec.units.contains(reserved) || symbols.contains(reserved)) fail(p+"/topics","message symbols collide with fields/parameters/queries","IR_ID");
        const auto& topics=array(item,"topics",p);
        std::set<std::size_t> reserved;
        if(spec.asynchronous) for(const auto& f:spec.chart_fields) reserved.insert(field_index(f,p+"/chart/fields"));
        for(std::size_t i=0;i<topics.size();++i) {
            const auto tp=p+"/topics/"+std::to_string(i); const auto& topic=topics[i];
            TypedAbm::Topic t; t.id=text(topic,"id",tp); t.unit=topic_units.at(t.id);
            t.capacity=static_cast<std::size_t>(integer(get(topic,"capacity",tp),tp+"/capacity"));
            symbols.emplace("message_value",t.unit); symbols.emplace("message_sender",Dimension{});
            if(topic.contains("guard")) t.guard=expression(topic,"guard",tp,symbols,Dimension{});
            if(topic.contains("assign")) t.assignments=assignments(topic,tp,reserved);
            t.publish=parse_publish(topic,tp,true);
            symbols.erase("message_value"); symbols.erase("message_sender");
            spec.topics.push_back(std::move(t));
        }
        if(item.contains("publications")) {
            std::set<std::tuple<double,std::string,std::uint64_t,std::uint64_t>> seen;
            const auto& entries=array(item,"publications",p);
            for(std::size_t i=0;i<entries.size();++i) {
                const auto pp=p+"/publications/"+std::to_string(i); const auto& entry=entries[i];
                keys(entry,{"time","topic","sender","receiver","sequence","value"},pp);
                TypedAbm::Publication v; v.time=number(get(entry,"time",pp),pp+"/time");v.topic=text(entry,"topic",pp);
                v.sender=agent_id(get(entry,"sender",pp),pp+"/sender");
                if(entry.contains("receiver")) v.receiver=agent_id(entry.at("receiver"),pp+"/receiver");
                v.sequence=static_cast<std::uint64_t>(integer(get(entry,"sequence",pp),pp+"/sequence",0,max_integer));
                v.value=number(get(entry,"value",pp),pp+"/value");
                if(v.time<0 || v.time>model.horizon) fail(pp+"/time","publication outside horizon");
                if(!live_at(v.sender,v.time) || (v.receiver && !live_at(*v.receiver,v.time))) fail(pp,"publication endpoint inactive at scheduled time","IR_REF");
                if(!topic_units.contains(v.topic)) fail(pp+"/topic","unknown publication topic","IR_REF");
                if(!seen.emplace(v.time,v.topic,v.sender,v.sequence).second) fail(pp,"duplicate topic sender sequence");
                spec.publications.push_back(std::move(v));
            }
        }
    }
    const auto& outputs=array(document,"outputs",""); if(outputs.empty()) fail("/outputs","at least one output required");
    for(std::size_t i=0;i<outputs.size();++i) {
        const auto op="/outputs/"+std::to_string(i); const auto& o=outputs[i]; keys(o,{"id","agent","field","metric","state","query","inactive_value"},op);
        TypedAbm::Output out; out.id=identifier(o,"id",op); if(!ids.insert(out.id).second) fail(op+"/id","duplicate output","IR_ID");
        if(o.contains("agent")) {
            if((o.contains("metric") && text(o,"metric",op)!="alive") || o.contains("state")) fail(op,"mixed output selectors");
            out.metric=o.contains("metric") ? "alive" : o.contains("query") ? "query" : "agent"; out.agent=static_cast<std::uint64_t>(integer(o.at("agent"),op+"/agent"));
            if(out.agent>=born_at.size()) fail(op+"/agent","unknown agent","IR_REF");
        } else {
            out.metric=text(o,"metric",op);
            if(out.metric=="agent" || out.metric=="query" || out.metric=="alive" || o.contains("query")) fail(op,"agent/query output requires an explicit agent selector");
        }
        if(o.contains("inactive_value")) {
            if(out.metric!="agent" && out.metric!="query") fail(op,"inactive_value requires field/query agent output");
            out.inactive_value=number(o.at("inactive_value"),op+"/inactive_value");
        }
        if(out.metric=="alive") {
            if(o.contains("field") || o.contains("query")) fail(op,"alive cannot select field/query");
        } else if(out.metric=="query") {
            if(o.contains("field")) fail(op,"mixed query/field selectors");
            const auto name=text(o,"query",op);
            const auto found=std::find_if(spec.queries.begin(),spec.queries.end(),[&](const auto& q) { return q.id==name; });
            if(found==spec.queries.end()) fail(op+"/query","unknown query","IR_REF");
            out.query=static_cast<std::size_t>(found-spec.queries.begin());
        } else if(out.metric=="agent" || out.metric=="sum") {
            if(out.metric=="sum" && o.contains("state")) fail(op,"sum cannot select state");
            out.field=field_index(text(o,"field",op),op+"/field"); if(spec.fields[out.field].kind==des::FieldKind::string) fail(op+"/field","string output unsupported");
        } else if(out.metric=="state_count") {
            if(o.contains("field") || !spec.asynchronous) fail(op,"invalid state count");
            out.state=integer(get(o,"state",op),op+"/state"); if(std::find(spec.states.begin(),spec.states.end(),out.state)==spec.states.end()) fail(op+"/state","unknown state");
        } else if(out.metric!="active" || o.contains("field") || o.contains("state")) fail(op,"invalid output metric");
        spec.outputs.push_back(out);
    }
    model.typed_abm=std::move(spec);
}

std::vector<Row> run_typed_abm(const Model& model,const std::map<std::string,double>& parameters,std::uint64_t seed,std::uint32_t scenario,std::uint32_t replication) {
    try {
        if(!model.typed_abm) throw std::invalid_argument("missing typed population specification");
        const auto& spec=*model.typed_abm;
        const Queries queries(spec);
        const auto validate=[binding=spec.space,limit=spec.agent_limit](const AgentStore& s) { if(s.next_id()>limit) throw std::overflow_error("agent allocation limit exceeded"); if(binding) (void)abm::SpatialSnapshot<AgentTag>(s,*binding); };
        (void)rng::pack_counter({scenario,replication,0,0,0,0});
        AgentStore store(0,spec.fields); store.spawn_many(spec.agents);
        if(spec.network_generator) store.configure_network(generate_network(spec,parameters,{seed,scenario,replication,0}));
        else if(spec.network) store.configure_network(*spec.network);
        std::unique_ptr<Atomic> atomic;
        if(spec.asynchronous) {
            const auto definition=chart(spec,store,parameters); const Chart::Draws draws{seed,scenario,replication};
            Atomic::Async population(store,[definition,draws](const auto& t,const auto& s) { return definition.on_timer(t,s,draws); },100000,validate);
            for(std::uint64_t id=0;id<spec.agents.size();++id) population.apply(definition.start({0,id},population.store(),spec.initial,0,draws));
            atomic=std::make_unique<Atomic>(std::move(population),[definition,draws](const auto& c,const auto& s) { return definition.message(c.agent,s,c.kind,c.time,draws); });
            atomic->configure_births([definition,draws,initial=spec.initial](auto ref,const auto& current,double time) {
                const auto effects=definition.start(ref,current,initial,time,draws);
                Atomic::Async::Birth birth{effects.updates.at(0).second,{}};
                for(const auto& timer:effects.schedules) birth.timers.push_back({timer.time,timer.kind,timer.generation});
                return birth;
            });
        } else {
            Atomic::Sync population(store,validate);
            for(const auto& phase:spec.phases) {
                Atomic::Sync::Publisher publisher;
                if(!phase.publish.empty()) publisher=[entries=phase.publish,parameters,queries](auto r,const auto& s) {
                    return emissions(entries,r,s,values(s,r,parameters,queries));
                };
                Atomic::Sync::LifecycleRule lifecycle;
                if(phase.lifecycle.retire || !phase.lifecycle.births.empty()) lifecycle=[life=phase.lifecycle,parameters,queries](auto r,const auto& s) { return behavior(life,r,s,parameters,queries); };
                population.add_phase([assignments=phase.assignments,parameters,queries](auto r,const auto& s) { return assign(assignments,r,s,parameters,queries); },publisher,lifecycle);
            }
            atomic=std::make_unique<Atomic>(std::move(population),model.dt);
        }
        if(!spec.topics.empty()) {
            Atomic::TopicConfig config; config.budget=spec.delivery_budget;
            std::map<std::string,TypedAbm::Topic> topics;
            for(const auto& topic:spec.topics) { config.capacities.emplace(topic.id,topic.capacity); topics.emplace(topic.id,topic); }
            config.handler=[topics,parameters,queries](const Atomic::Delivery& delivery,const AgentStore& current) {
                const auto& topic=topics.at(delivery.topic);
                auto context=parameters; context.emplace("message_value",delivery.value); context.emplace("message_sender",static_cast<double>(delivery.sender.id));
                const auto environment=values(current,delivery.receiver,context,queries);
                Atomic::TopicEffects result;
                if(topic.guard && topic.guard->evaluate(environment)==0) return result;
                if(!topic.assignments.empty()) result.effects.updates.push_back({delivery.receiver,assign(topic.assignments,delivery.receiver,current,context,queries)});
                result.publications=emissions(topic.publish,delivery.receiver,current,environment,delivery.sender);
                return result;
            };
            atomic->configure_topics(std::move(config));
        }
        devs::Simulator<Atomic::Message> simulator; const auto id=simulator.add(std::move(atomic));
        for(const auto& change:spec.lifecycle) {
            Atomic::LifecycleInput input{change.time,change.sequence,{},change.births};
            for(auto ref:change.retire) input.retirements.push_back({0,ref});
            simulator.inject(change.time,id,3,std::move(input));
        }
        for(const auto& change:spec.network_updates) {
            Atomic::NetworkInput input{change.time,change.sequence,{},{}};
            for(const auto& [a,b]:change.add) input.additions.push_back({{0,a},{0,b}});
            for(const auto& [a,b]:change.remove) input.removals.push_back({{0,a},{0,b}});
            simulator.inject(change.time,id,4,std::move(input));
        }
        for(const auto& m:spec.messages) simulator.inject(m.time,id,0,Atomic::Command{m.time,m.sequence,{0,m.agent},m.event});
        for(const auto& publication:spec.publications) {
            std::optional<AgentStore::Reference> receiver;
            if(publication.receiver) receiver=AgentStore::Reference{0,*publication.receiver};
            simulator.inject(publication.time,id,2,Atomic::TopicInput{publication.time,publication.topic,{0,publication.sender},receiver,publication.sequence,publication.value});
        }
        std::vector<Row> rows;
        const auto steps=static_cast<std::size_t>(std::llround(model.horizon/model.dt));
        for(std::size_t step=0;step<=steps;++step) {
            const double time=step*model.dt;
            (void)simulator.run_until_transactional(time);
            const auto& current=dynamic_cast<const Atomic&>(simulator.model(id)).store();
            for(const auto& output:spec.outputs) {
                double value=0;
                if(output.metric=="alive") value=current.alive({0,output.agent}) ? 1 : 0;
                else if((output.metric=="agent" || output.metric=="query") && !current.alive({0,output.agent}) && output.inactive_value) value=*output.inactive_value;
                else if(output.metric=="agent") value=numeric(current.field({0,output.agent},output.field));
                else if(output.metric=="query") value=queries.evaluate(spec.queries.at(output.query),current,{0,output.agent});
                else if(output.metric=="active") value=static_cast<double>(current.active_count());
                else for(std::uint64_t agent=0;agent<current.next_id();++agent) if(current.alive({0,agent})) {
                    if(output.metric=="sum") {
                        value+=numeric(current.field({0,agent},output.field));
                        if(spec.fields[output.field].kind!=des::FieldKind::real && std::abs(value)>max_integer)
                            throw std::overflow_error("integer aggregate exceeds exact numeric range");
                    }
                    else value+=std::get<std::int64_t>(current.field({0,agent},spec.chart_fields[0]))==output.state;
                }
                if(!std::isfinite(value)) throw std::overflow_error("ABM output is nonfinite");
                rows.push_back({time,output.id,value});
            }
        }
        return rows;
    } catch(const Error&) { throw; } catch(const std::exception& e) { fail("/components/0",e.what(),"IR_ABM_RUNTIME"); }
}
} // namespace ankurafathom::ir
