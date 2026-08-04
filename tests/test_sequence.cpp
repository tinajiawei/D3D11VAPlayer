#include "test_common.h"

#include "ui/media_sequence.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static void touch(const fs::path& p) {
    std::ofstream(p, std::ios::binary) << "x";
}

void test_sequence() {
    std::fprintf(stderr, "[test] MediaSequence\n");
    const fs::path root = fs::temp_directory_path() / "me_seq_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "sub", ec);
    touch(root / "a.mp4");
    touch(root / "b.png");
    touch(root / "c.jpg");
    touch(root / "e.txt");
    touch(root / "f.mp3");
    touch(root / "sub" / "d.webm");

    me::MediaSequence seq;
    std::wstring out;

    // 视频：递归扫描 + 自然排序 + 边界
    seq.rebuild((root / "a.mp4").wstring(), me::SequenceType::Video);
    auto info = seq.snapshot(me::SequenceType::Video, true);
    CHECK_EQ(info.count, 2);
    CHECK_EQ(info.index, 0);
    CHECK(info.auto_next);
    CHECK(seq.next(out));
    CHECK(out.find(L"d.webm") != std::wstring::npos);
    CHECK(!seq.next(out));
    CHECK(seq.prev(out));
    CHECK(out.find(L"a.mp4") != std::wstring::npos);
    CHECK(!seq.prev(out));

    // 图片过滤
    seq.rebuild((root / "b.png").wstring(), me::SequenceType::Image);
    info = seq.snapshot(me::SequenceType::Image, false);
    CHECK_EQ(info.count, 2);
    CHECK_EQ(info.index, 0);
    CHECK(seq.next(out));
    CHECK(out.find(L"c.jpg") != std::wstring::npos);

    // 全部：视频+图片+音频
    seq.rebuild((root / "b.png").wstring(), me::SequenceType::All);
    info = seq.snapshot(me::SequenceType::All, false);
    CHECK_EQ(info.count, 5);
    CHECK_EQ(info.index, 1);  // a.mp4, b.png, c.jpg, f.mp3, sub\\d.webm

    // 当前文件类型与过滤不符：插入到最前，仍可浏览
    seq.rebuild((root / "e.txt").wstring(), me::SequenceType::Video);
    info = seq.snapshot(me::SequenceType::Video, false);
    CHECK_EQ(info.count, 3);  // e.txt(插入最前) + a.mp4 + sub\\d.webm
    CHECK_EQ(info.index, 0);
    CHECK(info.current_name.find("e.txt") != std::string::npos);
    CHECK(seq.next(out));
    CHECK(out.find(L"a.mp4") != std::wstring::npos);

    fs::remove_all(root, ec);
}