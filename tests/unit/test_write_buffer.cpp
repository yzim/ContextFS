#include "write_buffer.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace cas;

static void test_basic_write_read() {
    Hash base = ZERO_HASH;
    std::vector<uint8_t> base_data = {'H', 'e', 'l', 'l', 'o'};
    WriteBuffer wb(base, base_data.size());

    uint8_t buf[16];
    size_t n = wb.read(0, buf, 5, base_data);
    assert(n == 5);
    assert(std::memcmp(buf, "Hello", 5) == 0);

    const uint8_t patch[] = {'W', 'O', 'R', 'L', 'D'};
    wb.write(0, patch, 5);
    n = wb.read(0, buf, 5, base_data);
    assert(n == 5);
    assert(std::memcmp(buf, "WORLD", 5) == 0);
    std::printf("  PASS test_basic_write_read\n");
}

static void test_write_extends_size() {
    Hash base = ZERO_HASH;
    std::vector<uint8_t> base_data = {'A', 'B'};
    WriteBuffer wb(base, base_data.size());

    const uint8_t ext[] = {'C', 'D', 'E'};
    wb.write(2, ext, 3);
    assert(wb.effective_size(base_data.size()) == 5);

    uint8_t buf[8];
    size_t n = wb.read(0, buf, 8, base_data);
    assert(n == 5);
    assert(std::memcmp(buf, "ABCDE", 5) == 0);
    std::printf("  PASS test_write_extends_size\n");
}

static void test_coalesce_adjacent() {
    Hash base = ZERO_HASH;
    std::vector<uint8_t> base_data(10, 0);
    WriteBuffer wb(base, base_data.size());

    const uint8_t a[] = {'A', 'B'};
    const uint8_t b[] = {'C', 'D'};
    wb.write(0, a, 2);
    wb.write(2, b, 2);

    uint8_t buf[4];
    size_t n = wb.read(0, buf, 4, base_data);
    assert(n == 4);
    assert(std::memcmp(buf, "ABCD", 4) == 0);
    std::printf("  PASS test_coalesce_adjacent\n");
}

static void test_coalesce_overlapping() {
    Hash base = ZERO_HASH;
    std::vector<uint8_t> base_data(10, 'x');
    WriteBuffer wb(base, base_data.size());

    const uint8_t a[] = {'A', 'B', 'C', 'D'};
    wb.write(0, a, 4);
    const uint8_t b[] = {'1', '2', '3'};
    wb.write(2, b, 3); // overlaps [2,4), extends to 5

    uint8_t buf[10];
    size_t n = wb.read(0, buf, 10, base_data);
    assert(n == 10);
    assert(buf[0] == 'A' && buf[1] == 'B');
    assert(buf[2] == '1' && buf[3] == '2' && buf[4] == '3');
    assert(buf[5] == 'x');
    std::printf("  PASS test_coalesce_overlapping\n");
}

static void test_truncate() {
    Hash base = ZERO_HASH;
    std::vector<uint8_t> base_data = {'A', 'B', 'C', 'D', 'E'};
    WriteBuffer wb(base, base_data.size());

    wb.truncate(3);
    assert(wb.effective_size(base_data.size()) == 3);

    uint8_t buf[8];
    size_t n = wb.read(0, buf, 8, base_data);
    assert(n == 3);
    assert(std::memcmp(buf, "ABC", 3) == 0);
    std::printf("  PASS test_truncate\n");
}

static void test_materialize() {
    Hash base = ZERO_HASH;
    std::vector<uint8_t> base_data = {'H', 'e', 'l', 'l', 'o'};
    WriteBuffer wb(base, base_data.size());

    const uint8_t patch[] = {'W'};
    wb.write(0, patch, 1);
    auto result = wb.materialize(base_data);
    assert(result.size() == 5);
    assert(result[0] == 'W');
    assert(result[1] == 'e');
    std::printf("  PASS test_materialize\n");
}

int main() {
    std::printf("test_write_buffer:\n");
    test_basic_write_read();
    test_write_extends_size();
    test_coalesce_adjacent();
    test_coalesce_overlapping();
    test_truncate();
    test_materialize();
    std::printf("All write buffer tests passed.\n");
    return 0;
}
