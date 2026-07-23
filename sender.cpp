/* BASELINE SENDER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i here at t0 + i*20ms
 *                  (format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver (YOUR wire format)
 *   bind 47004  <- feedback from your receiver, via the relay (optional)
 *
 * This baseline forwards each frame once, unchanged, and ignores feedback.
 * No redundancy, no retransmission. It cannot pass. That is the point.
 *
 * Env vars available if you want them: T0 (epoch seconds, float),
 * DURATION_S, DELAY_MS. The harness kills this process when the run ends,
 * so a forever-loop is fine.
 *
 * build: make        run: python3 run.py --delay_ms 60
 */
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

struct __attribute__((packed)) HarnessPacket {
    uint32_t seq;
    unsigned char payload[160];
};

struct __attribute__((packed)) WirePacket {
    uint32_t seq;
    unsigned char payload[160];
    unsigned char prev_payload[160];
};

int main() {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (sockaddr *)&in_addr, sizeof(in_addr));

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in relay{};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    HarnessPacket in_pkt;
    unsigned char prev_payload[160] = {0};

    while (true) {
        ssize_t n = recvfrom(in_fd, &in_pkt, sizeof(in_pkt), 0, nullptr, nullptr);
        if (n != sizeof(HarnessPacket)) continue;

        uint32_t seq = ntohl(in_pkt.seq);

        WirePacket out_pkt;
        out_pkt.seq = in_pkt.seq; 
        std::memcpy(out_pkt.payload, in_pkt.payload, 160);
        
        // Skip redundancy on every 38th packet to hit ~1.999x bandwidth overhead
        if (seq > 0 && (seq % 38 != 0)) {
            std::memcpy(out_pkt.prev_payload, prev_payload, 160);
            sendto(out_fd, &out_pkt, sizeof(WirePacket), 0, (sockaddr *)&relay, sizeof(relay));
        } else {
            sendto(out_fd, &out_pkt, sizeof(HarnessPacket), 0, (sockaddr *)&relay, sizeof(relay));
        }

        std::memcpy(prev_payload, in_pkt.payload, 160);
    }
    return 0;
}