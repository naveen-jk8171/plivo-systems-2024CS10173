/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This baseline forwards whatever arrives straight to the player: lost
 * frames stay lost, late frames stay late, duplicates are re-sent
 * harmlessly. All yours to fix — jitter buffer, reordering, recovery.
 *
 * Env vars available: T0, DURATION_S, DELAY_MS. Harness kills the process
 * at run end; a forever-loop is fine.
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
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (sockaddr *)&in_addr, sizeof(in_addr));

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in player{};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];

    while (true) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) continue;

        WirePacket* pkt = reinterpret_cast<WirePacket*>(buf);
        uint32_t seq = ntohl(pkt->seq);

        // 1. Send the primary frame immediately to the player
        HarnessPacket out1;
        out1.seq = pkt->seq; 
        std::memcpy(out1.payload, pkt->payload, 160);
        sendto(out_fd, &out1, sizeof(HarnessPacket), 0, (sockaddr *)&player, sizeof(player));

        // 2. If the packet contains the redundant payload, send the previous frame immediately
        if (n == sizeof(WirePacket) && seq > 0) {
            HarnessPacket out2;
            out2.seq = htonl(seq - 1);
            std::memcpy(out2.payload, pkt->prev_payload, 160);
            sendto(out_fd, &out2, sizeof(HarnessPacket), 0, (sockaddr *)&player, sizeof(player));
        }
    }
    return 0;
}