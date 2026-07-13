// pointscount — live TikTok gift-point counter.
//
// Starts from a user-supplied baseline (the points already on the stream) and
// adds the diamond value of every gift received from that moment on.
//
// Streak handling: while a streakable gift combo (type 1) is in progress,
// TikTok sends intermediate WebcastGiftMessage updates (repeat_end == 0) whose
// repeat_count is a running value for the SAME combo, not an increment. Those
// are only shown as a live "pending" figure; the combo's real value
// (repeat_count * diamond_count) is committed to the total exactly once, when
// the final message (repeat_end == 1) arrives. Non-streakable gifts are
// committed immediately.
//
// Usage: pointscount <@username> [--start N] [--debug [file]] [options]
//
// --debug dumps 100% of the bytes received from TikTok (raw WebSocket push
// frames, raw /im/fetch bodies, and every decoded webcast message payload)
// to a file, one base64 record per line, so missed gifts can be analyzed
// offline (see tools/decode_dump.py).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "ttlive/client.hpp"

namespace {

ttlive::TikTokLiveClient* g_client = nullptr;

void on_sigint(int) {
    if (g_client) g_client->disconnect();
}

// Gift point accounting, streak-safe.
//
// Rules learned from real captures:
//   * A streakable gift (gift_type == 1) is sent as a series of intermediate
//     frames with repeat_end == 0 and a final frame with repeat_end == 1 that
//     carries the FULL combo repeat_count. We count a streakable gift *only*
//     on its final frame (repeat_end == 1) and IGNORE every repeat_end == 0
//     frame. This is important because TikTok sometimes emits a trailing
//     repeat_end == 0 frame *after* the final one (or reorders them); treating
//     those as a new pending combo silently loses points. We keep no
//     "pending" state that can leak into the total.
//   * Non-streakable gifts (any other type) never send a final frame, so we
//     count them as they arrive.
class PointsCounter {
public:
    explicit PointsCounter(int64_t start) : total_(start) {}

    // Result of feeding a gift event to the counter.
    enum class Result { Counted, Streaking, Duplicate };

    // Feed a gift event. De-duplicates by server msg_id so the same gift
    // delivered over both transports (or replayed on reconnect / reloaded from
    // a ledger) is counted exactly once.
    Result on_gift(const ttlive::Event& e, int64_t& value_out) {
        std::lock_guard<std::mutex> lk(mu_);
        const bool streakable = (e.gift_type == 1);
        const int64_t value =
            static_cast<int64_t>(e.diamond_count) * std::max(e.repeat_count, 1);
        value_out = value;

        // Intermediate frame of a streak: display-only, never counted.
        if (streakable && e.gift_streaking) {
            in_progress_[combo_key(e)] = value;
            return Result::Streaking;
        }

        // Final frame of a streak (repeat_end == 1) or a non-streakable gift.
        // De-dup by msg_id: the counted frame is committed at most once.
        if (e.msg_id != 0 && !counted_ids_.insert(e.msg_id).second)
            return Result::Duplicate;

        if (streakable) in_progress_.erase(combo_key(e));
        total_ += value;
        return Result::Counted;
    }

    // Restore a previously-counted gift from a ledger line (no re-adding of
    // value; sets state directly). Used by --reconnect.
    void restore_counted(int64_t msg_id, int64_t total_after) {
        std::lock_guard<std::mutex> lk(mu_);
        if (msg_id != 0) counted_ids_.insert(msg_id);
        total_ = total_after;  // ledger lines are in order; last one wins
    }

    void set_total(int64_t t) {
        std::lock_guard<std::mutex> lk(mu_);
        total_ = t;
    }

    int64_t total() const {
        std::lock_guard<std::mutex> lk(mu_);
        return total_;
    }

    size_t counted_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return counted_ids_.size();
    }

    // Value of streaks still in progress (display only; never in the total).
    int64_t pending() const {
        std::lock_guard<std::mutex> lk(mu_);
        int64_t sum = 0;
        for (const auto& [k, v] : in_progress_) sum += v;
        return sum;
    }

private:
    // A combo is identified by (sender, gift).
    static std::string combo_key(const ttlive::Event& e) {
        return std::to_string(e.user.id) + ":" + std::to_string(e.gift_id);
    }

