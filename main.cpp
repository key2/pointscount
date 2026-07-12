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
// Usage: pointscount <@username> [--start N] [options passed to ttlive]

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

struct PendingStreak {
    std::string gift_name;
    int32_t repeat_count = 0;
    int64_t diamond_count = 0;
};

class PointsCounter {
public:
    explicit PointsCounter(int64_t start) : total_(start) {}

    // Returns true if the event changed the committed total.
    bool on_gift(const ttlive::Event& e) {
        std::lock_guard<std::mutex> lk(mu_);
        const bool streakable = (e.gift_type == 1);
        const int64_t value =
            static_cast<int64_t>(e.diamond_count) * std::max(e.repeat_count, 1);

        if (streakable && e.gift_streaking) {
            // Intermediate combo update: remember it, do NOT count it.
            PendingStreak& p = pending_[key(e)];
            p.gift_name = e.gift_name;
            p.repeat_count = e.repeat_count;
            p.diamond_count = e.diamond_count;
            return false;
        }

        // Final message of a streak (repeat_end == 1) carries the full combo
        // total in repeat_count — commit it once. Non-streakable gifts are
        // committed as-is.
        if (streakable) pending_.erase(key(e));
        total_ += value;
        return true;
    }

    int64_t total() const {
        std::lock_guard<std::mutex> lk(mu_);
        return total_;
    }

    // Value of streaks still in progress (not yet committed).
    int64_t pending() const {
        std::lock_guard<std::mutex> lk(mu_);
        int64_t sum = 0;
        for (const auto& [k, p] : pending_)
            sum += p.diamond_count * std::max(p.repeat_count, 1);
        return sum;
    }

private:
    // A combo is identified by (sender, gift). TikTok restarts repeat_count at
    // 1 for a new combo, and the final frame closes the entry.
    static std::string key(const ttlive::Event& e) {
        return std::to_string(e.user.id) + ":" + std::to_string(e.gift_id);
    }

    mutable std::mutex mu_;
    int64_t total_ = 0;
    std::map<std::string, PendingStreak> pending_;
};

void print_total(const PointsCounter& counter) {
    const int64_t pending = counter.pending();
    std::cout << ">>> TOTAL POINTS: " << counter.total();
    if (pending > 0) std::cout << "  (+" << pending << " streaking)";
    std::cout << "\n" << std::flush;
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
                     "  --no-ws            Use HTTP long-polling instead of WebSocket\n"
                     "Example: %s @sandrahensley7197 --start 15000\n",
                     argv[0], argv[0]);
        return 2;
    }

    std::string username = argv[1];
    int64_t start_points = 0;
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
        }
    }

    PointsCounter counter(start_points);

    ttlive::TikTokLiveClient client(username, opts);
    g_client = &client;
    std::signal(SIGINT, on_sigint);

    client.on(ttlive::EventType::Connect, [&](const ttlive::Event& e) {
        std::cout << "[CONNECTED] @" << e.unique_id << " room_id=" << e.room_id
                  << "  starting from " << start_points << " points\n";
        print_total(counter);
    });

    client.on(ttlive::EventType::Gift, [&](const ttlive::Event& e) {
        const bool streakable = (e.gift_type == 1);
        const int64_t value =
            static_cast<int64_t>(e.diamond_count) * std::max(e.repeat_count, 1);

        if (streakable && e.gift_streaking) {
            counter.on_gift(e);
            std::cout << "[STREAK…] " << e.user.nickname << " sending '"
                      << e.gift_name << "' x" << e.repeat_count << " (worth "
                      << value << " so far, not counted yet)\n";
            print_total(counter);
            return;
        }

        counter.on_gift(e);
        std::cout << "[GIFT] " << e.user.nickname << " sent '" << e.gift_name
                  << "' x" << e.repeat_count << " = +" << value << " points\n";
        print_total(counter);
    });

    client.on(ttlive::EventType::LiveEnd, [&](const ttlive::Event&) {
        std::cout << "[LIVE ENDED]\n";
        print_total(counter);
    });

    client.on(ttlive::EventType::Disconnect, [&](const ttlive::Event&) {
        std::cout << "[DISCONNECTED] final total: " << counter.total() << "\n";
    });

    try {
        client.run();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
