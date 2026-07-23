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
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
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

void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

double get_time_s() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + (tv.tv_usec / 1000000.0);
}

int main() {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(in_fd, (sockaddr *)&in_addr, sizeof(in_addr));
    set_nonblocking(in_fd);

    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in nack_addr{};
    nack_addr.sin_family = AF_INET;
    nack_addr.sin_port = htons(47004);
    nack_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(nack_fd, (sockaddr *)&nack_addr, sizeof(nack_addr));
    set_nonblocking(nack_fd);

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in relay{};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::vector<HarnessPacket> history(65536);
    std::vector<bool> sent(65536, false);
    std::vector<double> last_resend(65536, 0.0);

    HarnessPacket pkt;
    uint32_t nack_seq;
    int total_up_bytes = 0;
    const int MAX_UP_BYTES = 475000; // Strictly under 480k (2.0x cap)

    while (true) {
        bool activity = false;
        
        while (recvfrom(in_fd, &pkt, sizeof(pkt), 0, nullptr, nullptr) == sizeof(HarnessPacket)) {
            activity = true;
            uint32_t seq = ntohl(pkt.seq);
            if (seq < 65536) {
                history[seq] = pkt;
                sent[seq] = true;
                
                // 50% FEC Coverage (seq % 2 != 0)
                if (seq > 0 && (seq % 2 != 0) && (total_up_bytes + sizeof(WirePacket) <= MAX_UP_BYTES)) {
                    WirePacket out_pkt;
                    out_pkt.seq = pkt.seq;
                    std::memcpy(out_pkt.payload, pkt.payload, 160);
                    std::memcpy(out_pkt.prev_payload, history[seq-1].payload, 160);
                    sendto(out_fd, &out_pkt, sizeof(out_pkt), 0, (sockaddr *)&relay, sizeof(relay));
                    total_up_bytes += sizeof(WirePacket);
                } else if (total_up_bytes + sizeof(HarnessPacket) <= MAX_UP_BYTES) {
                    sendto(out_fd, &pkt, sizeof(pkt), 0, (sockaddr *)&relay, sizeof(relay));
                    total_up_bytes += sizeof(HarnessPacket);
                }
            }
        }

        double now = get_time_s();
        while (recvfrom(nack_fd, &nack_seq, sizeof(nack_seq), 0, nullptr, nullptr) == sizeof(uint32_t)) {
            activity = true;
            uint32_t missing = ntohl(nack_seq);
            if (missing < 65536 && sent[missing]) {
                if (total_up_bytes + sizeof(HarnessPacket) <= MAX_UP_BYTES) {
                    if (now - last_resend[missing] > 0.020) {
                        sendto(out_fd, &history[missing], sizeof(HarnessPacket), 0, (sockaddr *)&relay, sizeof(relay));
                        total_up_bytes += sizeof(HarnessPacket);
                        last_resend[missing] = now;
                    }
                }
            }
        }
        
        if (!activity) usleep(150); 
    }
    return 0;
}