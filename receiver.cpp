// receiver.cpp — Iterative FEC solver + NACK ARQ receiver
// Compiles as C++17. Build: make
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// ── Constants ──────────────────────────────────────────────────────────
constexpr uint8_t PKT_DATA              = 1;
constexpr uint8_t PKT_PARITY            = 2;
constexpr uint8_t PKT_NACK              = 3;
constexpr uint8_t PKT_PARITY_INTERLEAVED = 4;

constexpr int PAYLOAD_BYTES = 160;
constexpr int HIST_SIZE     = 2048;
constexpr int WIRE_PKT_SIZE = 1 + 4 + PAYLOAD_BYTES;

// ── Wire formats ───────────────────────────────────────────────────────
struct __attribute__((packed)) WireData {
    uint8_t  type;
    uint32_t seq;
    uint8_t  payload[PAYLOAD_BYTES];
};
static_assert(sizeof(WireData) == WIRE_PKT_SIZE, "WireData must be 165 bytes");

struct __attribute__((packed)) WireNack {
    uint8_t  type;
    uint8_t  count;
    uint32_t seqs[30];
};

// ── State ──────────────────────────────────────────────────────────────
static bool    has_data[HIST_SIZE];
static uint8_t data_store[HIST_SIZE][PAYLOAD_BYTES];

static bool    has_parity_c[HIST_SIZE];   // consecutive parity
static uint8_t parity_c_store[HIST_SIZE][PAYLOAD_BYTES];

static bool    has_parity_i[HIST_SIZE];   // interleaved parity
static uint8_t parity_i_store[HIST_SIZE][PAYLOAD_BYTES];

static int    max_seq_seen = -1;
static double last_nack_time = 0.0;

// Player socket (global for helper functions)
static int         player_fd;
static sockaddr_in player_addr;

// ── Helpers ────────────────────────────────────────────────────────────
static double now_sec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1'000'000.0;
}

static void emit_to_player(uint32_t seq, const uint8_t* payload) {
    uint8_t buf[164];
    uint32_t net_seq = htonl(seq);
    std::memcpy(buf, &net_seq, 4);
    std::memcpy(buf + 4, payload, PAYLOAD_BYTES);
    sendto(player_fd, buf, 164, 0,
           reinterpret_cast<sockaddr*>(&player_addr), sizeof(player_addr));
}

// ── FEC solver ─────────────────────────────────────────────────────────
// Try to solve a single parity equation. If exactly one member is missing,
// recover it by XOR-ing all known members against the parity payload.
static void try_solve(const bool* has_par, const uint8_t (*par_store)[PAYLOAD_BYTES],
                      uint32_t eq_seq, const uint32_t* members, int n,
                      bool& changed) {
    if (!has_par[eq_seq % HIST_SIZE]) return;

    int      missing_n   = 0;
    uint32_t missing_seq = 0;

    for (int j = 0; j < n; ++j) {
        if (!has_data[members[j] % HIST_SIZE]) {
            ++missing_n;
            missing_seq = members[j];
        }
    }
    if (missing_n != 1) return;

    // Recover the single missing frame
    uint8_t recovered[PAYLOAD_BYTES];
    std::memcpy(recovered, par_store[eq_seq % HIST_SIZE], PAYLOAD_BYTES);
    for (int j = 0; j < n; ++j) {
        if (members[j] != missing_seq) {
            for (int b = 0; b < PAYLOAD_BYTES; ++b)
                recovered[b] ^= data_store[members[j] % HIST_SIZE][b];
        }
    }

    std::memcpy(data_store[missing_seq % HIST_SIZE], recovered, PAYLOAD_BYTES);
    has_data[missing_seq % HIST_SIZE] = true;
    emit_to_player(missing_seq, recovered);
    changed = true;
}

// Run iterative solver until no more equations can be resolved.
static void run_solver() {
    bool changed = true;
    while (changed) {
        changed = false;

        uint32_t lo = (max_seq_seen > 200) ? max_seq_seen - 200 : 0;
        uint32_t hi = max_seq_seen + 10;

        for (uint32_t i = lo; i <= hi; ++i) {
            // Consecutive parity: covers {i, i-1, i-2, i-3}
            {
                uint32_t m[4]; int n = 0;
                for (int j = 0; j <= 3; ++j)
                    if (i >= static_cast<uint32_t>(j)) m[n++] = i - j;
                if (n > 0) try_solve(has_parity_c, parity_c_store, i, m, n, changed);
            }
            // Interleaved parity: covers {i, i-2, i-4, i-6}
            {
                uint32_t m[4]; int n = 0;
                for (int j = 0; j <= 3; ++j) {
                    uint32_t off = j * 2;
                    if (i >= off) m[n++] = i - off;
                }
                if (n > 0) try_solve(has_parity_i, parity_i_store, i, m, n, changed);
            }
        }
    }
}

