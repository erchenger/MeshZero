// SPDX-License-Identifier: GPL-3.0-only
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/history.h"
#include "common/proto.h"
#include "common/qr.h"
#include "common/util.h"

using namespace mz;

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void test_framing()
{
    FrameParser parser;
    std::vector<std::string> frames;
    const auto sink = [&frames](const std::string& p) { frames.push_back(p); };

    const std::string a = frame_wrap("hello");
    const std::string b = frame_wrap(std::string(300, 'x'));
    // Console noise before and between frames must be skipped.
    const std::string stream = "boot text\n" + a + "junk" + b + frame_wrap("");
    for (size_t i = 0; i < stream.size(); i += 7) {
        const size_t n = std::min<size_t>(7, stream.size() - i);
        parser.feed(reinterpret_cast<const uint8_t*>(stream.data()) + i, n, sink);
    }
    CHECK(frames.size() == 3);
    CHECK(frames.size() == 3 && frames[0] == "hello");
    CHECK(frames.size() == 3 && frames[1] == std::string(300, 'x'));
    CHECK(frames.size() == 3 && frames[2].empty());

    // An oversized length must not swallow the following valid frame.
    frames.clear();
    parser.reset();
    const std::string bad = std::string("\x94\xC3\xFF\xFF", 4) + a;
    parser.feed(reinterpret_cast<const uint8_t*>(bad.data()), bad.size(), sink);
    CHECK(frames.size() == 1 && frames[0] == "hello");
}

static void test_proto_roundtrip()
{
    meshtastic_ToRadio req    = meshtastic_ToRadio_init_zero;
    req.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    req.want_config_id        = 0xdeadbeef;
    const std::string bytes   = pb_encode_msg(meshtastic_ToRadio_fields, req);
    CHECK(!bytes.empty());
    meshtastic_ToRadio back = meshtastic_ToRadio_init_zero;
    CHECK(pb_decode_msg(meshtastic_ToRadio_fields, bytes, back));
    CHECK(back.which_payload_variant == meshtastic_ToRadio_want_config_id_tag);
    CHECK(back.want_config_id == 0xdeadbeef);
}

static void test_base64()
{
    const uint8_t key[] = {0x01};
    CHECK(base64_encode(key, 1) == "AQ==");
    std::vector<uint8_t> out;
    CHECK(base64_decode("AQ==", out) && out.size() == 1 && out[0] == 1);
    CHECK(base64_decode("1PG7OiApB1nwvP-rz05pAQ", out) && out.size() == 16);
    CHECK(!base64_decode("not base64!", out));
    const uint8_t bytes[] = {0xfb, 0xff, 0xfe};
    CHECK(base64_encode(bytes, 3, true) == "-__-");
}

static void test_util()
{
    CHECK(node_id_str(0x1234abcd) == "!1234abcd");
    CHECK(version_compare("0.1.1", "0.1.0") > 0);
    CHECK(version_compare("0.1.10", "0.1.9") > 0);
    CHECK(version_compare("0.2", "0.1.9") > 0);
    CHECK(version_compare("0.1", "0.1.0") == 0);
    CHECK(version_compare("0.1.0", "0.1.1") < 0);
    CHECK(version_compare("", "0.1.0") < 0);
    CHECK(release_version("{\"url\":\"x\",\"tag_name\": \"v0.1.2\",\"name\":\"MeshZero 0.1.2\"}") == "0.1.2");
    CHECK(release_version("{\"tag_name\":\"1.0\"}") == "1.0");
    CHECK(release_version("{\"tag_name\":\"v1.0-beta'; rm\"}").empty());
    CHECK(release_version("{\"message\":\"Not Found\"}").empty());
    CHECK(release_version("").empty());
    CHECK(unescape_field(escape_field("a\tb\nc\\d")) == "a\tb\nc\\d");
    CHECK(escape_field("a\tb").find('\t') == std::string::npos);
    CHECK(utf8_truncate("h\xc3\xa9llo", 2) == "h");
    CHECK(utf8_truncate("h\xc3\xa9llo", 3) == "h\xc3\xa9");
    CHECK(age_str(100, 130) == "30s");
    CHECK(age_str(0, 130) == "-");
    const double d = geo_distance_m(52.0, 4.0, 52.0, 5.0);
    CHECK(d > 68000 && d < 69000);
    const double b = geo_bearing_deg(52.0, 4.0, 52.0, 5.0);
    CHECK(b > 85 && b < 95);
    CHECK(std::string(compass_point(b)) == "E");
}

