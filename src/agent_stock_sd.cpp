#include "agent_stock_sd.hpp"
#include "ankurafathom/hybrid/agent_stocks_atomic.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"

namespace ankurafathom::ir {
namespace {
using Json=nlohmann::json;
using Spec=AgentStockSd;
using Store=Spec::Store;
using Core=hybrid::AgentStocks<ContinuousAgentTag>;
using Atomic=hybrid::AgentStocksAtomic<ContinuousAgentTag>;
using Aggregate=hybrid::PopulationAggregate<ContinuousAgentTag>;
using Reduction=Aggregate::Reduction;
using Message=hybrid::AggregateMessage<ContinuousAgentTag>;
using SD=hybrid::SignalSD<Message>;
[[noreturn]] void fail(const std::string& p,const std::string& message,const std::string& code="IR_AGENT_STOCK_SD") {
    throw Error(code,p,message);
}
std::string escape(const std::string& s) {
    std::string result;for(char c:s) result+=c=='~' ? "~0" : c=='/' ? "~1" : std::string(1,c);return result;
}
void keys(const Json& j,std::initializer_list<const char*> allowed,const std::string& p) {
    if(!j.is_object()) fail(p,"expected object","IR_TYPE");
    for(auto i=j.begin();i!=j.end();++i)
        if(std::none_of(allowed.begin(),allowed.end(),[&](auto key) { return i.key()==key; }))
            fail(p+"/"+escape(i.key()),"unknown field","IR_FIELD");
}
const Json& get(const Json& j,const std::string& key,const std::string& p) {
    if(!j.contains(key)) fail(p+"/"+key,"missing required field","IR_MISSING");return j.at(key);
}
std::string text(const Json& j,const std::string& key,const std::string& p) {
    const auto& v=get(j,key,p);
    if(!v.is_string() || v.get<std::string>().empty()) fail(p+"/"+key,"expected nonempty string","IR_TYPE");
    return v.get<std::string>();
}
std::string identifier(const Json& j,const std::string& key,const std::string& p) {
    auto id=text(j,key,p);
    if((!std::isalpha(static_cast<unsigned char>(id[0])) && id[0]!='_') || id=="t" || id=="dt" || Expression::builtin(id) ||
       std::any_of(id.begin(),id.end(),[](unsigned char c) { return !std::isalnum(c) && c!='_'; }))
        fail(p+"/"+key,"invalid or reserved identifier","IR_ID");
    return id;
}
std::string claim(const Json& j,const std::string& p,std::set<std::string>& ids) {
    const auto id=identifier(j,"id",p);
    if(!ids.insert(id).second) fail(p+"/id","duplicate identifier","IR_ID");return id;
}
double number(const Json& j,const std::string& p) {
    if(!j.is_number() || !std::isfinite(j.get<double>())) fail(p,"expected finite number","IR_TYPE");
    return j.get<double>();
}
bool flag(const Json& j,const std::string& key,const std::string& p,bool fallback=true) {
    if(!j.contains(key)) return fallback;
    if(!j.at(key).is_boolean()) fail(p+"/"+key,"expected boolean","IR_TYPE");return j.at(key).get<bool>();
}
const Json& array(const Json& j,const std::string& key,const std::string& p) {
    const auto& value=get(j,key,p);if(!value.is_array()) fail(p+"/"+key,"expected array","IR_TYPE");return value;
}
Dimension unit(const Json& j,const std::string& p) {
    try { return Dimension::parse(text(j,"unit",p)); }
    catch(const std::invalid_argument& e) { fail(p+"/unit",e.what(),"IR_UNIT"); }
}
Expression expression(const Json& j,const std::string& key,const std::string& p,
                      const std::map<std::string,Dimension>& symbols,Dimension expected) {
    try {
        Expression result(text(j,key,p));
        for(const auto& f:result.functions()) if(f!="NONNEGATIVE") fail(p+"/"+key,"unsupported expression function","IR_FUNCTION");
        for(const auto& s:result.symbols()) if(!symbols.contains(s)) fail(p+"/"+key,"unknown or nonnumeric symbol: "+s,"IR_SYMBOL");
        if(result.infer_unit(symbols)!=expected) fail(p+"/"+key,"expression dimension differs from declaration","IR_UNIT");
        return result;
    } catch(const std::invalid_argument& e) { fail(p+"/"+key,e.what(),"IR_EXPRESSION"); }
}
double numeric(const Store::Value& v) {
    if(const auto* x=std::get_if<double>(&v)) return *x;
    if(const auto* x=std::get_if<std::int64_t>(&v)) {
        if(*x < -9007199254740991LL || *x > 9007199254740991LL) throw std::overflow_error("integer is outside exact expression range");
        return static_cast<double>(*x);
    }
    if(const auto* x=std::get_if<bool>(&v)) return *x ? 1 : 0;
    throw std::invalid_argument("string is not an expression value");
}
std::map<std::string,double> agent_values(Store::Reference ref,const Store& store,const std::map<std::string,double>& params) {
    auto values=params;
    for(std::size_t i=0;i<store.schema().size();++i)
        if(store.schema()[i].kind!=des::FieldKind::string) values.emplace(store.schema()[i].name,numeric(store.field(ref,i)));
    return values;
}
double evaluate(const Expression& e,const std::map<std::string,double>& values,const std::string& pointer) {
    try { return e.evaluate(values); }
    catch(const std::exception& error) { fail(pointer,error.what(),"IR_AGENT_STOCK_SD_RUNTIME"); }
}
Reduction::Kind reduction_kind(const std::string& op) {
    if(op=="sum") return Reduction::Kind::sum;
    if(op=="mean") return Reduction::Kind::mean;
    if(op=="count") return Reduction::Kind::count;
    if(op=="min") return Reduction::Kind::minimum;
    if(op=="max") return Reduction::Kind::maximum;
    throw std::invalid_argument("unknown aggregate operation");
}
} // namespace

void parse_agent_stock_sd(const Json& document,Model& model,std::set<std::string>& ids) {
    if(document.contains("links")) fail("/links","this subset uses named population/aggregate/stock bindings","IR_FIELD");
    Spec spec;
    const auto time_unit=Dimension::parse(model.time_unit);
    const auto& components=array(document,"components","");
    std::vector<std::string> names,kinds;
    std::optional<std::size_t> population;
    // Claim every component before resolving references, independent of order.
    for(std::size_t i=0;i<components.size();++i) {
        const auto p="/components/"+std::to_string(i);const auto& c=components[i];
        const auto kind=text(c,"kind",p);
        if(kind=="population") {
            keys(c,{"id","kind","execution","fields","agents","dt"},p);
            if(population) fail(p,"exactly one continuous population is supported");
            population=i;
        } else if(kind=="agent_stock") keys(c,{"id","kind","population","field","inflow_expr","outflow_expr","non_negative"},p);
        else if(kind=="aggregate") keys(c,{"id","kind","population","op","expr","filter","empty_value","unit"},p);
        else if(kind=="stock") keys(c,{"id","kind","init","unit","non_negative"},p);
        else if(kind=="flow") keys(c,{"id","kind","source","destination","expr","unit","non_negative"},p);
        else fail(p+"/kind","unsupported component kind");
        names.push_back(claim(c,p,ids));kinds.push_back(kind);
    }
    if(!population) fail("/components","one continuous population is required");
    const auto& pop=components[*population];const auto pp="/components/"+std::to_string(*population);
    spec.population_id=names[*population];
    if(text(pop,"execution",pp)!="continuous") fail(pp+"/execution","expected continuous execution");
    spec.population_dt=pop.contains("dt") ? number(pop.at("dt"),pp+"/dt") : model.dt;
    if(spec.population_dt<=0 || !std::isfinite(model.horizon/spec.population_dt) || model.horizon/spec.population_dt>1000000)
        fail(pp+"/dt","population step must be positive, with at most one million steps","IR_TIME");
    const auto& fields=array(pop,"fields",pp);
    if(fields.empty()) fail(pp+"/fields","population requires fields");
    std::map<std::string,std::size_t> field_index;
    auto field_symbols=model.parameter_units;
    for(std::size_t i=0;i<fields.size();++i) {
        const auto p=pp+"/fields/"+std::to_string(i);const auto& f=fields[i];
        keys(f,{"name","type","unit"},p);
        const auto name=identifier(f,"name",p),type=text(f,"type",p);
        if(model.parameters.contains(name) || !field_index.emplace(name,i).second) fail(p+"/name","duplicate field or parameter shadowing","IR_ID");
        const auto dimension=unit(f,p);
        des::FieldKind kind;
        if(type=="real") kind=des::FieldKind::real;
        else if(type=="integer") kind=des::FieldKind::integer;
        else if(type=="boolean") kind=des::FieldKind::boolean;
        else if(type=="string") kind=des::FieldKind::string;
        else fail(p+"/type","unsupported field type","IR_TYPE");
        if((kind==des::FieldKind::boolean || kind==des::FieldKind::string) && dimension!=Dimension{})
            fail(p+"/unit","boolean and string fields must be dimensionless","IR_UNIT");
        spec.fields.push_back({name,kind});spec.field_units.emplace(name,dimension);
        if(kind!=des::FieldKind::string) field_symbols.emplace(name,dimension);
    }
    const auto& agents=array(pop,"agents",pp);
    if(agents.size()>1000000) fail(pp+"/agents","population exceeds one million records");
    for(std::size_t i=0;i<agents.size();++i) {
        const auto p=pp+"/agents/"+std::to_string(i);const auto& agent=agents[i];
        if(!agent.is_object() || agent.size()!=spec.fields.size()) fail(p,"record must match field schema","IR_TYPE");
        Store::Record record;
        for(const auto& f:spec.fields) {
            const auto fp=p+"/"+escape(f.name);const auto& v=get(agent,f.name,p);
            switch(f.kind) {
                case des::FieldKind::real: record.emplace_back(number(v,fp));break;
                case des::FieldKind::integer: {
                    const auto value=number(v,fp);
                    if(std::floor(value)!=value || std::abs(value)>9007199254740991.) fail(fp,"integer exceeds exact numeric range","IR_TYPE");
                    record.emplace_back(static_cast<std::int64_t>(value));break;
                }
                case des::FieldKind::boolean:
                    if(!v.is_boolean()) fail(fp,"expected boolean","IR_TYPE");record.emplace_back(v.get<bool>());break;
                case des::FieldKind::string:
                    if(!v.is_string()) fail(fp,"expected string","IR_TYPE");record.emplace_back(v.get<std::string>());break;
            }
        }
        spec.agents.push_back(std::move(record));
    }
    auto agent_symbols=field_symbols;
    agent_symbols.emplace("t",time_unit);agent_symbols.emplace("dt",time_unit);
    auto scalar_symbols=model.parameter_units;
    scalar_symbols.emplace("t",time_unit);scalar_symbols.emplace("dt",time_unit);
    std::set<std::size_t> bound_fields;
    std::map<std::string,Dimension> stock_units;
    for(std::size_t i=0;i<components.size();++i) {
        const auto& c=components[i];const auto p="/components/"+std::to_string(i);
        if(kinds[i]=="agent_stock") {
            if(text(c,"population",p)!=spec.population_id) fail(p+"/population","unknown continuous population");
            const auto name=text(c,"field",p);const auto found=field_index.find(name);
            if(found==field_index.end() || spec.fields[found->second].kind!=des::FieldKind::real)
                fail(p+"/field","agent stock requires a real population field");
            if(!bound_fields.insert(found->second).second) fail(p+"/field","field already bound to an agent stock");
            Spec::FieldStock stock{found->second,flag(c,"non_negative",p),{}, {},p};
            const auto rate_unit=spec.field_units.at(name).divided(time_unit);
            if(c.contains("inflow_expr")) stock.inflow=expression(c,"inflow_expr",p,agent_symbols,rate_unit);
            if(c.contains("outflow_expr")) stock.outflow=expression(c,"outflow_expr",p,agent_symbols,rate_unit);
            if(!stock.inflow && !stock.outflow) fail(p,"agent stock requires an inflow or outflow expression");
            for(const auto& record:spec.agents)
                if(stock.non_negative && std::get<double>(record[stock.field])<0) fail(p+"/field","negative initial value for nonnegative agent stock");
            spec.agent_stocks.push_back(std::move(stock));
        } else if(kinds[i]=="aggregate") {
            if(text(c,"population",p)!=spec.population_id) fail(p+"/population","unknown aggregate population");
            const auto op=text(c,"op",p);
            if(op!="sum" && op!="mean" && op!="count" && op!="min" && op!="max") fail(p+"/op","unsupported aggregate operation");
            Spec::Aggregate aggregate{names[i],op,p,{}, {}, {},unit(c,p)};
            if(op=="count") {
                if(c.contains("expr")) fail(p+"/expr","count has no projection","IR_FIELD");
                if(aggregate.unit!=Dimension{}) fail(p+"/unit","count must be dimensionless","IR_UNIT");
            } else aggregate.expression=expression(c,"expr",p,field_symbols,aggregate.unit);
            if(c.contains("filter")) aggregate.filter=expression(c,"filter",p,field_symbols,Dimension{});
            if(c.contains("empty_value")) {
                if(op=="sum" || op=="count") fail(p+"/empty_value","sum/count have fixed zero empty values","IR_FIELD");
                aggregate.empty=number(c.at("empty_value"),p+"/empty_value");
            }
            scalar_symbols.emplace(names[i],aggregate.unit);
            spec.aggregates.push_back(std::move(aggregate));
        } else if(kinds[i]=="stock") {
            const auto dimension=unit(c,p);
            const auto initial=number(get(c,"init",p),p+"/init");const auto nonnegative=flag(c,"non_negative",p);
            if(nonnegative && initial<0) fail(p+"/init","negative initial stock value");
            model.stocks.push_back({names[i],initial,nonnegative,text(c,"unit",p)});
            stock_units.emplace(names[i],dimension);scalar_symbols.emplace(names[i],dimension);
        }
    }
    if(spec.agent_stocks.empty() || spec.aggregates.empty() || model.stocks.empty())
        fail("/components","agent_stock_sd requires an agent stock, aggregate and SD stock");
    for(std::size_t i=0;i<components.size();++i) if(kinds[i]=="flow") {
        const auto& c=components[i];const auto p="/components/"+std::to_string(i);const auto dimension=unit(c,p);
        std::optional<std::string> source,destination;
        for(const auto* key:{"source","destination"}) if(c.contains(key) && !c.at(key).is_null()) {
            const auto id=text(c,key,p);const auto found=stock_units.find(id);
            if(found==stock_units.end()) fail(p+"/"+key,"flow endpoint is not an SD stock");
            if(found->second.divided(time_unit)!=dimension) fail(p+"/unit","flow and stock/time dimensions differ","IR_UNIT");
            (std::string(key)=="source" ? source : destination)=id;
        }
        if(!source && !destination) fail(p,"flow requires a stock endpoint");
        model.flows.push_back({names[i],i,source,destination,expression(c,"expr",p,scalar_symbols,dimension),
                               text(c,"unit",p),flag(c,"non_negative",p)});
    }
    const auto& outputs=array(document,"outputs","");
    if(outputs.empty()) fail("/outputs","at least one output is required");
    for(std::size_t i=0;i<outputs.size();++i) {
        const auto p="/outputs/"+std::to_string(i);const auto& o=outputs[i];
        keys(o,{"id","expr","unit"},p);const auto id=claim(o,p,ids);
        spec.outputs.push_back({id,p,expression(o,"expr",p,scalar_symbols,unit(o,p))});
    }
    model.agent_stock_sd=std::move(spec);
}

std::vector<Row> run_agent_stock_sd(const Model& model,const std::map<std::string,double>& parameters) {
    if(!model.agent_stock_sd) fail("","missing continuous-agent specification");
    const auto& spec=*model.agent_stock_sd;
    try {
        Store store(0,spec.fields);store.spawn_many(spec.agents);
        std::vector<Core::Field> fields;
        for(const auto& binding:spec.agent_stocks) fields.push_back({binding.field,binding.non_negative});
        Core core(std::move(store),std::move(fields));
        for(const auto& binding:spec.agent_stocks) {
            const auto bind=[&](const Expression& e,const std::string& path,bool inflow) {
                auto rate=[e,parameters,dt=spec.population_dt,path](auto ref,const Store& snapshot,const auto&,double time) {
                    auto values=agent_values(ref,snapshot,parameters);values.emplace("t",time);values.emplace("dt",dt);
                    const double rate=evaluate(e,values,path);
                    if(rate<0) fail(path,"agent inflow/outflow must be nonnegative","IR_AGENT_STOCK_SD_RUNTIME");
                    return rate;
                };
                const auto agent=Core::Endpoint::agent(binding.field),boundary=Core::Endpoint::boundary();
                core.add_agent_flow(inflow ? boundary : agent,inflow ? agent : boundary,std::move(rate));
            };
            if(binding.inflow) bind(*binding.inflow,binding.pointer+"/inflow_expr",true);
            if(binding.outflow) bind(*binding.outflow,binding.pointer+"/outflow_expr",false);
        }
        std::vector<Reduction> reducers;
        std::vector<std::string> aggregate_names,stock_names;
        for(const auto& aggregate:spec.aggregates) {
            Reduction::Projection projection;Reduction::Predicate filter;
            if(aggregate.expression)
                projection=[e=*aggregate.expression,parameters,p=aggregate.pointer+"/expr"](auto ref,const Store& s) {
                    return evaluate(e,agent_values(ref,s,parameters),p);
                };
            if(aggregate.filter)
                filter=[e=*aggregate.filter,parameters,p=aggregate.pointer+"/filter"](auto ref,const Store& s) {
                    const double value=evaluate(e,agent_values(ref,s,parameters),p);
                    if(value!=0 && value!=1) fail(p,"filter must evaluate to zero or one","IR_AGENT_STOCK_SD_RUNTIME");
                    return value==1;
                };
            reducers.emplace_back(aggregate.id,reduction_kind(aggregate.op),std::move(projection),std::move(filter),aggregate.empty);
            aggregate_names.push_back(aggregate.id);
        }
        auto aggregate=std::make_unique<Aggregate>(core.store(),std::move(reducers));
        std::vector<SD::Stock> stocks;std::map<std::string,std::size_t> indexes;
        for(const auto& stock:model.stocks) {
            indexes.emplace(stock.id,stocks.size());stocks.push_back({stock.id,stock.initial,stock.non_negative});stock_names.push_back(stock.id);
        }
        const auto values=[parameters,aggregate_names,stock_names,dt=model.dt](const auto& state,const auto& signals,double time) {
            auto result=parameters;result.emplace("t",time);result.emplace("dt",dt);
            for(std::size_t i=0;i<stock_names.size();++i) result.emplace(stock_names[i],state.at(i));
            for(std::size_t i=0;i<aggregate_names.size();++i) result.emplace(aggregate_names[i],signals.at(i));
            return result;
        };
        std::vector<SD::Flow> flows;
        for(const auto& flow:model.flows)
            flows.push_back({flow.source ? indexes.at(*flow.source) : SD::boundary,
                flow.destination ? indexes.at(*flow.destination) : SD::boundary,
                [expression=flow.expression,values,p="/components/"+std::to_string(flow.component_index)+"/expr"](const auto& state,const auto& signals,double time) {
                    return evaluate(expression,values(state,signals,time),p);
                },flow.non_negative});
        devs::Simulator<Message> simulator;
        const auto source_id=simulator.add(std::make_unique<Atomic>(std::move(core),spec.population_dt));
        const auto aggregate_id=simulator.add(std::move(aggregate));
        const auto sd_id=simulator.add(std::make_unique<SD>(std::move(stocks),std::move(flows),std::vector<double>(spec.aggregates.size(),0),model.dt));
        simulator.connect(source_id,1,aggregate_id,0);simulator.connect(aggregate_id,1,sd_id,0);
        const auto steps=static_cast<std::size_t>(std::llround(model.horizon/model.dt));
        std::vector<Row> rows;rows.reserve((steps+1)*spec.outputs.size());
        for(std::size_t tick=0;tick<=steps;++tick) {
            const double time=tick*model.dt;
            (void)simulator.run_until_transactional(time,10000000);
            const auto& sd=dynamic_cast<const SD&>(simulator.model(sd_id));
            const auto state=values(sd.state(),sd.signals(),time);
            for(const auto& output:spec.outputs) rows.push_back({time,output.id,evaluate(output.expression,state,output.pointer+"/expr")});
        }
        return rows;
    } catch(const Error&) { throw; }
    catch(const std::exception& error) { fail("",error.what(),"IR_AGENT_STOCK_SD_RUNTIME"); }
}
} // namespace ankurafathom::ir
