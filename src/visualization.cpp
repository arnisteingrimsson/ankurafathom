#include "visualization.hpp"
#include "provenance.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace ankurafathom::ir::visualization {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
std::string html(std::string_view s) {
    std::string out;
    for(unsigned char c:s) {
        if(c=='&') out+="&amp;";else if(c=='<') out+="&lt;";else if(c=='>') out+="&gt;";
        else if(c=='\"') out+="&quot;";else if(c=='\'') out+="&#39;";
        else if(c<32) out+=' ';else out+=static_cast<char>(c);
    }return out;
}
std::string quote(std::string_view s) {
    std::string out="\"";
    for(unsigned char c:s) {
        if(c=='\\' || c=='\"') {out+='\\';out+=static_cast<char>(c);}
        else if(c=='\n') out+="\\n";else if(c<32) out+=' ';else out+=static_cast<char>(c);
    }return out+'\"';
}
std::string mermaid_label(std::string_view s) {
    std::string out;
    for(unsigned char c:s) {
        if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c==' ' || c=='_' || c>=128) out+=static_cast<char>(c);
        else out+="#"+std::to_string(c)+";";
    }return out;
}
struct Graph {
    std::map<std::string,Json> nodes;
    std::set<std::tuple<std::string,std::string,std::string>> edges;
    void node(const std::string& id,const std::string& name,const std::string& kind,Json declaration=nullptr) {
        nodes[id]={{"id",id},{"name",name},{"kind",kind},{"declaration",std::move(declaration)}};
    }
    void edge(const std::string& from,const std::string& to,const std::string& role) {edges.emplace(from,to,role);}
    Json json() const {
        Json n=Json::array(),e=Json::array();
        for(const auto& [id,node]:nodes) {(void)id;n.push_back(node);}
        for(const auto& [from,to,role]:edges) {
            if(!nodes.contains(from) || !nodes.contains(to)) throw Error("IR_VIZ_GRAPH","","unresolved graph endpoint");
            e.push_back({{"from",from},{"to",to},{"role",role}});
        }
        if(nodes.size()>2000 || edges.size()>10000) throw Error("IR_VIZ_LIMIT","","diagram exceeds 2000 nodes or 10000 edges; select an output for dependencies");
        return {{"nodes",n},{"edges",e}};
    }
    void focus(const std::string& root) {
        std::set<std::string> keep{root};std::vector<std::string> pending{root};
        for(std::size_t i=0;i<pending.size();++i)
            for(const auto& [from,to,role]:edges) { (void)role;if(to==pending[i] && keep.insert(from).second) pending.push_back(from); }
        std::erase_if(nodes,[&](const auto& p){return !keep.contains(p.first);});
        std::erase_if(edges,[&](const auto& e){return !keep.contains(std::get<0>(e)) || !keep.contains(std::get<1>(e));});
    }
};
std::pair<Graph,Graph> graphs(const Model& m,const std::string& output) {
    Graph stock,dependency;const auto doc=Json::parse(m.input.canonical_json);
    std::map<std::string,Json> declarations;
    for(const auto& c:doc.at("components")) declarations[c.at("id").get<std::string>()]=c;
    const auto symbol=[](const std::string& id){return "symbol:"+id;};
    dependency.node(symbol("t"),"t","time",{{"unit",m.time_unit}});
    dependency.node(symbol("dt"),"dt","time-step",{{"value",m.dt},{"unit",m.time_unit}});
    for(const auto& [id,value]:m.parameters) dependency.node(symbol(id),id,"parameter",{{"value",value},{"dimension",m.parameter_units.at(id).powers}});
    const auto links=[&](const std::string& id,const Expression& e,const std::string& role) {
        for(const auto& s:e.symbols()) dependency.edge(symbol(s),id,role);
        for(const auto& f:e.functions()) if(!Expression::builtin(f)) dependency.edge(symbol(f),id,"function");
    };
    for(const auto& s:m.stocks) {
        stock.node(symbol(s.id),s.id,"stock",declarations.at(s.id));
        dependency.node(symbol(s.id),s.id,"stock",declarations.at(s.id));
    }
    for(const auto& a:m.auxiliaries) {dependency.node(symbol(a.id),a.id,"auxiliary",declarations.at(a.id));links(symbol(a.id),a.expression,"expression-symbol");}
    for(const auto& d:m.delays) {
        dependency.node(symbol(d.id),d.id,"delay",declarations.at(d.id));links(symbol(d.id),d.input,"delay-input");
        if(const auto* e=std::get_if<Expression>(&d.duration)) links(symbol(d.id),*e,"delay-duration");
    }
    for(const auto& t:m.tables) dependency.node(symbol(t.id),t.id,"table",declarations.at(t.id));
    for(const auto& f:m.flows) {
        const auto id=symbol(f.id);stock.node(id,f.id,"flow",declarations.at(f.id));dependency.node(id,f.id,"flow",declarations.at(f.id));
        links(id,f.expression,"expression-symbol");
        const auto boundary=[&](const char* side) {
            const auto b="boundary:"+f.id+":"+side;stock.node(b,"external","boundary");return b;
        };
        stock.edge(f.source?symbol(*f.source):boundary("source"),id,"material-source");
        stock.edge(id,f.destination?symbol(*f.destination):boundary("destination"),"material-destination");
        if(f.source) dependency.edge(id,symbol(*f.source),"stock-outflow");
        if(f.destination) dependency.edge(id,symbol(*f.destination),"stock-inflow");
    }
    for(const auto& binding:m.parameter_data) {
        const auto id="binding:"+binding.id;Json key;std::visit([&](const auto& v){key=v;},binding.source_key);
        dependency.node(id,binding.id,"parameter-data",{{"source",binding.source},{"source_key",key},{"columns",binding.columns},
            {"file_sha256",binding.file_hash},{"canonical_sha256",binding.canonical_hash}});
        for(const auto& [parameter,column]:binding.columns) {(void)column;dependency.edge(id,symbol(parameter),"binds");}
    }
    // Preserve complete data receipts, including interpolation and content hashes.
    const auto receipts=provenance::inputs(m,std::nullopt,std::nullopt,0,1).at("data");
    for(const auto& binding:m.series_data) {
        const auto receipt=std::find_if(receipts.begin(),receipts.end(),[&](const auto& r){return r.at("binding").at("id")==binding.id;});
        dependency.node(symbol(binding.id),binding.id,"exogenous-series",*receipt);
    }
    for(std::size_t i=0;i<m.outputs.size();++i) {
        const auto& o=m.outputs[i];const auto id="output:"+o.id;
        dependency.node(id,o.id,"output",doc.at("outputs").at(i));
        if(o.expression) links(id,*o.expression,"expression-symbol");else dependency.edge(symbol(o.source),id,"observes");
    }
    if(!output.empty()) {
        if(!dependency.nodes.contains("output:"+output)) throw Error("IR_VIZ_OUTPUT","/output","unknown output id");
        dependency.focus("output:"+output);
    }
    return {stock,dependency};
}
void serialize(const Graph& graph,const std::string& title,const std::string& caption,const fs::path& stem) {
    std::string dot="digraph model {\ngraph [rankdir=LR, bgcolor=\"white\", fontname=\"Helvetica\", fontsize=12, labelloc=t, label="+quote(title+"\n"+caption)+"];\nnode [fontname=\"Helvetica\", fontsize=11, style=filled, fillcolor=\"#eef3f8\"];\nedge [fontname=\"Helvetica\", fontsize=9, color=\"#52657a\"];\n";
    std::string mermaid="flowchart LR\n  caption[\""+mermaid_label(title+" | "+caption)+"\"]\n";
    std::map<std::string,std::string> ids;std::size_t i=0;
    for(const auto& [id,node]:graph.nodes) {
        const auto name="n"+std::to_string(i++);ids[id]=name;
        const auto kind=node.at("kind").get<std::string>();
        auto label=node.at("name").get<std::string>()+"\n"+kind;
        if(node.at("declaration").is_object() && node.at("declaration").contains("unit")) label+=" ["+node.at("declaration").at("unit").get<std::string>()+"]";
        const std::string shape=kind=="stock"?"box":kind=="flow"?"diamond":kind=="boundary"?"oval":kind=="output"?"box":"ellipse";
        dot+=name+" [shape="+shape+", label="+quote(label)+"];\n";
        mermaid+="  "+name+"[\""+mermaid_label(label)+"\"]\n";
    }
    for(const auto& [from,to,role]:graph.edges) {
        dot+=ids.at(from)+" -> "+ids.at(to)+" [label="+quote(role)+"];\n";
        mermaid+="  "+ids.at(from)+" -->|"+role+"| "+ids.at(to)+"\n";
    }
    runtime::write_text_atomically(stem.string()+".dot",dot+"}\n");
    runtime::write_text_atomically(stem.string()+".mmd",mermaid);
}
void render(const fs::path& stem) {
    std::string input=stem.string()+".dot",output=stem.string()+".svg";
    char command[]="dot",format[]="-Tsvg",flag[]="-o";
    char* args[]={command,format,input.data(),flag,output.data(),nullptr};
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)!=0) throw Error("IR_VIZ_RENDER","/renderer","cannot initialize renderer actions");
    const auto log=stem.string()+".render.log";
    const int redirected=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,log.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    const int muted=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
    pid_t pid;const int spawned=(redirected || muted)?EINVAL:posix_spawnp(&pid,command,&actions,nullptr,args,environ);
    posix_spawn_file_actions_destroy(&actions);
    if(spawned) throw Error("IR_VIZ_RENDER","/renderer","cannot start Graphviz dot; install Graphviz and put dot on PATH");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);int status=0;
    while(true) {
        const auto result=waitpid(pid,&status,WNOHANG);
        if(result==pid) break;
        if(result<0 && errno!=EINTR) throw Error("IR_VIZ_RENDER","/renderer","cannot wait for Graphviz");
        if(std::chrono::steady_clock::now()>=deadline) {
            kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0 && errno==EINTR) {}
            throw Error("IR_VIZ_RENDER","/renderer","Graphviz exceeded 30-second layout limit");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(!WIFEXITED(status) || WEXITSTATUS(status)!=0 || !fs::is_regular_file(output) || fs::file_size(output)==0)
        throw Error("IR_VIZ_RENDER","/renderer","Graphviz did not produce an SVG");
    fs::remove(log);
}
struct Staging {
    fs::path path;
    ~Staging() {std::error_code ec;fs::remove_all(path,ec);}
};
}
int cli(int argc,char** argv) {
    std::string destination,output,manifest_path;std::set<std::string> seen;
    for(int i=3;i<argc;i+=2) {
        const std::string option=argv[i];
        if(!seen.insert(option).second || i+1>=argc || argv[i+1][0]=='\0' || std::string_view(argv[i+1]).starts_with("--"))
            throw Error("IR_USAGE","","viz option requires one value and cannot repeat");
        if(option=="--out") destination=argv[i+1];else if(option=="--output") output=argv[i+1];
        else if(option=="--manifest") manifest_path=argv[i+1];else throw Error("IR_USAGE","","unknown viz option");
    }
    if(destination.empty()) throw Error("IR_USAGE","","usage: fathom viz model.json --out new-directory [--output ID] [--manifest run.json]");
    const auto target=fs::absolute(destination).lexically_normal();
    if(fs::symlink_status(target).type()!=fs::file_type::not_found) throw Error("IR_VIZ_DESTINATION","/out","destination already exists");
    const auto model=load_file(argv[2]);
    if(model.kind!=Model::Kind::sd) throw Error("IR_VIZ_SCOPE","/mode","structure diagrams currently require standalone SD");
    Json manifest_id=nullptr;
    if(!manifest_path.empty()) {
        const auto manifest=provenance::read_manifest(manifest_path);
        if(manifest.at("manifest_version")=="0.3") throw Error("IR_VIZ_SCOPE","/manifest","file-backed manifest required");
        const auto& input=manifest.at("inputs");std::optional<runtime::Experiment> experiment;std::optional<InputIdentity> identity;
        const auto seed=input.at("execution").at("seed").get<std::uint64_t>();
        if(!input.at("experiment").is_null()) {
            identity.emplace();const auto doc=provenance::read_input(input.at("experiment").at("path").get<std::string>(),*identity,"/experiment");
            experiment=load_experiment_json(doc.dump(),model,seed);
        }
        provenance::require_equal(provenance::inputs(model,experiment,identity,seed,input.at("execution").at("threads").get<std::size_t>()),input,"/inputs");
        manifest_id=manifest.at("id");
    }
    const auto [stock,dependency]=graphs(model,output);
    const std::string caveat="Static declarations; dependency arrows are not causal effects. Flow arrows follow declared endpoints; signed rates may reverse transport.";
    Json report={{"visualization_version","0.1"},{"scope","standalone-sd-static-structure"},{"model_name",model.name},
        {"model_sha256",model.input.canonical_hash},{"manifest_id",manifest_id},{"selected_output",output.empty()?Json(nullptr):Json(output)},
        {"verification",{{"inputs",manifest_id.is_null()?"model-loaded":"exact-manifest-match"},{"results","not-verified"}}},
        {"stock_flow",stock.json()},{"dependencies",dependency.json()},
        {"limitations",Json::array({caveat,"All syntactic branches are included. No feedback polarity, delay-history/clipping-allocation expansion or scenario values are inferred.",
            "Stock-flow view contains all stocks and flows. Output selection filters only the dependency view."})}};
    auto pattern=(target.parent_path()/".fathom-viz-XXXXXX").string();
    const auto created=mkdtemp(pattern.data());if(!created) throw Error("IR_IO","/out","cannot create staging directory; parent must exist");
    Staging staging{created};
    const std::string caption="Model SHA256: "+model.input.canonical_hash+"\n"+
        (manifest_id.is_null()?"No run selected; static structure":"Manifest: "+manifest_id.get<std::string>()+"\nInputs verified; results not verified")+
        "\nDeclared endpoints; signed rates may reverse transport. Dependencies are not causal effects.";
    serialize(stock,model.name+" - stock and flow",caption,staging.path/"stock-flow");
    serialize(dependency,model.name+" - dependencies"+(output.empty()?"":" for "+output),caption,staging.path/"dependencies");
    render(staging.path/"stock-flow");render(staging.path/"dependencies");
    runtime::write_text_atomically(staging.path/"graph.json",report.dump(2)+"\n");
    const std::string page="<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><title>"+html(model.name)+
        " - structure</title><style>body{font:16px system-ui;color:#18334b;max-width:1400px;margin:32px auto;padding:0 20px}img{max-width:100%;height:auto}code{overflow-wrap:anywhere}p{line-height:1.5}</style><h1>"+html(model.name)+
        "</h1><p>"+html(caveat)+"</p><p><code>"+html(caption)+"</code></p><p><a href=\"graph.json\">Structured graph and declarations</a></p><h2>Stock and flow</h2><p>Rectangles: stocks. Diamonds: flow rates. Ovals: external boundaries.</p><a href=\"stock-flow.svg\"><img src=\"stock-flow.svg\" alt=\"Stock and flow diagram\"></a><h2>Dependencies"+
        (output.empty()?"":" for "+html(output))+"</h2><p>Arrows point from a referenced value to its consumer; stock-inflow/outflow edges represent stock updates. Cycles are retained. "
        "Parameter values and formulas in graph.json are base declarations, not scenario evaluations.</p><a href=\"dependencies.svg\"><img src=\"dependencies.svg\" alt=\"Dependency diagram\"></a></html>\n";
    runtime::write_text_atomically(staging.path/"index.html",page);
    // Reserve the destination exclusively, including against concurrent writers.
    if(!fs::create_directory(target)) throw Error("IR_VIZ_DESTINATION","/out","destination already exists");
    try {for(const auto& entry:fs::directory_iterator(staging.path)) fs::rename(entry.path(),target/entry.path().filename());}
    catch(...) {std::error_code ec;fs::remove_all(target,ec);throw;}
    std::cout<<report.dump()<<'\n';if(!std::cout) throw Error("IR_IO","","cannot write visualization receipt");return 0;
}
}
