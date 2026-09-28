#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace ankurafathom::runtime {
inline bool same_file(const std::filesystem::path& a,const std::filesystem::path& b) {
    if(a==b) return true;
    return std::filesystem::exists(a) && std::filesystem::exists(b) && std::filesystem::equivalent(a,b);
}

// POSIX publication: no result becomes visible until the complete sibling file
// has been written and closed. Does not promise crash-durable directory metadata.
// The callback borrows the descriptor. It must finish all writes and close any
// duplicate descriptors before returning; it must not close this descriptor.
template<class Write>
void write_file_atomically(const std::filesystem::path& path,Write&& write) {
    if(path.empty() || path.filename().empty()) throw std::invalid_argument("empty result filename");
    const auto parent=path.has_parent_path()?path.parent_path():std::filesystem::path(".");
    const auto pattern=(parent/(".fathom-"+path.filename().string()+".tmp-XXXXXX")).string();
    std::vector<char> name(pattern.begin(),pattern.end());name.push_back('\0');
    const int descriptor=::mkstemp(name.data());
    if(descriptor<0) throw std::system_error(errno,std::generic_category(),"cannot create temporary result");
    struct Temporary {
        int descriptor;
        const char* name;
        bool committed=false;
        ~Temporary() { if(descriptor>=0) ::close(descriptor);if(!committed) ::unlink(name); }
    } temporary{descriptor,name.data()};
    write(descriptor);
    while(::fsync(descriptor)!=0) if(errno!=EINTR) throw std::system_error(errno,std::generic_category(),"cannot flush temporary result");
    temporary.descriptor=-1;
    if(::close(descriptor)!=0) throw std::system_error(errno,std::generic_category(),"cannot close temporary result");
    std::filesystem::rename(name.data(),path);
    temporary.committed=true;
}

inline void write_text_atomically(const std::filesystem::path& path,std::string_view text) {
    write_file_atomically(path,[&](int descriptor) {
    std::size_t offset=0;
    while(offset<text.size()) {
        const auto count=::write(descriptor,text.data()+offset,std::min<std::size_t>(text.size()-offset,16*1024*1024));
        if(count<0) { if(errno==EINTR) continue;throw std::system_error(errno,std::generic_category(),"cannot write temporary result"); }
        if(count==0) throw std::runtime_error("temporary result write made no progress");
        offset+=static_cast<std::size_t>(count);
    }
    });
}
} // namespace ankurafathom::runtime
