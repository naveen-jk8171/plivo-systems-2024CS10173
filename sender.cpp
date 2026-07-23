// sender.cpp — Hybrid FEC + ARQ sender
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
constexpr int WIRE_PKT_SIZE = 1 + 4 + PAYLOAD_BYTES; // 165 bytes

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

// ── Frame history ──────────────────────────────────────────────────────
static uint8_t history[HIST_SIZE][PAYLOAD_BYTES];
static double  last_retransmit[HIST_SIZE];
static int     max_seq = -1;

// ── Token bucket ───────────────────────────────────────────────────────
static double tokens         = 0.0;
static constexpr double MAX_TOKENS    = 700.0;
static constexpr double TOKEN_RATE    = 15600.0;  // bytes/sec ≈ 1.95× raw
static double last_token_time = 0.0;

// ── Helpers ────────────────────────────────────────────────────────────
static double now_sec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1'000'000.0;
}

static void refill_tokens() {
    double now = now_sec();
    tokens += (now - last_token_time) * TOKEN_RATE;
    if (tokens > MAX_TOKENS) tokens = MAX_TOKENS;
    last_token_time = now;
}

static bool spend(double cost) {
    if (tokens >= cost) { tokens -= cost; return true; }
    return false;
}

// ── Main ───────────────────────────────────────────────────────────────
int main() {
    // Socket: harness source → sender (port 47010)
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, reinterpret_cast<sockaddr*>(&in_addr), sizeof(in_addr)) < 0) {
        perror("bind 47010"); return 1;
    }

    // Socket: feedback from receiver via relay (port 47004)
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in fb_addr{};
    fb_addr.sin_family      = AF_INET;
    fb_addr.sin_port        = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, reinterpret_cast<sockaddr*>(&fb_addr), sizeof(fb_addr)) < 0) {
        perror("bind 47004"); return 1;
    }

    // Socket: sender → relay (port 47001)
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in relay{};
    relay.sin_family      = AF_INET;
    relay.sin_port        = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Init
    last_token_time = now_sec();
    for (int i = 0; i < HIST_SIZE; ++i) last_retransmit[i] = 0.0;
    int max_fd = (in_fd > fb_fd) ? in_fd : fb_fd;

    uint8_t buf[2048];

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        FD_SET(fb_fd, &rfds);
        timeval tv{0, 5000};  // 5 ms

        int ready = select(max_fd + 1, &rfds, nullptr, nullptr, &tv);
        refill_tokens();

        if (ready <= 0) continue;

        // ── Harness frame arrived ──────────────────────────────────────
        if (FD_ISSET(in_fd, &rfds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n < 164) continue;  // malformed

            uint32_t seq;
            std::memcpy(&seq, buf, 4);
            seq = ntohl(seq);

            // Store in history
            std::memcpy(history[seq % HIST_SIZE], buf + 4, PAYLOAD_BYTES);
            if (static_cast<int>(seq) > max_seq) max_seq = seq;

            // 1) Always send the DATA packet
            WireData d{};
            d.type = PKT_DATA;
            d.seq  = htonl(seq);
            std::memcpy(d.payload, buf + 4, PAYLOAD_BYTES);
            sendto(out_fd, &d, sizeof(d), 0,
                   reinterpret_cast<sockaddr*>(&relay), sizeof(relay));
            tokens -= sizeof(d);

            // 2) Send consecutive parity if budget allows
            if (spend(WIRE_PKT_SIZE)) {
                WireData p{};
                p.type = PKT_PARITY;
                p.seq = htonl(seq);
                std::memset(p.payload, 0, PAYLOAD_BYTES);
                for (int j = 0; j <= 3; ++j) {
                    if (static_cast<int>(seq) >= j) {
                        uint32_t s = seq - j;
                        for (int b = 0; b < PAYLOAD_BYTES; ++b)
                            p.payload[b] ^= history[s % HIST_SIZE][b];
                    }
                }
                sendto(out_fd, &p, sizeof(p), 0,
                       reinterpret_cast<sockaddr*>(&relay), sizeof(relay));
            }

            // 3) Send interleaved parity too if budget still allows
            if (spend(WIRE_PKT_SIZE)) {
                WireData p{};
                p.type = PKT_PARITY_INTERLEAVED;
                p.seq = htonl(seq);
                std::memset(p.payload, 0, PAYLOAD_BYTES);
                for (int j = 0; j <= 3; ++j) {
                    int offset = j * 2;
                    if (static_cast<int>(seq) >= offset) {
                        uint32_t s = seq - offset;
                        for (int b = 0; b < PAYLOAD_BYTES; ++b)
                            p.payload[b] ^= history[s % HIST_SIZE][b];
                    }
                }
                sendto(out_fd, &p, sizeof(p), 0,
                       reinterpret_cast<sockaddr*>(&relay), sizeof(relay));
            }
        }

        // ── NACK feedback arrived ──────────────────────────────────────
        if (FD_ISSET(fb_fd, &rfds)) {
            ssize_t n = recvfrom(fb_fd, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n < 2 || buf[0] != PKT_NACK) continue;

            auto* nack = reinterpret_cast<WireNack*>(buf);
            int count = std::min(static_cast<int>(nack->count), 30);
            double now = now_sec();

            for (int i = 0; i < count; ++i) {
                uint32_t s = ntohl(nack->seqs[i]);
                if (static_cast<int>(s) > max_seq) continue;
                if (max_seq >= HIST_SIZE && s < static_cast<uint32_t>(max_seq - HIST_SIZE + 1)) continue;

                // Throttle: at most once per 20 ms per sequence
                if (now - last_retransmit[s % HIST_SIZE] < 0.02) continue;
                if (!spend(WIRE_PKT_SIZE)) break;  // no budget left

                WireData r{};
                r.type = PKT_DATA;
                r.seq  = htonl(s);
                std::memcpy(r.payload, history[s % HIST_SIZE], PAYLOAD_BYTES);
                sendto(out_fd, &r, sizeof(r), 0,
                       reinterpret_cast<sockaddr*>(&relay), sizeof(relay));
                last_retransmit[s % HIST_SIZE] = now;
            }
        }
    }
}
