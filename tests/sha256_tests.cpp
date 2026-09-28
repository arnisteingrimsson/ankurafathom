#include "ankurafathom/runtime/sha256.hpp"
#include <fstream>
#include <iostream>
#include <vector>

namespace rt=ankurafathom::runtime;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: sha256_tests report.json");
        require(rt::sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","empty vector");
        require(rt::sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","abc vector");
        require(rt::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","multiblock vector");
        require(rt::sha256(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","million-a vector");
        rt::Sha256 original;original.update("a");const auto prefix=original.digest();
        auto copy=original;copy.update("bc");original.update("different");
        require(prefix==rt::sha256("a") && copy.digest()==rt::sha256("abc") && original.digest()==rt::sha256("adifferent"),"snapshot/copy altered hash state");
        std::vector<std::size_t> lengths;for(std::size_t n=0;n<130;++n) lengths.push_back(n);
        for(const std::size_t n:{255,256,257,4095,4096,4097,65535,65536,65537,1000000,4194304}) lengths.push_back(n);
        std::ofstream report(argv[1]);report.exceptions(std::ios::badbit|std::ios::failbit);report<<"[\n";
        bool first=true;
        for(const auto length:lengths) {
            std::string bytes(length,'\0');for(std::size_t i=0;i<length;++i) bytes[i]=static_cast<char>((i*131+17)%256);
            const auto expected=rt::sha256(bytes);
            for(const std::size_t chunk:{1,7,55,64,127,4096}) {
                rt::Sha256 streaming;
                for(std::size_t offset=0;offset<bytes.size();offset+=chunk)
                    streaming.update(std::string_view(bytes).substr(offset,std::min(chunk,bytes.size()-offset)));
                require(streaming.digest()==expected && streaming.digest()==expected,"chunking or repeated digest changed hash");
            }
            if(!first) report<<",\n";first=false;
            report<<"{\"length\":"<<length<<",\"sha256\":\""<<expected<<"\"}";
        }
        report<<"\n]\n";
        std::cout<<lengths.size()<<" binary patterns x 7 chunk layouts and four SHA-256 known-answer vectors passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
