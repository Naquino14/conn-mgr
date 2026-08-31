#include "conn-mgr.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/icmp.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/sys/reboot.h>

// see sample:
// https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/net/wifi/apsta_mode/src/main.c

LOG_MODULE_REGISTER(conn_mgr);

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"

#define NET_EVENT_WIFI_MASK                                                    \
	    (NET_EVENT_WIFI_CONNECT_RESULT   | NET_EVENT_WIFI_DISCONNECT_RESULT |  \
	     NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |  \
	     NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED)

#define DNS_TIMEOUT_MS 2000
#define CONN_WATCHDOG_PERIOD K_SECONDS(5)
#define CONN_WATCHDOG_MAX_MISSES 12

static int watchdog_enabled = false;
static struct k_work_delayable conn_watchdog_work;
static uint32_t watchdog_disconnect_count;

// configs for wifi station mode
static struct net_if *sta_iface;
static struct wifi_connect_req_params sta_wifi_config;

// configs for pings
static struct k_sem ping_sem;
static struct net_icmp_ctx icmp_ctx;

// callbacks
static struct net_mgmt_event_callback net_mgmt_event_cb;

// flags
static bool initd = false;
static bool connected = false;

static void conn_watchdog_handler(struct k_work *work);

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event_code, struct net_if *iface) {
    switch (event_code) {
        case NET_EVENT_WIFI_CONNECT_RESULT:
            const struct wifi_status *status = (const struct wifi_status *)cb->info;
            if (status && status->status) {
                LOG_ERR("WiFi connection failed with status %d", status->status);
                connected = false;
                break;
            } else {
                LOG_INF("WiFi connected");
                connected = true;
            }
            break;
        case NET_EVENT_WIFI_DISCONNECT_RESULT:
            LOG_INF("WiFi disconnected");
            connected = false;
        default: 
            break;
    }
}

#if !defined(CONFIG_WIFI_SSID)
#error CONFIG_WIFI_SSD not defined! Run `make menuconfig` and set a WIFI_SSID.
#endif 

#if !defined(CONFIG_WIFI_SECURITY)
#error CONFIG_WIFI_SECURITY not defined! Run `make menuconfig` and set CONFIG_WIFI_SECURITY.
#endif

BUILD_ASSERT(sizeof(CONFIG_WIFI_SSID) > 1, 
                "CONFIG_WIFI_SSID is empty! Run `make menuconfig` and set a WIFI_SSID");
                

int conn_mgr_init() {
    if (initd) {
        LOG_ERR("Connection manager already initialized");
        return -ENOTSUP;
    }
    
    LOG_INF("Performing first time setup of wifi iface");

    net_mgmt_init_event_callback(&net_mgmt_event_cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
    net_mgmt_add_event_callback(&net_mgmt_event_cb);

    sta_iface = net_if_get_wifi_sta();

    if (sta_iface == NULL) {
        LOG_ERR("WiFi station interface not found.");
        return -ENETDOWN;
    }

    struct net_linkaddr *mac_addr = net_if_get_link_addr(sta_iface);
    initd = true;
    LOG_INF("First time WiFi iface setup complete. MAC: " MACSTR, 
                mac_addr->addr[0], mac_addr->addr[1], mac_addr->addr[2],
                mac_addr->addr[3], mac_addr->addr[4], mac_addr->addr[5]);

    k_sem_init(&ping_sem, 1, 1);
    k_work_init_delayable(&conn_watchdog_work, conn_watchdog_handler);
    watchdog_disconnect_count = 0;

    return 0;
}

static void conn_watchdog_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!watchdog_enabled) {
        return;
    }

    if (connected) {
        if (watchdog_disconnect_count > 0) {
            LOG_INF("Connection watchdog recovered link after %u missed checks",
                    watchdog_disconnect_count);
        }
        watchdog_disconnect_count = 0;
        k_work_reschedule(&conn_watchdog_work, CONN_WATCHDOG_PERIOD);
        return;
    }

    watchdog_disconnect_count++;
    LOG_WRN("Connection watchdog detected disconnect (%u/%u)",
            watchdog_disconnect_count, CONN_WATCHDOG_MAX_MISSES);

    int ret = conn_mgr_connect();
    if (ret < 0) {
        LOG_WRN("Watchdog reconnect request failed (%d)", ret);
    }

    if (watchdog_disconnect_count >= CONN_WATCHDOG_MAX_MISSES) {
        LOG_ERR("Connection watchdog exceeded recovery limit, rebooting");
        sys_reboot(SYS_REBOOT_WARM);
    }

    k_work_reschedule(&conn_watchdog_work, CONN_WATCHDOG_PERIOD);
}

