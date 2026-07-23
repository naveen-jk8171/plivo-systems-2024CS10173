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
#include <sys/time.h>
#include <fcntl.h>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>

struct __attribute__((packed)) HarnessPacket {
    uint32_t seq;
    unsigned char payload[160];
};

struct __attribute__((packed)) WirePacket {
    uint32_t seq;
    unsigned char payload[160];
    unsigned char prev_payload[160];
};

double get_time_s() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + (tv.tv_usec / 1000000.0);
}

void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int main() {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(in_fd, (sockaddr *)&in_addr, sizeof(in_addr));
    set_nonblocking(in_fd);

    int player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in player{};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in feedback{};
    feedback.sin_family = AF_INET;
    feedback.sin_port = htons(47003);
    feedback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::vector<bool> received(65536, false);
    std::vector<double> last_nack_time(65536, 0.0);
    std::vector<int> nack_count(65536, 0);
    
    int highest_seq = -1;
    int first_missing = 0;
    double t0 = 0.0;
    unsigned char buf[2048];
    
    while (true) {
        if (t0 == 0.0) {
            char* t0_env = getenv("T0");
            if (t0_env) t0 = atof(t0_env);
        }

        bool processed = false;
        ssize_t n;
        
        while ((n = recvfrom(in_fd, buf, sizeof(buf), 0, nullptr, nullptr)) > 0) {
            processed = true;
            uint32_t seq = 0;
            
            // 1. EXTRACT FROM FEC WIRE PACKET
            if (n == sizeof(WirePacket)) {
                WirePacket* wpkt = reinterpret_cast<WirePacket*>(buf);
                seq = ntohl(wpkt->seq);
                
                if (seq < 65536) {
                    if (!received[seq]) {
                        received[seq] = true;
                        HarnessPacket p;
                        p.seq = wpkt->seq;
                        std::memcpy(p.payload, wpkt->payload, 160);
                        sendto(player_fd, &p, sizeof(p), 0, (sockaddr *)&player, sizeof(player));
                    }
                    if (seq > 0 && !received[seq - 1]) {
                        received[seq - 1] = true;
                        HarnessPacket p;
                        p.seq = htonl(seq - 1);
                        std::memcpy(p.payload, wpkt->prev_payload, 160);
                        sendto(player_fd, &p, sizeof(p), 0, (sockaddr *)&player, sizeof(player));
                    }
                }
            } 
            // 2. EXTRACT FROM ARQ RESEND PACKET
            else if (n == sizeof(HarnessPacket)) {
                HarnessPacket* hpkt = reinterpret_cast<HarnessPacket*>(buf);
                seq = ntohl(hpkt->seq);
                
                if (seq < 65536) {
                    if (!received[seq]) {
                        received[seq] = true;
                        sendto(player_fd, hpkt, sizeof(HarnessPacket), 0, (sockaddr *)&player, sizeof(player));
                    }
                }
            }

            if (seq < 65536 && (int)seq > highest_seq) {
                highest_seq = seq;
            }
        }

        while (first_missing < 65536 && received[first_missing]) {
            first_missing++;
        }

        double now = get_time_s();
        
        // 3. INSTANT ARQ LOGIC: If a sequence is still missing behind our highest marker, FEC failed.
        for (int i = first_missing; i < highest_seq; i++) {
            if (!received[i] && nack_count[i] == 0) {
                uint32_t nack_seq = htonl(i);
                sendto(fb_fd, &nack_seq, sizeof(nack_seq), 0, (sockaddr *)&feedback, sizeof(feedback));
                last_nack_time[i] = now;
                nack_count[i]++;
            }
        }

        // 4. TIME-BASED FALLBACK: For packets at the absolute end of the stream without trailing packets to trigger gaps
        if (t0 > 0.0) {
            int time_expected = (now - t0 - 0.035) / 0.020; 
            int check_max = std::max(highest_seq, time_expected);
            if (check_max > 65535) check_max = 65535;

            for (int i = first_missing; i <= check_max; i++) {
                if (!received[i] && nack_count[i] < 2) {
                    if (now - last_nack_time[i] > 0.025) { 
                        uint32_t nack_seq = htonl(i);
                        sendto(fb_fd, &nack_seq, sizeof(nack_seq), 0, (sockaddr *)&feedback, sizeof(feedback));
                        last_nack_time[i] = now;
                        nack_count[i]++;
                    }
                }
            }
        }

        if (!processed) usleep(200); 
    }
    return 0;
}