#include "fuse_read_buffer.h"
#include "hash.h"
#include "object_store.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-fuse-read-buffer-") + suffix + "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* path = mkdtemp(buf.data());
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
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
        else std::remove(child.c_str());
    }
    closedir(dir);
    rmdir(path.c_str());
}

static void test_make_fd_read_buf_fields_and_bounds() {
    std::string root = make_tmp_dir("buf");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    Hash hash = store.write_blob(payload);
    REQUIRE(hash != ZERO_HASH);
    BlobView view;
    REQUIRE(store.open_blob(hash, view) == 0);
    REQUIRE(view);

    fuse_bufvec* buf = nullptr;
    REQUIRE(make_fd_read_buf(view, 3, 1, &buf) == 0);
    REQUIRE(buf != nullptr);
    REQUIRE(buf->count == 1);
    REQUIRE(buf->buf[0].fd == view.fd());
    REQUIRE(buf->buf[0].size == 3);
    REQUIRE(buf->buf[0].pos == 13);
    REQUIRE((buf->buf[0].flags & FUSE_BUF_IS_FD) != 0);
    REQUIRE((buf->buf[0].flags & FUSE_BUF_FD_SEEK) != 0);
    std::free(buf);

    // requested > remaining: clamped to one byte left at offset 4 of 5.
    REQUIRE(make_fd_read_buf(view, 99, 4, &buf) == 0);
    REQUIRE(buf->buf[0].size == 1);
    std::free(buf);

    // exactly at EOF: zero bytes.
    REQUIRE(make_fd_read_buf(view, 1, 5, &buf) == 0);
    REQUIRE(buf->buf[0].size == 0);
    std::free(buf);

    // offset way past EOF: bounded to payload_size, size 0, pos at end.
    REQUIRE(make_fd_read_buf(view, 1, std::numeric_limits<off_t>::max(), &buf) == 0);
    REQUIRE(buf->buf[0].size == 0);
    REQUIRE(buf->buf[0].pos == 17);
    std::free(buf);

    // negative offset rejected.
    REQUIRE(make_fd_read_buf(view, 1, -1, &buf) == -EINVAL);

    // invalid (empty) view rejected.
    REQUIRE(make_fd_read_buf(BlobView{}, 1, 0, &buf) == -EIO);

    // null out pointer rejected.
    REQUIRE(make_fd_read_buf(view, 1, 0, nullptr) == -EINVAL);

    remove_dir_recursive(root);
    std::printf("  PASS test_make_fd_read_buf_fields_and_bounds\n");
}

int main() {
    std::printf("test_fuse_read_buffer:\n");
    test_make_fd_read_buf_fields_and_bounds();
    std::printf("PASS test_fuse_read_buffer\n");
    return 0;
}