int cmd_conn_mgr_enable_watchdog(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    conn_mgr_enable_watchdog();
    return 0;
}

void conn_mgr_enable_watchdog() {
    if (watchdog_enabled) 
        return;

    if (!initd) {
        int ret = conn_mgr_init();
        if (ret < 0) {
            LOG_ERR("Cannot enable connection watchdog before init (%d)", ret);
            return;
        }
    }

    watchdog_enabled = true;
    watchdog_disconnect_count = 0;
    k_work_reschedule(&conn_watchdog_work, CONN_WATCHDOG_PERIOD);
    LOG_INF("Connection watchdog enabled");
}

int cmd_conn_mgr_connect(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    return conn_mgr_connect();
}

int conn_mgr_connect() {
    if (!initd) 
        conn_mgr_init();

    sta_wifi_config.ssid = (const uint8_t*)CONFIG_WIFI_SSID;
    sta_wifi_config.ssid_length = sizeof(CONFIG_WIFI_SSID) - 1;
    
#if defined(CONFIG_WIFI_PSK)
    if (sizeof(CONFIG_WIFI_PSK) > 1) {
        sta_wifi_config.psk = (const uint8_t*)CONFIG_WIFI_PSK;
        sta_wifi_config.psk_length = sizeof(CONFIG_WIFI_PSK);
    }
#endif  

    sta_wifi_config.security = CONFIG_WIFI_SECURITY;
    sta_wifi_config.channel = WIFI_CHANNEL_ANY;
    sta_wifi_config.band = WIFI_FREQ_BAND_2_4_GHZ;

    LOG_INF("Attempting to connect to SSID %s", CONFIG_WIFI_SSID);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_wifi_config, sizeof(struct wifi_connect_req_params));

    if (ret < 0) {
        LOG_ERR("Connection to SSID %s failed with code %d.", CONFIG_WIFI_SSID, ret);
    }

    return ret;
}

static int cmd_conn_mgr_disconnect(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    return conn_mgr_disconnect();
}

int conn_mgr_disconnect() {
    if (!connected) {
        LOG_WRN("Not connected, cannot disconnect.");
        return -ENOTCONN;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);

    if (ret < 0) {
        LOG_ERR("Disconnection failed with code %d.", ret);
    }

    return ret;
}

static bool cmd_is_connected(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(sh, "Connected: %s", conn_mgr_is_connected() ? "true" : "false");
    return 0;
}

bool conn_mgr_is_connected() {
    return connected;
}

static void dns_result_cb(enum dns_resolve_status status, struct dns_addrinfo *info, void* user_data) {
    char hr_addr[NET_IPV4_ADDR_LEN];
    char* hr_family;
    void* addr;

    switch (status) {
        case DNS_EAI_CANCELED:
            LOG_INF("DNS Query Cancelled");
            return;
        case DNS_EAI_FAIL:
            LOG_ERR("DNS Resolve Failed");
            return;
        case DNS_EAI_NODATA:
            LOG_ERR("DNS Cannot Resolve Address");
            return;
        case DNS_EAI_ALLDONE:
            LOG_INF("DNS Resolution Done");
            return;
        case DNS_EAI_INPROGRESS:
            LOG_INF("DNS Resolution in progress...");
            break;
        default:
            LOG_ERR("DNS Resolution error (%d)", status);
            return;
    }

    if (!info)
        return;

    if (info->ai_family == NET_AF_INET) { // ipv4
        hr_family = "IPv4";
        addr = &net_sin(&info->ai_addr)->sin_addr;
    } else if (info->ai_family == NET_AF_INET6) { // ipv6
        hr_family = "IPv6";
        addr = &net_sin6(&info->ai_addr)->sin6_addr;
    } else {
        LOG_ERR("Invalid IP AF (%d)", info->ai_family);
        return;
    }
    net_addr_ntop(info->ai_family, addr, hr_addr, sizeof(hr_addr));
    LOG_INF("DNS Resolver found address %s", hr_addr);

    if (user_data != NULL) 
        ((conn_mgr_dns_query_callback_t)user_data)(hr_addr);
}

