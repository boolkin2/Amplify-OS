// ============================================================================
// ping.c — отправка ICMP Echo Request и ожидание Reply.
// ============================================================================

#include "ping.h"
#include "icmp.h"
#include "../drivers/timer.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stdbool.h>

static volatile uint16_t g_pending_id  = 0;
static volatile uint16_t g_pending_seq = 0;
static volatile bool     g_got         = false;

static void on_reply(uint32_t src_ip, uint16_t id, uint16_t seq) {
    (void)src_ip;
    if (id == g_pending_id && seq == g_pending_seq) {
        g_got = true;
    }
}

int ping_one(uint32_t dst_ip, uint16_t seq) {
    static const char payload[] = "AOSPING-AMPLIFY-OS-1234";
    uint16_t id = 0xA05F;

    g_pending_id  = id;
    g_pending_seq = seq;
    g_got         = false;

    icmp_set_reply_cb(on_reply);

    uint32_t t0 = (uint32_t)timer_get_uptime_ms();
    if (!icmp_send_echo(dst_ip, id, seq, payload, sizeof(payload) - 1)) {
        icmp_set_reply_cb(0);
        return -1;
    }

    while (!g_got) {
        if ((uint32_t)timer_get_uptime_ms() - t0 > PING_TIMEOUT_MS) {
            icmp_set_reply_cb(0);
            return -1;
        }
        __asm__ volatile("sti; hlt");
    }
    int rtt = (int)((uint32_t)timer_get_uptime_ms() - t0);
    icmp_set_reply_cb(0);
    return rtt;
}
