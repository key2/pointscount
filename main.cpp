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
#include <string>

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

    // Returns true if the event changed the committed total. `counted` tells
    // the caller whether this gift's value was added (vs an ignored
    // intermediate streak frame).
    bool on_gift(const ttlive::Event& e, bool& counted) {
        std::lock_guard<std::mutex> lk(mu_);
        const bool streakable = (e.gift_type == 1);
        const int64_t value =
            static_cast<int64_t>(e.diamond_count) * std::max(e.repeat_count, 1);

        // Intermediate frame of a streak: display-only, never counted.
        if (streakable && e.gift_streaking) {
            in_progress_[key(e)] = value;  // for the live "streaking" figure
            counted = false;
            return false;
        }

        // Final frame of a streak (repeat_end == 1) or a non-streakable gift.
        if (streakable) in_progress_.erase(key(e));
        total_ += value;
        counted = true;
        return true;
    }

    int64_t total() const {
        std::lock_guard<std::mutex> lk(mu_);
        return total_;
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
    static std::string key(const ttlive::Event& e) {
        return std::to_string(e.user.id) + ":" + std::to_string(e.gift_id);
    }

    mutable std::mutex mu_;
    int64_t total_ = 0;
    std::map<std::string, int64_t> in_progress_;  // current combo value, display
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
                     "  --transport ws|poll  Real-time transport (default ws).\n"
                     "                     ws   = WebSocket (low latency, auto-reconnect)\n"
                     "                     poll = HTTP long-polling\n"
                     "  --no-ws            Alias for --transport poll\n"
                     "  --debug [file]     Dump 100%% of the raw bytes received from TikTok\n"
                     "                     to a file (default tiktok_dump_<date>.log) for\n"
                     "                     offline analysis with tools/decode_dump.py\n"
                     "Example: %s @sandrahensley7197 --start 15000 --debug\n",
                     argv[0], argv[0]);
        return 2;
    }

    std::string username = argv[1];
    int64_t start_points = 0;
    bool debug = false;
    std::string debug_file;
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
        } else if (a == "--transport" && i + 1 < argc) {
            std::string t = argv[++i];
            if (t == "ws" || t == "websocket") {
                opts.use_websocket = true;
            } else if (t == "poll" || t == "polling" || t == "http") {
                opts.use_websocket = false;
            } else {
                std::fprintf(stderr, "Unknown --transport '%s' (use ws|poll)\n",
                             t.c_str());
                return 2;
            }
        } else if (a == "--debug") {
            debug = true;
            // Optional filename (next arg, unless it's another option).
            if (i + 1 < argc && argv[i + 1][0] != '-') debug_file = argv[++i];
        }
    }

    PointsCounter counter(start_points);

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
        const int64_t value =
            static_cast<int64_t>(e.diamond_count) * std::max(e.repeat_count, 1);

        bool counted = false;
        counter.on_gift(e, counted);

        if (!counted) {
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

        std::cout << "[GIFT] " << e.user.nickname << " sent '" << e.gift_name
                  << "' x" << e.repeat_count << " = +" << value << " points\n";
        dump.note("gift COMMIT user=" + e.user.unique_id +
                  " gift_id=" + std::to_string(e.gift_id) + " name='" +
                  e.gift_name + "' x" + std::to_string(e.repeat_count) +
                  " type=" + std::to_string(e.gift_type) +
                  " diamonds=" + std::to_string(e.diamond_count) + " +" +
                  std::to_string(value) +
                  " total=" + std::to_string(counter.total()));
        print_total(counter);
    });

    client.on(ttlive::EventType::LiveEnd, [&](const ttlive::Event&) {
        std::cout << "[LIVE ENDED]\n";
        print_total(counter);
    });

    client.on(ttlive::EventType::Disconnect, [&](const ttlive::Event&) {
        std::cout << "[DISCONNECTED] final total: " << counter.total() << "\n";
        dump.note("disconnected final_total=" + std::to_string(counter.total()));
    });

    try {
        client.run();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
