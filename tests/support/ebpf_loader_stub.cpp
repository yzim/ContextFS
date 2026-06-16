#include "ebpf_loader.h"

namespace cas {

struct EbpfLoader::cas_probe_skel_wrapper {};

EbpfLoader::EbpfLoader() = default;
EbpfLoader::~EbpfLoader() = default;

bool EbpfLoader::load_and_attach() { return false; }
void EbpfLoader::detach() {}
int EbpfLoader::session_map_fd() const { return -1; }
int EbpfLoader::policy_map_fd() const { return -1; }
int EbpfLoader::ringbuf_map_fd() const { return -1; }
int EbpfLoader::drops_map_fd() const { return -1; }
uint64_t EbpfLoader::cgroup_id_from_path(const std::string&) const { return 0; }
bool EbpfLoader::register_session(const std::string&, uint64_t, uint32_t, uint32_t, uint8_t) { return false; }
bool EbpfLoader::unregister_session(const std::string&) { return false; }
uint64_t EbpfLoader::total_drops() const { return 0; }

} // namespace cas
