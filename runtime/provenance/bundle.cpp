#include "provenance.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/sha256.hpp"
#include <fstream>
#include <set>

namespace ankurafathom::ir::provenance {
namespace {
namespace fs=std::filesystem;
constexpr std::size_t member_limit=256*1024*1024,total_limit=1024*1024*1024;
[[noreturn]] void invalid(const std::string& message) { throw Error("IR_BUNDLE","/bundle",message); }
std::string hash(std::string_view bytes) { runtime::Sha256 h;h.update(bytes);return h.digest(); }
void keys(const Json& value,std::initializer_list<const char*> names) {
    if(!value.is_object() || value.size()!=names.size()) invalid("invalid bundle object");
    for(const auto* name:names) if(!value.contains(name)) invalid("missing bundle field");
}
std::string read_bytes(const fs::path& path,std::size_t& total,bool member) {
    const auto status=member?fs::symlink_status(path):fs::status(path);
    if(!fs::is_regular_file(status)) invalid("input must be a regular file: "+path.string());
    const auto size=fs::file_size(path);
    if(size>member_limit || size>total_limit-total) invalid("bundle byte limit exceeded");
    std::ifstream input(path,std::ios::binary);
    if(!input) invalid("cannot read bundle input: "+path.string());
    std::string bytes;char block[65536];
    while(input.read(block,sizeof(block)) || input.gcount()) {
        const auto count=static_cast<std::size_t>(input.gcount());
        if(count>member_limit-bytes.size() || count>total_limit-total) invalid("bundle byte limit exceeded");
        bytes.append(block,count);total+=count;
    }
    if(!input.eof()) invalid("cannot finish reading bundle input");
    return bytes;
}
Json parse(const std::string& bytes) {
    InputIdentity unused;
    return parse_input(bytes,unused,"/bundle");
}
void file_version(const Json& manifest) {
    if(manifest.at("manifest_version")=="0.3")
        throw Error("IR_REPLAY_UNSUPPORTED","/manifest/manifest_version","bundles require file-backed version 0.1 or 0.2 manifests");
}
std::string data_name(std::size_t index,const Json& data) {
    keys(data,{"binding","path","file_sha256","canonical_sha256"});
    const auto path=data.at("path").get<std::string>();
    if(path.find('\0')!=std::string::npos || !fs::path(path).is_absolute()) invalid("data path must be absolute");
    const auto suffix=fs::path(path).extension().string();
    if(suffix!=".csv" && suffix!=".parquet" && suffix!=".arrow" && suffix!=".ipc") invalid("unsupported bundled table suffix");
    return "data/"+std::to_string(index)+suffix;
}
std::set<std::string> members(const Json& manifest) {
    std::set<std::string> names{"bundle.json","model.json"};
    const auto& input=manifest.at("inputs");
    if(!input.at("experiment").is_null()) names.insert("experiment.json");
    const auto& data=input.at("data");
    if(data.size()>1024) invalid("too many bundle data inputs");
    for(std::size_t i=0;i<data.size();++i) names.insert(data_name(i,data[i]));
    return names;
}
InputIdentity original_identity(const std::string& bytes,const Json& expected) {
    InputIdentity actual;
    (void)parse_input(bytes,actual,"/bundle");
    if(actual.file_hash!=expected.at("file_sha256") || actual.canonical_hash!=expected.at("canonical_sha256") ||
       actual.canonical_json!=expected.at("canonical_json")) invalid("bundled JSON identity differs from manifest");
    actual.path=expected.at("path").get<std::string>();
    return actual;
}
Bundle load(const std::string& directory,bool require_build) {
    // Resolve a real directory root; never follow a symlink supplied as the root.
    auto requested=fs::absolute(directory).lexically_normal();
    if(requested.filename().empty()) requested=requested.parent_path();
    if(!fs::is_directory(fs::symlink_status(requested))) invalid("bundle root must be a real directory");
    const auto root=fs::canonical(requested);
    std::size_t total=0;
    const auto envelope=parse(read_bytes(root/"bundle.json",total,true));
    keys(envelope,{"bundle_version","manifest"});
    if(envelope.at("bundle_version")!="0.1") invalid("unsupported bundle version");
    auto manifest=validate_manifest(envelope.at("manifest"));file_version(manifest);
    const auto names=members(manifest);
    std::set<std::string> found;
    std::size_t entries=0;
    for(const auto& entry:fs::recursive_directory_iterator(root)) {
        if(++entries>names.size()+1) invalid("unexpected bundle entries");
        const auto relative=entry.path().lexically_relative(root).generic_string();
        const auto status=entry.symlink_status();
        if(relative=="data" && !manifest.at("inputs").at("data").empty() && fs::is_directory(status)) continue;
        if(!fs::is_regular_file(status) || !names.contains(relative)) invalid("unexpected or non-regular bundle member: "+relative);
        found.insert(relative);
    }
    if(found!=names) invalid("missing bundle members");
    const auto& input=manifest.at("inputs");
    const auto model_bytes=read_bytes(root/"model.json",total,true);
    auto original=original_identity(model_bytes,input.at("model"));
    auto document=Json::parse(original.canonical_json);
    const auto& data=input.at("data");
    if(document.contains("data") && !document.at("data").is_array()) invalid("invalid model data declarations");
    if((document.contains("data")?document.at("data").size():0)!=data.size()) invalid("data binding inventory differs");
    std::map<std::string,std::string> original_sources;
    for(std::size_t i=0;i<data.size();++i) {
        const auto& item=data[i];const auto& binding=item.at("binding");
        const auto id=binding.at("id").get<std::string>();
        if(!original_sources.emplace(id,item.at("path").get<std::string>()).second) invalid("duplicate bundle binding");
        bool matched=false;
        for(auto& declaration:document.at("data")) if(declaration.at("id")==id) {
            if(matched || declaration!=binding) invalid("bundle binding differs from captured model");
            const auto resolved=fs::path(original.path).parent_path()/binding.at("source").get<std::string>();
            if(resolved.string()!=item.at("path")) invalid("recorded data path differs from model resolution");
            const auto name=data_name(i,item);
            const auto bytes=read_bytes(root/name,total,true);
            if(hash(bytes)!=item.at("file_sha256")) invalid("bundled table bytes differ from manifest");
            declaration["source"]=(root/name).string();matched=true;
        }
        if(!matched) invalid("bundle binding missing from model");
    }
    auto model=load_json(document.dump(),&root);
    // Only physical resolution changes. Restore the validated original identity
    // for receipt comparison and original-ID result publication. Table identities
    // below still come from the actual immutable snapshots loaded from the bundle.
    model.input=std::move(original);
    for(auto& binding:model.parameter_data) binding.source=original_sources.at(binding.id);
    for(auto& binding:model.series_data) binding.source=original_sources.at(binding.id);
    if(model.population_data) model.population_data->source=original_sources.at(model.population_data->id);
    std::optional<runtime::Experiment> experiment;
    std::optional<InputIdentity> experiment_input;
    const auto seed=input.at("execution").at("seed").get<std::uint64_t>();
    if(!input.at("experiment").is_null()) {
        const auto bytes=read_bytes(root/"experiment.json",total,true);
        experiment_input=original_identity(bytes,input.at("experiment"));
        experiment=load_experiment_json(bytes,model,seed);
    }
    auto actual=inputs(model,experiment,experiment_input,seed,input.at("execution").at("threads").get<std::size_t>());
    // Packing is input capture, not an assertion that this host can replay an old
    // build. Replay always requires the unmodified current build policy.
    if(!require_build) actual["execution"]["build"]=input.at("execution").at("build");
    require_equal(actual,input,"/inputs");
    std::vector<std::string> files;
    for(const auto& name:names) files.push_back((root/name).string());
    return {std::move(manifest),std::move(model),std::move(experiment),std::move(experiment_input),root,std::move(files)};
}
}
Bundle load_bundle(const std::string& directory,bool require_build) {
    try { return load(directory,require_build); }
    catch(const Json::exception& e) { throw Error("IR_BUNDLE","/bundle",e.what()); }
    catch(const fs::filesystem_error& e) { throw Error("IR_BUNDLE","/bundle",e.what()); }
}
bool within_bundle(const std::string& path,const fs::path& root) {
    const auto relative=fs::weakly_canonical(path).lexically_relative(root);
    return !relative.empty() && *relative.begin()!="..";
}
Json create_bundle(const std::string& manifest_path,const std::string& directory) {
    try {
        const auto manifest=read_manifest(manifest_path);file_version(manifest);
        (void)members(manifest);
        const auto destination=fs::absolute(directory).lexically_normal();
        if(destination.filename().empty() || fs::exists(fs::symlink_status(destination))) invalid("bundle destination must be new");
        auto pattern=(destination.parent_path()/".fathom-bundle-XXXXXX").string();
        std::vector<char> name(pattern.begin(),pattern.end());name.push_back('\0');
        if(!::mkdtemp(name.data())) throw Error("IR_IO","/bundle","cannot create staging directory");
        struct Staging {
            fs::path path;
            bool committed=false;
            ~Staging() { if(!committed) { std::error_code ignored;fs::remove_all(path,ignored); } }
        } staging{fs::path(name.data())};
        std::size_t total=0;
        const auto copy=[&](const Json& input,const std::string& target) {
            const auto bytes=read_bytes(input.at("path").get<std::string>(),total,false);
            if(hash(bytes)!=input.at("file_sha256")) invalid("source bytes changed since recorded run");
            runtime::write_text_atomically(staging.path/target,bytes);
        };
        const auto& input=manifest.at("inputs");
        copy(input.at("model"),"model.json");
        if(!input.at("experiment").is_null()) copy(input.at("experiment"),"experiment.json");
        if(!input.at("data").empty()) fs::create_directory(staging.path/"data");
        for(std::size_t i=0;i<input.at("data").size();++i) copy(input.at("data")[i],data_name(i,input.at("data")[i]));
        runtime::write_text_atomically(staging.path/"bundle.json",Json{{"bundle_version","0.1"},{"manifest",manifest}}.dump(2)+"\n");
        (void)load_bundle(staging.path.string(),false);
        if(fs::exists(fs::symlink_status(destination))) invalid("bundle destination appeared during capture");
        fs::rename(staging.path,destination);
        staging.committed=true;
        return {{"verdict","pass"},{"scope","input-bundle"},{"bundle_version","0.1"},{"manifest_id",manifest.at("id")},{"path",destination.string()}};
    } catch(const Json::exception& e) { throw Error("IR_BUNDLE","/bundle",e.what()); }
      catch(const fs::filesystem_error& e) { throw Error("IR_BUNDLE","/bundle",e.what()); }
}
}
