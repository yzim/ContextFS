#include "runtime_state.h"
#include "object_store.h"
#include "hash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir() {
    std::vector<char> templ{'/','t','m','p','/','a','g','e','n','t','v','f','s','-','r','t','-','X','X','X','X','X','X','\0'};
    char* path = mkdtemp(templ.data());
    REQUIRE(path != nullptr);
    return std::string(path);
}

static void remove_dir_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st;
        REQUIRE(lstat(child.c_str(), &st) == 0);
        if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
        else REQUIRE(std::remove(child.c_str()) == 0);
    }
    closedir(dir);
    REQUIRE(rmdir(path.c_str()) == 0);
}

static Hash tagged_hash(uint8_t tag) {
    Hash h{};
    h[0] = tag;
    return h;
}

static UnionRuntimeState sample_state() {
    UnionRuntimeState s;
    s.record_version = 1;
    s.parent_union_state_id = std::string(64, '0');
    s.branch = "main";
    s.fs_commit = tagged_hash(0x41);
    s.agent_state_id = std::string(64, 'a');
    s.runtime_id = "rt-0001";
    s.runtime_generation = 3;
    s.template_id = "tmpl-0007";
    s.template_kind = "live_fork";
    s.boundary_kind = "before_tool";
    // Contains characters that the percent codec must escape to round-trip
    // through the key=value / warning-separator framing: '%' itself (the
    // escape introducer), '|' (the warning field separator) and '=' (the
    // key=value separator). Proves the %-itself encode/decode symmetry.
    s.command_ref = "argv:counter 100% pure|x=y";
    s.resource_manifest_ref = "inline:regular-files-only";
    s.timestamp_ns = 1700000000123456789ULL;
    s.warnings.push_back({"socket", "external sockets are blockers in this slice", true});
    return s;
}

static void test_roundtrip_preserves_fields() {
    UnionRuntimeState in = sample_state();
    std::vector<uint8_t> body = serialize_union_runtime_state(in);
    REQUIRE(!body.empty());
    const std::string header = "agentvfs.union_runtime_state.v1\n";
    REQUIRE(body.size() > header.size());
    REQUIRE(std::string(body.begin(), body.begin() + header.size()) == header);

    UnionRuntimeState out;
    std::string error;
    REQUIRE(deserialize_union_runtime_state(body, out, error));
    REQUIRE(error.empty());
    REQUIRE(out.record_version == 1);
    REQUIRE(out.parent_union_state_id == in.parent_union_state_id);
    REQUIRE(out.branch == "main");
    REQUIRE(out.fs_commit == in.fs_commit);
    REQUIRE(out.agent_state_id == in.agent_state_id);
    REQUIRE(out.runtime_id == "rt-0001");
    REQUIRE(out.runtime_generation == 3);
    REQUIRE(out.template_id == "tmpl-0007");
    REQUIRE(out.template_kind == "live_fork");
    REQUIRE(out.boundary_kind == "before_tool");
    // The command_ref carries '%', '|' and '=': this both checks round-trip
    // equality AND proves the percent-encode/decode symmetry for the
    // %-itself case (the codec's escape introducer) plus the field/separator
    // characters that would otherwise corrupt the framing.
    REQUIRE(out.command_ref == in.command_ref);
    REQUIRE(out.command_ref == "argv:counter 100% pure|x=y");
    REQUIRE(out.resource_manifest_ref == "inline:regular-files-only");
    // timestamp_ns must round-trip exactly (realistic nanosecond value).
    REQUIRE(out.timestamp_ns == in.timestamp_ns);
    REQUIRE(out.timestamp_ns == 1700000000123456789ULL);
    REQUIRE(out.warnings.size() == 1);
    REQUIRE(out.warnings[0].blocker);
}

static void test_cas_write_sets_union_state_id_to_blob_hash() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    UnionRuntimeState s = sample_state();
    std::string error;
    Hash id = write_union_runtime_state(store, s, error);
    REQUIRE(id != ZERO_HASH);
    REQUIRE(error.empty());
    REQUIRE(s.union_state_id == hash_to_hex(id));

    UnionRuntimeState out;
    REQUIRE(read_union_runtime_state(store, id, out, error));
    REQUIRE(out.union_state_id == hash_to_hex(id));
    REQUIRE(out.fs_commit == s.fs_commit);
    remove_dir_recursive(root);
}

static void test_warning_roundtrip_preserves_blocker_state() {
    RestoreEligibility e;
    REQUIRE(restore_eligibility_to_string(RestoreEligibility::FsOnly) == "fs_only");
    REQUIRE(restore_eligibility_from_string("metadata_only", e));
    REQUIRE(e == RestoreEligibility::MetadataOnly);
    REQUIRE(!restore_eligibility_from_string("live-ish", e));

    std::string body = "agentvfs.union_runtime_state.v1\n"
        "record_version=1\n"
        "branch=main\n"
        "fs_commit=" + std::string(64, '4') + "\n"
        "runtime_id=rt\n"
        "runtime_generation=1\n"
        "template_id=tmpl\n"
        "template_kind=live_fork\n"
        "boundary_kind=manual\n"
        "warning=socket|1|external%20socket\n";
    UnionRuntimeState out;
    std::string error;
    REQUIRE(deserialize_union_runtime_state(
        std::vector<uint8_t>(body.begin(), body.end()), out, error));
    REQUIRE(out.warnings.size() == 1);
    REQUIRE(out.warnings[0].kind == "socket");
    REQUIRE(out.warnings[0].blocker);
    REQUIRE(out.warnings[0].description == "external socket");
}

int main() {
    std::printf("test_runtime_state:\n");
    test_roundtrip_preserves_fields();
    test_cas_write_sets_union_state_id_to_blob_hash();
    test_warning_roundtrip_preserves_blocker_state();
    std::printf("PASS test_runtime_state\n");
    return 0;
}
