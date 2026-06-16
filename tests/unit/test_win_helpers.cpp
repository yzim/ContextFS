#include "platform/windows/path_translation.h"
#include "platform/windows/ntstatus_map.h"
#include <cassert>
#include <cerrno>
#include <cstdio>

int main() {
    using namespace cas::win;

    // Path translation
    assert(narrow(L"\\foo\\bar.txt") == "/foo/bar.txt");
    assert(widen("/foo/bar.txt") == L"\\foo\\bar.txt");
    assert(narrow(L"") == "");
    assert(widen("") == L"");
    assert(narrow(L"foo\\bar") == "/foo/bar");        // missing leading sep tolerated
    assert(widen(narrow(L"\\a\\b\\c")) == L"\\a\\b\\c");

    // NTSTATUS map
    assert(errno_to_ntstatus(0) == STATUS_SUCCESS);
    assert(errno_to_ntstatus(ENOENT) == STATUS_OBJECT_NAME_NOT_FOUND);
    assert(errno_to_ntstatus(ESTALE) == STATUS_FILE_INVALID);
    assert(errno_to_ntstatus(99999) == STATUS_UNSUCCESSFUL);

    std::printf("win_helpers: OK\n");
}
