// SPDX-License-Identifier: GPL-3.0-only
#include "common/history.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <sys/stat.h>
#include <unistd.h>

#include "common/util.h"

namespace mz {

std::string history_dir()
{
    const char* env = getenv("MESHZERO_STATE_DIR");
    return env && env[0] ? env : "/var/lib/meshzero";
}

std::string history_path()
{
    return history_dir() + "/messages.log";
}

HistoryWriter::HistoryWriter(std::string path, size_t max_bytes) : path_(std::move(path)), max_bytes_(max_bytes) {}

void HistoryWriter::write_line(const std::string& line)
{
    struct stat st {};
    if (stat(path_.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) > max_bytes_) {
        rename(path_.c_str(), (path_ + ".1").c_str());
    }
    FILE* f = fopen(path_.c_str(), "ae");
    if (!f) return;
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
    chmod(path_.c_str(), 0644);
}

void HistoryWriter::append(const StoredMsg& m)
{
    const unsigned flags = (m.outgoing ? 1u : 0u) | (m.pki ? 2u : 0u) | (m.emoji ? 4u : 0u);
    char head[160];
    snprintf(head, sizeof(head), "M\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%d\t%u\t", m.ts, m.id, m.from, m.to,
             static_cast<unsigned>(m.channel), flags, m.reply_id, static_cast<int>(m.snr * 4), m.rssi, m.hops,
             static_cast<unsigned>(m.ack));
    write_line(std::string(head) + escape_field(m.text) + "\n");
}

void HistoryWriter::append_ack(uint32_t id, AckState state, int error)
{
    char line[64];
    snprintf(line, sizeof(line), "A\t%u\t%u\t%d\n", id, static_cast<unsigned>(state), error);
    write_line(line);
}

// A damaged or hand-edited log must not produce a value outside the enum.
static AckState parse_ack(const std::string& field)
{
    const int v = atoi(field.c_str());
    return v >= static_cast<int>(AckState::None) && v <= static_cast<int>(AckState::Failed) ? static_cast<AckState>(v)
                                                                                           : AckState::None;
}

// A message record ("M") split into its fields; false for any other line.
static bool parse_msg(const std::vector<std::string>& f, StoredMsg& m)
{
    if (f.size() < 13 || f[0] != "M") return false;
    m.ts                 = static_cast<uint32_t>(strtoul(f[1].c_str(), nullptr, 10));
    m.id                 = static_cast<uint32_t>(strtoul(f[2].c_str(), nullptr, 10));
    m.from               = static_cast<uint32_t>(strtoul(f[3].c_str(), nullptr, 10));
    m.to                 = static_cast<uint32_t>(strtoul(f[4].c_str(), nullptr, 10));
    m.channel            = static_cast<uint8_t>(strtoul(f[5].c_str(), nullptr, 10));
    const unsigned flags = static_cast<unsigned>(strtoul(f[6].c_str(), nullptr, 10));
    m.outgoing           = (flags & 1u) != 0;
    m.pki                = (flags & 2u) != 0;
    m.emoji              = (flags & 4u) != 0;
    m.reply_id           = static_cast<uint32_t>(strtoul(f[7].c_str(), nullptr, 10));
    m.snr                = static_cast<float>(atoi(f[8].c_str())) / 4.0f;
    m.rssi               = atoi(f[9].c_str());
    m.hops               = atoi(f[10].c_str());
    m.ack                = parse_ack(f[11]);
    m.text               = unescape_field(f[12]);
    return true;
}

static void load_file(const std::string& path, std::vector<StoredMsg>& out, std::unordered_map<uint32_t, size_t>& outgoing)
{
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> f = split(line, '\t');
        StoredMsg m;
        if (parse_msg(f, m)) {
            if (m.outgoing && m.id != 0) outgoing[m.id] = out.size();
            out.push_back(std::move(m));
        } else if (f.size() >= 4 && f[0] == "A") {
            const uint32_t id = static_cast<uint32_t>(strtoul(f[1].c_str(), nullptr, 10));
            const auto it     = outgoing.find(id);
            if (it == outgoing.end()) continue;
            StoredMsg& m         = out[it->second];
            const AckState state = parse_ack(f[2]);
            if (state == AckState::None) continue;
            // A real acknowledgement is final; never downgrade it.
            if (m.ack == AckState::Acked) continue;
            m.ack       = state;
            m.ack_error = atoi(f[3].c_str());
        }
    }
}

std::vector<StoredMsg> history_load(const std::string& path)
{
    std::vector<StoredMsg> out;
    std::unordered_map<uint32_t, size_t> outgoing;
    load_file(path + ".1", out, outgoing);
    load_file(path, out, outgoing);
    return out;
}

// One file of history_remove(). `dropped` carries the ids of dropped outgoing
// messages on to the next file, where their acknowledgements may be.
static bool remove_in_file(const std::string& path, const std::function<bool(const StoredMsg&)>& drop,
                           std::unordered_set<uint32_t>& dropped)
{
    std::ifstream in(path);
    if (!in) return true;
    std::string kept;
    std::string line;
    bool changed = false;
    while (std::getline(in, line)) {
        const std::vector<std::string> f = split(line, '\t');
        StoredMsg m;
        if (parse_msg(f, m) && drop(m)) {
            if (m.outgoing && m.id != 0) dropped.insert(m.id);
            changed = true;
            continue;
        }
        if (f.size() >= 4 && f[0] == "A" && dropped.count(static_cast<uint32_t>(strtoul(f[1].c_str(), nullptr, 10)))) {
            changed = true;
            continue;
        }
        kept += line + '\n';
    }
    in.close();
    if (!changed) return true;
    const std::string tmp = path + ".tmp";
    FILE* f               = fopen(tmp.c_str(), "we");
    if (!f) return false;
    const bool written = fwrite(kept.data(), 1, kept.size(), f) == kept.size();
    if (fclose(f) != 0 || !written) {
        unlink(tmp.c_str());
        return false;
    }
    chmod(tmp.c_str(), 0644);
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool history_remove(const std::function<bool(const StoredMsg&)>& drop, const std::string& path)
{
    std::unordered_set<uint32_t> dropped;
    const bool rotated = remove_in_file(path + ".1", drop, dropped);
    return remove_in_file(path, drop, dropped) && rotated;
}

}  // namespace mz