static void test_battery()
{
    char tmpl[] = "/tmp/mzbatXXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    const std::string root = dir;
    const auto supply = [&root](const std::string& name, std::vector<std::pair<std::string, std::string>> files) {
        mkdir((root + "/" + name).c_str(), 0755);
        for (const auto& [file, text] : files) {
            FILE* f = fopen((root + "/" + name + "/" + file).c_str(), "w");
            fputs(text.c_str(), f);
            fclose(f);
        }
    };
    bool charging = true;
    CHECK(battery_percent(charging, root + "/missing") == -1 && !charging);
    supply("AC", {{"type", "Mains\n"}, {"online", "1\n"}});
    supply("a-controller", {{"type", "Battery\n"}, {"scope", "Device\n"}, {"capacity", "100\n"}, {"status", "Full\n"}});
    CHECK(battery_percent(charging, root) == -1);
    supply("bq27220-0", {{"type", "Battery\n"}, {"capacity", "76\n"}, {"status", "Charging\n"}});
    CHECK(battery_percent(charging, root) == 76 && charging);
    supply("bq27220-0", {{"status", "Discharging\n"}});
    CHECK(battery_percent(charging, root) == 76 && !charging);
    supply("bq27220-0", {{"capacity", "\n"}});
    CHECK(battery_percent(charging, root) == -1);
}

static void test_history()
{
    char tmpl[] = "/tmp/mzhistXXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    const std::string path = std::string(dir) + "/messages.log";
    {
        HistoryWriter w(path, 400);
        StoredMsg in;
        in.ts = 1000;
        in.id = 11;
        in.from = 0xaabbccdd;
        in.to = kBroadcast;
        in.channel = 2;
        in.snr = -7.25f;
        in.rssi = -101;
        in.hops = 3;
        in.text = "tab\there\nnewline";
        w.append(in);

        StoredMsg out;
        out.ts = 1001;
        out.id = 12;
        out.from = 1;
        out.to = 0xaabbccdd;
        out.outgoing = true;
        out.pki = true;
        out.ack = AckState::Pending;
        out.text = "reply";
        w.append(out);
        w.append_ack(12, AckState::Implicit, 0);
        w.append_ack(12, AckState::Acked, 0);
        w.append_ack(12, AckState::Implicit, 0);  // must not downgrade
        w.append_ack(999, AckState::Acked, 0);    // unknown id is ignored
    }
    std::vector<StoredMsg> msgs = history_load(path);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) {
        CHECK(msgs[0].text == "tab\there\nnewline");
        CHECK(msgs[0].channel == 2 && msgs[0].hops == 3 && msgs[0].rssi == -101);
        CHECK(msgs[0].snr == -7.25f);
        CHECK(!msgs[0].outgoing && msgs[0].ack == AckState::None);
        CHECK(msgs[1].outgoing && msgs[1].pki && msgs[1].ack == AckState::Acked);
    }
    // Rotation keeps older messages readable.
    {
        HistoryWriter w(path, 400);
        for (int i = 0; i < 20; ++i) {
            StoredMsg m;
            m.ts = 2000 + static_cast<uint32_t>(i);
            m.id = 100 + static_cast<uint32_t>(i);
            m.text = "filler message number " + std::to_string(i);
            w.append(m);
        }
    }
    msgs = history_load(path);
    CHECK(msgs.size() >= 10);
    CHECK(!msgs.empty() && msgs.back().text == "filler message number 19");
    unlink(path.c_str());
    unlink((path + ".1").c_str());

    // Deleting one conversation keeps the others, and drops its acknowledgements.
    {
        HistoryWriter w(path);
        const auto msg = [](uint32_t id, uint32_t from, uint32_t to, uint8_t channel, bool outgoing, const char* text) {
            StoredMsg m;
            m.id       = id;
            m.from     = from;
            m.to       = to;
            m.channel  = channel;
            m.outgoing = outgoing;
            m.ack      = outgoing ? AckState::Pending : AckState::None;
            m.text     = text;
            return m;
        };
        w.append(msg(1, 0x11, kBroadcast, 0, false, "on the primary channel"));
        w.append(msg(2, 0x99, 0x22, 0, true, "to 22"));
        w.append(msg(3, 0x22, 0x99, 0, false, "from 22"));
        w.append(msg(4, 0x99, kBroadcast, 1, true, "on channel 1"));
        w.append_ack(2, AckState::Acked, 0);
        w.append_ack(4, AckState::Implicit, 0);
    }
    const auto with = [](uint32_t peer) {
        return [peer](const StoredMsg& m) { return m.to != kBroadcast && (m.outgoing ? m.to : m.from) == peer; };
    };
    CHECK(history_remove(with(0x22), path));
    msgs = history_load(path);
    CHECK(msgs.size() == 2 && msgs[0].text == "on the primary channel" && msgs[1].text == "on channel 1");
    CHECK(msgs.size() == 2 && msgs[1].ack == AckState::Implicit);
    {
        std::ifstream in(path);
        const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(all.find("A\t2\t") == std::string::npos);
    }
    CHECK(history_remove([](const StoredMsg&) { return true; }, path));
    CHECK(history_load(path).empty());
    CHECK(history_remove([](const StoredMsg&) { return true; }, path + ".missing"));
    unlink(path.c_str());
    rmdir(dir);
}