static int cmd_conn_mgr_dns_query(const struct shell* sh, size_t argc, char** argv) {
    if (argc < 2) {
        shell_print(sh, "Usage: conn_mgr dns_query <hostname>");
        return -EINVAL;
    }
    return conn_mgr_dns_query(argv[1], NULL);
}

int conn_mgr_dns_query(const char* hostname, conn_mgr_dns_query_callback_t cb) {
    // ipv4 only for now?

    uint16_t dns_id;
    int ret = dns_get_addr_info(hostname, 
                                DNS_QUERY_TYPE_A,
                                &dns_id,
                                dns_result_cb,
                                (void*)cb,
                                DNS_TIMEOUT_MS);
    
    if (ret < 0) {
        LOG_ERR("Cannot resolve IPv4 address (%d)", ret);
        return ret;
    }

    return 0;
}

static void ping_cleanup() {
    // cleanup ctx and release semaphore to re-enable pings
    net_icmp_cleanup_ctx(&icmp_ctx);
    k_sem_give(&ping_sem);
}

static int net_icmp_reply_handler_cb(struct net_icmp_ctx *ctx,
                                     struct net_pkt *pkt,
                                     struct net_icmp_ip_hdr *ip_hdr,
                                     struct net_icmp_hdr *icmp_hdr,
                                     void *user_data) {
    if (icmp_hdr->type == NET_ICMPV4_ECHO_REPLY) {
        LOG_INF("ICMP echo reply received");
        if (user_data != NULL)
            ((conn_mgr_ping_callback_t)user_data)(0);
    } else {
        LOG_WRN("ICMP timeout type %d", icmp_hdr->type);
        ((conn_mgr_ping_callback_t)user_data)(-ETIMEDOUT);
    }
    
    ping_cleanup();
    return 0;
}

int conn_mgr_ping_host(const char* ip, conn_mgr_ping_callback_t cb) {
    if (!initd || !connected) {
        LOG_ERR("Cannot ping %s if not connected to a network or initialized!", ip);
        return -ENOTSUP;
    }

    // take sem so only 1 ping is allowed at a time
    int ret = k_sem_take(&ping_sem, K_NO_WAIT);
    if (ret == -EBUSY) {
        LOG_ERR("conn_mgr_ping_host called when ping was in progress.");
        return ret;
    }
    
    LOG_INF("Pinging host %s", ip);
    
    // init ping tracker struct. Use NET_ICMPV4_ECHO_REPLY because thats the message we are expecting back.
    ret = net_icmp_init_ctx(&icmp_ctx, NET_AF_INET, NET_ICMPV4_ECHO_REPLY, 0, net_icmp_reply_handler_cb);
    if (ret < 0) {
        LOG_ERR("Failed to init ICMP context: %d", ret);
        ping_cleanup();
        return ret;
    }

    struct net_sockaddr_in ipv4_destination = {
        .sin_family = NET_AF_INET
    };

    // turn IP address string into an ip sockaddr struct for the ping function
    ret = net_addr_pton(NET_AF_INET, ip, &ipv4_destination.sin_addr);
    if (ret < 0) {
        LOG_ERR("IP Address Invalid (%d)", ret);
        ping_cleanup();
        return ret;
    }

    // send ping, and supply our callback as an extra param
    ret = net_icmp_send_echo_request(&icmp_ctx, sta_iface, (struct net_sockaddr*)&ipv4_destination, NULL, (void*)cb);
    if (ret < 0) {
        LOG_ERR("Sending ICMP Echo request to %s failed (%d)", ip, ret);
        ping_cleanup();
        return ret;
    }

    LOG_INF("ICMP Echo request sent to %s", ip);

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_conn_mgr,
    SHELL_CMD(connect, NULL, "Initialize if needed and connect to configured WiFi Network", cmd_conn_mgr_connect),
    SHELL_CMD(disconnect, NULL, "Disconnect from the WiFi Network", cmd_conn_mgr_disconnect),
    SHELL_CMD(is_connected, NULL, "Check if currently connected to a WiFi network", cmd_is_connected),
    SHELL_CMD(dns_query, NULL, "Perform a DNS query for the given hostname", cmd_conn_mgr_dns_query),
    SHELL_CMD(en_watchdog, NULL, "Enable the WiFi connection watchdog", cmd_conn_mgr_enable_watchdog),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(conn_mgr, &sub_conn_mgr, "Connection Manager commands", NULL);