    mutable std::mutex mu_;
    int64_t total_ = 0;
    std::map<std::string, int64_t> in_progress_;   // current combo value, display
    std::unordered_set<int64_t> counted_ids_;      // msg_ids already counted
};

void print_total(const PointsCounter& counter) {
    const int64_t pending = counter.pending();
    std::cout << ">>> TOTAL POINTS: " << counter.total();
    if (pending > 0) std::cout << "  (+" << pending << " streaking)";
    std::cout << "\n" << std::flush;
}

// ---- Debug dump ------------------------------------------------------------

std::string base64_encode(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[v & 63] : '=';
    }
    return out;
}

// Line-oriented dump file. Records:
//   <epoch_ms> <kind> <base64>     raw bytes from TikTok
//   # <epoch_ms> <free text>       human-readable context markers
class DebugDump {
public:
    bool open(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        f_.open(path, std::ios::out | std::ios::app | std::ios::binary);
        return f_.is_open();
    }

    void record(const std::string& kind, const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!f_.is_open()) return;
        f_ << now_ms() << ' ' << kind << ' ' << base64_encode(data, len) << '\n';
        f_.flush();  // survive crashes / Ctrl+C
    }

    void note(const std::string& text) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!f_.is_open()) return;
        f_ << "# " << now_ms() << ' ' << text << '\n';
        f_.flush();
    }

private:
    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::mutex mu_;
    std::ofstream f_;
};

std::string default_dump_name() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "tiktok_dump_%Y%m%d_%H%M%S.log",
                  std::localtime(&t));
    return buf;
}

// ---- Unified ledger --------------------------------------------------------
//
// A single append-only text file recording every *counted* gift (regardless of
// which transport delivered it — the client de-duplicates by msg_id). Each
// line is self-describing so the session state (running total + the set of
// counted msg_ids) can be fully rebuilt on --reconnect.
//
//   # session  <ts> user=<u> mode=<connect|reconnect> start=<n>
//   C <msg_id> <total_after> <value> <gift_id> <repeat> <diamonds> <user> <name>
//   # end      <ts> total=<n>
//
// On a fresh connect the file is truncated; on --reconnect it is replayed to
// restore state and then appended to.
struct LedgerEntry {
    int64_t msg_id = 0;
    int64_t total_after = 0;
};

class Ledger {
public:
    // Replay an existing ledger: returns the counted (msg_id,total) entries in
    // file order. Malformed / partially-written lines are skipped.
    static std::vector<LedgerEntry> replay(const std::string& path) {
        std::vector<LedgerEntry> out;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            char tag = 0;
            ss >> tag;
            if (tag != 'C') continue;
            LedgerEntry e;
            int64_t value, gid, rep, dia;
            if (ss >> e.msg_id >> e.total_after >> value >> gid >> rep >> dia)
                out.push_back(e);
        }
        return out;
    }

    // Open for appending (reconnect) or truncating (fresh connect).
    bool open(const std::string& path, bool append) {
        std::lock_guard<std::mutex> lk(mu_);
        auto mode = std::ios::out | (append ? std::ios::app : std::ios::trunc);
        f_.open(path, mode);
        return f_.is_open();
    }

    void header(const std::string& user, const std::string& mode, int64_t start) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!f_.is_open()) return;
        f_ << "# session " << now_ms() << " user=" << user << " mode=" << mode
           << " start=" << start << '\n';
        f_.flush();
    }

    void count(const ttlive::Event& e, int64_t value, int64_t total_after) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!f_.is_open()) return;
        // Sanitize name/handle (no spaces/newlines) so the line stays parseable.
        f_ << "C " << e.msg_id << ' ' << total_after << ' ' << value << ' '
           << e.gift_id << ' ' << e.repeat_count << ' ' << e.diamond_count << ' '
           << sanitize(e.user.unique_id) << ' ' << sanitize(e.gift_name) << '\n';
        f_.flush();  // durable against a kill
    }

    void end(int64_t total) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!f_.is_open()) return;
        f_ << "# end " << now_ms() << " total=" << total << '\n';
        f_.flush();
    }

    bool is_open() { std::lock_guard<std::mutex> lk(mu_); return f_.is_open(); }

