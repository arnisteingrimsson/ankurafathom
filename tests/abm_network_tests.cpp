#include "ankurafathom/abm/network.hpp"
#include <iostream>
#include <numeric>
#include <random>

namespace {
using namespace ankurafathom;
using Network=abm::CsrNetwork;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool caught=false; try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid network operation succeeded");
}
std::vector<std::uint64_t> neighbors(const Network& graph,std::uint64_t id) {
    const auto row=graph.neighbors(id); return {row.begin(),row.end()};
}
void csr_and_edits() {
    Network graph({99,7,31,2},{{7,31},{31,2}});
    require(graph.vertices()==std::vector<std::uint64_t>({2,7,31,99}),"vertices not canonical");
    require(neighbors(graph,31)==std::vector<std::uint64_t>({2,7}) && neighbors(graph,99).empty(),"CSR rows differ");
    require(graph.offsets()==std::vector<std::size_t>({0,1,2,4,4}),"CSR offsets differ");
    auto snapshot=graph;
    graph.edit({{99,2}},{{2,31}});
    require(neighbors(graph,2)==std::vector<std::uint64_t>({99}) && neighbors(snapshot,2)==std::vector<std::uint64_t>({31}),"network snapshot changed");
    const auto baseline=graph.edges();
    rejects([&] { graph.edit({{7,99},{2,99}},{}); });
    rejects([&] { graph.edit({{7,99}},{{2,31}}); });
    rejects([&] { graph.edit({{7,99},{99,7}},{}); });
    rejects([&] { graph.edit({{7,31}},{{7,31}}); });
    require(graph.edges()==baseline,"failed edge edit partly committed");
    rejects([&] { Network invalid({1,1},{}); });
    rejects([&] { Network invalid({1,2},{{1,3}}); });
    rejects([&] { Network invalid({1,2},{{1,1}}); });
    rejects([&] { Network invalid({1,2},{{1,2},{2,1}}); });
    Network directed({1,2,3},{{1,2},{2,1},{3,1}},true);
    require(neighbors(directed,1)==std::vector<std::uint64_t>{2} && directed.edge_count()==3,"directed edges collapsed");
    Network empty({},{});
    require(empty.offsets()==std::vector<std::size_t>{0},"empty CSR differs");
    rejects([&] { (void)empty.neighbors(0); });
    std::mt19937_64 random(20260925);
    std::vector<std::uint64_t> vertices(13); std::iota(vertices.begin(),vertices.end(),100);
    for(bool directed_mode:{false,true}) {
        std::vector<std::vector<bool>> matrix(13,std::vector<bool>(13));
        Network changing(vertices,{},directed_mode);
        for(int step=0;step<256;++step) {
            const auto a=random()%13,b=random()%13;
            if(a==b) continue;
            Network::Edge edge{vertices[a],vertices[b]};
            if(matrix[a][b]) changing.edit({}, {edge}); else changing.edit({edge},{});
            matrix[a][b]=!matrix[a][b];
            if(!directed_mode) matrix[b][a]=matrix[a][b];
            for(std::size_t i=0;i<13;++i) {
                std::vector<std::uint64_t> expected;
                for(std::size_t j=0;j<13;++j) if(matrix[i][j]) expected.push_back(vertices[j]);
                require(neighbors(changing,vertices[i])==expected,"CSR edits differ from independent adjacency matrix");
            }
        }
    }
}
void integer_choices() {
    std::vector<std::uint32_t> words{0,1,2,3,4,5,6}; std::size_t index=0;
    require(rng::uniform_index(10,[&] { return words.at(index++); })==6 && index==7,"integer rejection threshold differs");
    require(rng::uniform_index(std::uint64_t{1}<<32,[] { return 0xFFFFFFFFU; })==0xFFFFFFFFULL,"full-word bound differs");
    require(rng::uniform_index(1,[] { return 123U; })==0,"singleton choice differs");
    rejects([&] { (void)rng::uniform_index(0,[] { return 0U; }); });
    rejects([&] { (void)rng::uniform_index((std::uint64_t{1}<<32)+1,[] { return 0U; }); });
}
void generators() {
    std::vector<std::uint64_t> vertices(32); std::iota(vertices.begin(),vertices.end(),10);
    const abm::NetworkDraws draws{12345,7,8,900};
    require(abm::erdos_renyi(vertices,0,draws).edge_count()==0,"ER zero has edges");
    require(abm::erdos_renyi(vertices,1,draws).edge_count()==496,"ER complete edge count differs");
    auto graph=abm::erdos_renyi(vertices,.3,draws);
    std::vector<Network::Edge> expected;
    std::uint64_t ordinal=0;
    for(std::size_t i=0;i<vertices.size();++i) for(std::size_t j=i+1;j<vertices.size();++j,++ordinal)
        if(rng::uniform_open(rng::draw(draws.seed,{draws.scenario,draws.replication,ordinal,0,draws.stream,0})[0])<.3)
            expected.emplace_back(vertices[i],vertices[j]);
    require(graph.edges()==expected,"ER pair draw addresses differ");
    std::reverse(vertices.begin(),vertices.end());
    require(abm::erdos_renyi(vertices,.3,draws).edges()==graph.edges(),"ER depends on vertex declaration order");
    for(unsigned seed=0;seed<32;++seed) {
        const abm::NetworkDraws context{seed,1,2,3};
        for(double probability:{0.,.25,1.}) {
            auto ws=abm::watts_strogatz(vertices,4,probability,context);
            require(ws.edge_count()==64,"rewiring changed edge count");
            if(probability==0) for(auto id:vertices) require(ws.neighbors(id).size()==4,"ring degree differs");
            auto ordered=vertices; std::sort(ordered.begin(),ordered.end());
            require(abm::watts_strogatz(ordered,4,probability,context).edges()==ws.edges(),"rewiring depends on declaration order");
        }
        for(std::size_t m:{1,2,5}) {
            auto ba=abm::barabasi_albert(vertices,m,context);
            require(ba.edge_count()==m*(vertices.size()-m),"preferential attachment edge count differs");
            std::set<std::uint64_t> reached{vertices.front()};
            for(std::size_t pass=0;pass<vertices.size();++pass) {
                const auto previous=reached;
                for(auto id:previous) for(auto other:ba.neighbors(id)) reached.insert(other);
            }
            require(reached.size()==vertices.size(),"preferential attachment disconnected initial star");
            require(ba.edges()==abm::barabasi_albert(vertices,m,context).edges(),"preferential graph does not replay");
        }
    }
    require(abm::watts_strogatz({0,1,2},2,1,draws).edge_count()==3,"full ring rewiring failed");
    require(abm::watts_strogatz({0},0,1,draws).edge_count()==0,"isolated ring failed");
    require(abm::barabasi_albert({0,1,2,3},3,draws).edges()==std::vector<Network::Edge>({{0,1},{0,2},{0,3}}),"initial star differs");
    rejects([&] { (void)abm::watts_strogatz(vertices,3,.2,draws); });
    rejects([&] { (void)abm::barabasi_albert(vertices,0,draws); });
    rejects([&] { (void)abm::erdos_renyi({},.5,{0,65536,0,0}); });
    rejects([&] { (void)abm::erdos_renyi({},std::numeric_limits<double>::quiet_NaN(),draws); });
}
}
int main() {
    try { csr_and_edits(); integer_choices(); generators(); std::cout<<"CSR/matrix agreement, atomic edits and addressed graph invariants passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