// ── Slot management ────────────────────────────────────────────────────
static void advance_watermark(uint32_t seq) {
    if (static_cast<int>(seq) <= max_seq_seen) return;
    max_seq_seen = seq;
    // Evict stale slots that will be reused
    for (int i = 1; i <= 50; ++i) {
        uint32_t old = seq + HIST_SIZE / 2 + i;
        has_data[old % HIST_SIZE]     = false;
        has_parity_c[old % HIST_SIZE] = false;
        has_parity_i[old % HIST_SIZE] = false;
    }
}

// ── Main ───────────────────────────────────────────────────────────────
int main() {
    // Socket: relay → receiver (port 47002)
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, reinterpret_cast<sockaddr*>(&in_addr), sizeof(in_addr)) < 0) {
        perror("bind 47002"); return 1;
    }

    // Socket: receiver → harness player (port 47020)
    player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    std::memset(&player_addr, 0, sizeof(player_addr));
    player_addr.sin_family      = AF_INET;
    player_addr.sin_port        = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Socket: receiver → relay feedback (port 47003)
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in fb_addr{};
    fb_addr.sin_family      = AF_INET;
    fb_addr.sin_port        = htons(47003);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Init state
    std::memset(has_data,     0, sizeof(has_data));
    std::memset(has_parity_c, 0, sizeof(has_parity_c));
    std::memset(has_parity_i, 0, sizeof(has_parity_i));
    last_nack_time = now_sec();

    uint8_t buf[2048];

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        timeval tv{0, 5000};  // 5 ms

        int ready = select(in_fd + 1, &rfds, nullptr, nullptr, &tv);

        // ── Process incoming packet ────────────────────────────────────
        if (ready > 0 && FD_ISSET(in_fd, &rfds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n >= WIRE_PKT_SIZE) {
                auto* pkt = reinterpret_cast<WireData*>(buf);
                uint32_t seq = ntohl(pkt->seq);
                advance_watermark(seq);

                switch (pkt->type) {
                case PKT_DATA:
                    if (!has_data[seq % HIST_SIZE]) {
                        has_data[seq % HIST_SIZE] = true;
                        std::memcpy(data_store[seq % HIST_SIZE], pkt->payload, PAYLOAD_BYTES);
                        emit_to_player(seq, pkt->payload);
                        run_solver();
                    }
                    break;

                case PKT_PARITY:
                    if (!has_parity_c[seq % HIST_SIZE]) {
                        has_parity_c[seq % HIST_SIZE] = true;
                        std::memcpy(parity_c_store[seq % HIST_SIZE], pkt->payload, PAYLOAD_BYTES);
                        run_solver();
                    }
                    break;

                case PKT_PARITY_INTERLEAVED:
                    if (!has_parity_i[seq % HIST_SIZE]) {
                        has_parity_i[seq % HIST_SIZE] = true;
                        std::memcpy(parity_i_store[seq % HIST_SIZE], pkt->payload, PAYLOAD_BYTES);
                        run_solver();
                    }
                    break;
                }
            }
        }

        // ── Periodic NACK generation ───────────────────────────────────
        double now = now_sec();
        if (now - last_nack_time >= 0.005) {
            last_nack_time = now;

            WireNack nack{};
            nack.type  = PKT_NACK;
            nack.count = 0;

            int start = (max_seq_seen > 100) ? max_seq_seen - 100 : 0;
            for (int i = start; i < max_seq_seen; ++i) {
                if (!has_data[i % HIST_SIZE]) {
                    nack.seqs[nack.count++] = htonl(i);
                    if (nack.count == 30) break;
                }
            }

            if (nack.count > 0) {
                sendto(fb_fd, &nack, 2 + 4 * nack.count, 0,
                       reinterpret_cast<sockaddr*>(&fb_addr), sizeof(fb_addr));
            }
        }
    }
}