private:
    static std::string sanitize(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char c : s) o += (c == ' ' || c == '\n' || c == '\r') ? '_' : c;
        return o.empty() ? "-" : o;
    }
    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    std::mutex mu_;
    std::ofstream f_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "Usage: %s <@username | username> [options]\n"
                     "Options:\n"
                     "  --start <points>   Current points already on the stream (default 0)\n"
                     "  --room-id <id>     Skip room scraping, connect directly\n"
                     "  --no-live-check    Skip the is-live check\n"
                     "  --cookies \"k=v;..\" Seed cookies\n"
                     "  --transport ws|poll|both  Real-time transport (default ws).\n"
                     "                     ws   = WebSocket (fast, auto-reconnect)\n"
                     "                     poll = HTTP long-polling (more complete)\n"
                     "                     both = run both at once, de-duplicated by\n"
                     "                            msg_id (accurate even if one drops)\n"
                     "  --no-ws            Alias for --transport poll\n"
                     "  --log <file>       Unified ledger of counted gifts (msg_id +\n"
                     "                     running total); required for --reconnect\n"
                     "  --reconnect        Resume from --log: replay it to restore the\n"
                     "                     total and counted msg_ids, then keep appending.\n"
                     "                     Without it a fresh session truncates the log.\n"
                     "  --debug [file]     Dump 100%% of the raw bytes received from TikTok\n"
                     "                     to a file (default tiktok_dump_<date>.log) for\n"
                     "                     offline analysis with tools/decode_dump.py\n"
                     "Examples:\n"
                     "  %s @_for.sera --start 15000 --transport both --log run.ledger\n"
                     "  %s @_for.sera --transport both --log run.ledger --reconnect\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }

    std::string username = argv[1];
    int64_t start_points = 0;
    bool debug = false, reconnect = false;
    std::string debug_file, log_file;
    ttlive::ClientOptions opts;
    // The initial /im/fetch backlog can contain old gift messages from before
    // we started counting — skip them so the baseline stays correct.
    opts.process_connect_events = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--start" && i + 1 < argc) {
            start_points = std::stoll(argv[++i]);
        } else if (a == "--room-id" && i + 1 < argc) {
            opts.room_id_override = std::stoll(argv[++i]);
        } else if (a == "--no-live-check") {
            opts.fetch_live_check = false;
        } else if (a == "--cookies" && i + 1 < argc) {
            opts.cookies = argv[++i];
        } else if (a == "--no-ws") {
            opts.use_websocket = false;
            opts.use_polling = true;
        } else if (a == "--transport" && i + 1 < argc) {
            std::string t = argv[++i];
            if (t == "ws" || t == "websocket") {
                opts.use_websocket = true;  opts.use_polling = false;
            } else if (t == "poll" || t == "polling" || t == "http") {
                opts.use_websocket = false; opts.use_polling = true;
            } else if (t == "both" || t == "dual") {
                opts.use_websocket = true;  opts.use_polling = true;
            } else {
                std::fprintf(stderr,
                             "Unknown --transport '%s' (use ws|poll|both)\n",
                             t.c_str());
                return 2;
            }
        } else if (a == "--log" && i + 1 < argc) {
            log_file = argv[++i];
        } else if (a == "--reconnect") {
            reconnect = true;
        } else if (a == "--debug") {
            debug = true;
            // Optional filename (next arg, unless it's another option).
            if (i + 1 < argc && argv[i + 1][0] != '-') debug_file = argv[++i];
        }
    }

    if (reconnect && log_file.empty()) {
        std::fprintf(stderr, "Error: --reconnect requires --log <file>\n");
        return 2;
    }

    PointsCounter counter(start_points);

    // Unified ledger: on --reconnect, replay it to restore the total and the
    // set of counted msg_ids so nothing is double-counted across restarts; on a
    // fresh connect, truncate it.
    Ledger ledger;
    if (!log_file.empty()) {
        if (reconnect) {
            auto entries = Ledger::replay(log_file);
            for (const auto& en : entries) counter.restore_counted(en.msg_id, en.total_after);
            std::cout << "[RECONNECT] restored " << entries.size()
                      << " counted gifts from " << log_file
                      << "; resuming at " << counter.total() << " points\n";
        }
        if (!ledger.open(log_file, /*append=*/reconnect)) {
            std::fprintf(stderr, "Error: cannot open ledger '%s'\n",
                         log_file.c_str());
            return 1;
        }
        ledger.header(username, reconnect ? "reconnect" : "connect",
                      reconnect ? counter.total() : start_points);
    }

    DebugDump dump;
    if (debug) {
        if (debug_file.empty()) debug_file = default_dump_name();
        if (!dump.open(debug_file)) {
            std::fprintf(stderr, "Error: cannot open debug dump file '%s'\n",
                         debug_file.c_str());
            return 1;
        }
        std::cout << "[DEBUG] dumping all TikTok traffic to " << debug_file << "\n";
        dump.note("pointscount debug dump start user=" + username +
                  " start_points=" + std::to_string(start_points));
        // Everything TikTok sends — raw WS frames and im/fetch bodies before
        // any parsing, plus each decoded message — lands in the file.
        opts.raw_sink = [&dump](const std::string& kind, const uint8_t* data,
                                size_t len) { dump.record(kind, data, len); };
    }

    const char* transport = (opts.use_websocket && opts.use_polling) ? "both (ws+poll)"
                            : opts.use_websocket ? "websocket" : "polling";
    std::cout << "[TRANSPORT] " << transport
              << (log_file.empty() ? "" : "  ledger=" + log_file) << "\n";

    ttlive::TikTokLiveClient client(username, opts);
    g_client = &client;
    std::signal(SIGINT, on_sigint);

    client.on(ttlive::EventType::Connect, [&](const ttlive::Event& e) {
        // NOTE: the counter is created once and is intentionally NOT reset
        // here. TikTok rooms reconnect (the room_id can even change mid-stream
        // when the host restarts the LIVE); the running total must survive
        // reconnects so points already counted are never thrown away.
        std::cout << "[CONNECTED] @" << e.unique_id << " room_id=" << e.room_id
                  << "  total so far " << counter.total() << " points\n";
        dump.note("connected room_id=" + std::to_string(e.room_id) +
                  " total=" + std::to_string(counter.total()));
        print_total(counter);
    });

    client.on(ttlive::EventType::Gift, [&](const ttlive::Event& e) {
        int64_t value = 0;
        PointsCounter::Result r = counter.on_gift(e, value);

        if (r == PointsCounter::Result::Streaking) {
            std::cout << "[STREAK…] " << e.user.nickname << " sending '"
                      << e.gift_name << "' x" << e.repeat_count << " (worth "
                      << value << " so far, not counted yet)\n";
            dump.note("gift STREAKING user=" + e.user.unique_id +
                      " gift_id=" + std::to_string(e.gift_id) + " name='" +
                      e.gift_name + "' x" + std::to_string(e.repeat_count) +
                      " type=" + std::to_string(e.gift_type) +
                      " diamonds=" + std::to_string(e.diamond_count) +
                      " NOT_COUNTED total=" + std::to_string(counter.total()));
            print_total(counter);
            return;
        }
        if (r == PointsCounter::Result::Duplicate) {
            // Already counted (other transport / replayed / reloaded). Silent.
            dump.note("gift DUP msg_id=" + std::to_string(e.msg_id) +
                      " user=" + e.user.unique_id + " +0 (already counted)");
            return;
        }

        // Counted.
        const int64_t total = counter.total();
        if (ledger.is_open()) ledger.count(e, value, total);
        std::cout << "[GIFT] " << e.user.nickname << " sent '" << e.gift_name
                  << "' x" << e.repeat_count << " = +" << value << " points\n";
        dump.note("gift COMMIT msg_id=" + std::to_string(e.msg_id) +
                  " user=" + e.user.unique_id +
                  " gift_id=" + std::to_string(e.gift_id) + " name='" +
                  e.gift_name + "' x" + std::to_string(e.repeat_count) +
                  " type=" + std::to_string(e.gift_type) +
                  " diamonds=" + std::to_string(e.diamond_count) + " +" +
                  std::to_string(value) + " total=" + std::to_string(total));
        print_total(counter);
    });

    client.on(ttlive::EventType::LiveEnd, [&](const ttlive::Event&) {
        std::cout << "[LIVE ENDED]\n";
        print_total(counter);
    });

    client.on(ttlive::EventType::Disconnect, [&](const ttlive::Event&) {
        std::cout << "[DISCONNECTED] final total: " << counter.total() << "\n";
        dump.note("disconnected final_total=" + std::to_string(counter.total()));
        if (ledger.is_open()) ledger.end(counter.total());
    });

    try {
        client.run();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