// Out-of-range ack states in a damaged log are read as "none", never cast blindly.
static void test_history_bad_ack()
{
    char tmpl[] = "/tmp/mzhistXXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    const std::string path = std::string(dir) + "/messages.log";
    if (FILE* f = fopen(path.c_str(), "w")) {
        fputs("M\t1\t5\t1\t2\t0\t1\t0\t0\t0\t0\t200\tsent\n", f);  // outgoing, ack 200
        fputs("M\t2\t6\t1\t2\t0\t1\t0\t0\t0\t0\t1\tpending\n", f);  // outgoing, pending
        fputs("A\t6\t77\t0\n", f);                                   // bogus ack line
        fclose(f);
    }
    const std::vector<StoredMsg> msgs = history_load(path);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) {
        CHECK(msgs[0].ack == AckState::None);
        CHECK(msgs[1].ack == AckState::Pending);
    }
    unlink(path.c_str());
    rmdir(dir);
}

static void test_qr()
{
    // The link "https://meshtastic.org/e/#CgMSAQE" as a QR code, one character per module.
    static const char* const kCode[] = {
        "#######.#...####.###..#######",
        "#.....#.#.#.##..#.#...#.....#",
        "#.###.#..#......#.#.#.#.###.#",
        "#.###.#.#..##.##.#....#.###.#",
        "#.###.#..#.#..#.##.#..#.###.#",
        "#.....#..#.###.##.##..#.....#",
        "#######.#.#.#.#.#.#.#.#######",
        "........#..#...######........",
        "#.##.###..#..#.###.##.#..#.##",
        ".###...###..####.####.###...#",
        "##.#.##....#.#....#..#.#..##.",
        "..##...#...##...#.#.#.#.#...#",
        "#..##.###.....##.###.....##..",
        "..####...#....#..#.#..#...###",
        "#...#.##...###...#.#..###.###",
        "##.#.#..#.##..###..##...#..#.",
        "###..####.#.#.#..#####..##.#.",
        ".....#..#.##..#......#.#.###.",
        "#.##..#.#.##.######.##.##.#..",
        "..#.#....###.##..######...#..",
        ".#..####.#.#.##..##########..",
        "........##.....##.#.#...#####",
        "#######.###...#.#.###.#.##.#.",
        "#.....#.###..##.#.#.#...##.##",
        "#.###.#..##.#..#....#####.##.",
        "#.###.#.#..#....#..###.###..#",
        "#.###.#.##..#.#...####.#..#.#",
        "#.....#...###.#.....#..###.#.",
        "#######.###..#..#####...#..#.",
    };
    const int modules = static_cast<int>(sizeof(kCode) / sizeof(kCode[0]));
    const int scale = 5, border = 4;
    const int side = (modules + 2 * border) * scale;
    std::vector<uint8_t> image(static_cast<size_t>(side) * side, 255);
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (kCode[y][x] != '#') continue;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    image[static_cast<size_t>((y + border) * scale + dy) * side + (x + border) * scale + dx] = 0;
                }
            }
        }
    }
    CHECK(qr_decode(image.data(), side, side) == "https://meshtastic.org/e/#CgMSAQE");

    std::vector<uint8_t> blank(static_cast<size_t>(side) * side, 128);
    CHECK(qr_decode(blank.data(), side, side).empty());
    CHECK(qr_decode(nullptr, 0, 0).empty());
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        CHECK(qr_decode_photo(image.data(), side, side, attempt) == "https://meshtastic.org/e/#CgMSAQE");
        CHECK(qr_decode_photo(blank.data(), side, side, attempt).empty());
    }
    // Still works after its buffers were given back.
    qr_release();
    CHECK(qr_decode(image.data(), side, side) == "https://meshtastic.org/e/#CgMSAQE");
    qr_release();
}

int main()
{
    test_framing();
    test_proto_roundtrip();
    test_base64();
    test_util();
    test_battery();
    test_history();
    test_history_bad_ack();
    test_qr();
    if (failures == 0) printf("all tests passed\n");
    return failures == 0 ? 0 : 1;
}
