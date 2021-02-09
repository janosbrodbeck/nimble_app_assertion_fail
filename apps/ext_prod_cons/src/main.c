#include "sysinit/sysinit.h"
#include "os/os.h"
#include "console/console.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "log/log.h"
#include "shell/shell.h"

#define PRODUCER_STACK_SIZE 256
#define EXTSCANNER_MODULE "scanner"
#define BLE_ADDR_LEN 6

#define ADV_LEN 1100
#define MARKER_1 0xCA
#define MARKER_2 0xFE
#define MARKER_3 0xAF
#define MARKER_4 0xFE

typedef struct {
    bool ongoing;
    uint8_t data[ADV_LEN];
    size_t len;
} _chained_packet;

static uint8_t nimble_own_addr_type;
static _chained_packet _chain;
static bool _consumer;

static uint8_t _producer_buf[ADV_LEN];
struct os_task producer_task;
os_stack_t producer_task_stack[PRODUCER_STACK_SIZE];
uint32_t producer_interval;

static int _configure_adv_instance(uint8_t instance);
static int _advertise(uint8_t *data, size_t len);
static int _scanner_start();

static void print_ble_addr(uint8_t *addr, bool newline)
{
    for (uint8_t i=0; i < BLE_ADDR_LEN; i++) {
        printf("%02x", addr[BLE_ADDR_LEN-(i+1)]);
        if (i < BLE_ADDR_LEN-1) {
            printf(":");
        }
    }
    if (newline) {
        printf("\n");
    }
}

static int _advertise(uint8_t *data, size_t len)
{
    /* check if advertising instance 0 is free */
    if(ble_gap_adv_active())
    {
        return -1;
    }

    /* allocate mbuf */
    struct os_mbuf *buf = os_msys_get_pkthdr(len, 0);
    if (buf == NULL) {
        return -1;
    }

    int rc = os_mbuf_append(buf, data, len);
    if (rc) {
        printf("os_mbuf_append failed: %d\n", rc);
        os_mbuf_free_chain(buf);
        goto error;
    }

    rc = ble_gap_ext_adv_set_data(0, buf);
    if (rc) {
        printf("ble_gap_ext_adv_set_data failed: %d\n", rc);
        goto error;
    }

    int max_events = 3;
    rc = ble_gap_ext_adv_start(0, 0, max_events);
    if (rc) {
        printf("ble_gap_ext_adv_start failed: %d\n", rc);
        goto error;
    }
    printf("Advertised %d bytes\n", len);

error:
    return rc;
}

static bool _filter_packet(uint8_t *data, uint8_t len) {
    if (len > 4) {
        if (data[0] == MARKER_1 && data[1] == MARKER_2 &&
            data[2] == MARKER_3 && data[3] == MARKER_4) {
            return true;
        }
    }
    return false;
}

void _producer(void *arg)
{
    uint16_t ct = 1;
    os_time_t sleep_ticks;
    os_time_ms_to_ticks(producer_interval, &sleep_ticks);

    printf("Producer started...\n");
    while (1)
    {
        /* rewrite buffer to change pattern */
        memset(_producer_buf, ct, sizeof(_producer_buf));
        /* write marker */
        memset(_producer_buf, MARKER_1, 1);
        memset(_producer_buf+1, MARKER_2, 1);
        memset(_producer_buf+2, MARKER_3, 1);
        memset(_producer_buf+3, MARKER_4, 1);

        _advertise(_producer_buf, sizeof(_producer_buf));

        ct++;
        os_time_delay (sleep_ticks);
    }
}

static void _on_data(struct ble_gap_event *event, void *arg)
{
    /* TRUNCATED status -> recv of advertising failed */
    if (event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_TRUNCATED) {
        _chain.ongoing = false;
        return;
    }

    if (!_chain.ongoing) { /* first of multiple events or single complete event  */

        bool marked_packet = _filter_packet((uint8_t *)event->ext_disc.data,
                            event->ext_disc.length_data);

        /* do not process any further if packet without marker and no ongoing chain */
        if (!marked_packet && !_chain.ongoing) {
            return;
        }
        /* incomplete event, prepare for more data in the next events  */
        if (event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE) {
            _chain.ongoing = true;
        }

        /* copy data into buffer */
        memcpy(_chain.data, event->ext_disc.data, event->ext_disc.length_data);
        _chain.len = event->ext_disc.length_data;

    } else { /* subsequent events */

            /* sanity check */
            if (_chain.len+event->ext_disc.length_data > sizeof(_chain.data)) {
                printf("Broken events from nimBLE\n");
                _chain.ongoing = false;
                return;
            }
            /* copy data into buffer */
            memcpy(_chain.data+_chain.len, event->ext_disc.data,
                event->ext_disc.length_data);
            _chain.len += event->ext_disc.length_data;

            /* last event of advertising received */
            if (event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) {
                _chain.ongoing = false;
            }
    }

    if (_chain.ongoing) {
        return;
    }

    /* got full packet, now advertise back if consumer */
    if (_consumer) {
        _advertise(_chain.data, _chain.len);
    }
}


static int _gap_event(struct ble_gap_event *event, void *arg)
{
    (void) arg;

    switch(event->type) {
        case BLE_GAP_EVENT_ADV_COMPLETE:
            return 0;
        case BLE_GAP_EVENT_EXT_DISC:
            _on_data(event, arg);
            return 0;
    }
    return 0;
}

int cmd_consumer(int argc, char **argv)
{
    _consumer = true;
    int rc = _scanner_start();
    if (rc == 0) {
        printf("Consumer started...\n");
    } else {
        printf("Consumer start failed: %d\n", rc);
    }
    return 0;
}

int cmd_producer(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("producer <interval[ms]>\n");
        return -1;
    }
    producer_interval = atoi(argv[1]);
    _consumer = false;

    int rc = _scanner_start();
    if (rc != 0) {
        printf("Producer: scanner start failed: %d\n", rc);
        return rc;
    }

    rc = os_task_init(&producer_task, "prod", _producer, NULL, 50,
                OS_WAIT_FOREVER, producer_task_stack, PRODUCER_STACK_SIZE);
    if (rc != 0) {
        printf("Failed to start producer task: %d\n", rc);
    }
    return 0;
}

static const struct shell_cmd scanner_commands[] = {
    {
        .sc_cmd = "consumer",
        .sc_cmd_func = cmd_consumer,
    },
    {
        .sc_cmd = "producer",
        .sc_cmd_func = cmd_producer,
    },
    { 0 },
};

void cmd_init(void)
{
    shell_register(EXTSCANNER_MODULE, scanner_commands);
    shell_register_default_module(EXTSCANNER_MODULE);
}

static int _scanner_start()
{
    struct ble_gap_ext_disc_params uncoded = {0};
    uint8_t limited = 0;
    uint8_t filter_duplicates = 1;
    uint8_t duration = 0;
    uint8_t period = 0;

    uncoded.passive = 1;
    uncoded.itvl = BLE_GAP_SCAN_FAST_INTERVAL_MIN;
    uncoded.window = BLE_GAP_SCAN_FAST_WINDOW;

    int rc = ble_gap_ext_disc(nimble_own_addr_type, duration, period, filter_duplicates,
                            BLE_HCI_SCAN_FILT_NO_WL, limited, &uncoded, NULL, _gap_event, NULL);
    if (rc != 0) {
        printf("Scanner start failed: %d\n", rc);
    }
    return rc;
}

static int _configure_adv_instance(uint8_t instance)
{
    struct ble_gap_ext_adv_params params = { 0 };
    int8_t selected_tx_power;

    memset(&params, 0, sizeof(params));
    /* set advertise parameters */
    params.scannable = 0;
    params.directed = 0;
    params.own_addr_type = nimble_own_addr_type;
    params.channel_map = BLE_GAP_ADV_DFLT_CHANNEL_MAP;
    params.filter_policy = 0;
    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    params.tx_power = 127;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;

    int rc = ble_gap_ext_adv_configure(instance, &params, &selected_tx_power, _gap_event, NULL);
    if (rc) {
        printf("_configure_adv_instance: failed to configure advertising instance %d. "
            "Return code: 0x%02X\n", instance, rc);
        return -1;
    }

    ble_addr_t addr;
    rc = ble_hs_id_copy_addr(nimble_own_addr_type, addr.val, NULL);
    addr.type = nimble_own_addr_type;
    if (rc) {
        printf("_configure_adv_instance: failed to retrieve BLE address\n");
        return -1;
    }
    rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc) {
        printf("_configure_adv_instance: could not set address\n");
    }
    printf("Advertising instance %d configured\n", instance);
    return rc;
}

static void set_ble_addr(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &nimble_own_addr_type);
    assert(rc == 0);

    switch (nimble_own_addr_type) {
        case 0:
            printf("Own address type: ADDR_PUBLIC\n");
            break;
        case 1:
            printf("Own address type: ADDR_RANDOM\n");
            break;
        default:
            printf("Own address type: %d\n", nimble_own_addr_type);
            break;
    }
}

static void on_sync(void)
{
    set_ble_addr();

    /* print own address */
    uint8_t tmp_addr[BLE_ADDR_LEN];
    ble_hs_id_copy_addr(nimble_own_addr_type, tmp_addr, NULL);
    printf("Own address: ");
    print_ble_addr(tmp_addr, true);

    /* configure advertising instances */
    for (uint8_t i = 0; i < MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES+1; i++) {
        _configure_adv_instance(i);
    }
}

static void on_reset(int reason)
{
    MODLOG_DFLT(INFO, "Resetting state; reason=%d\n", reason);
}

int main(int argc, char **argv)
{
    sysinit();
    cmd_init();

    printf("\nAPP: mynewt extender scanner\n");
    printf("MSYS_1_BLOCK_SIZE: %d\n", MYNEWT_VAL_MSYS_1_BLOCK_SIZE);
    printf("MSYS_1_BLOCK_COUNT: %d\n", MYNEWT_VAL_MSYS_1_BLOCK_COUNT);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    while (1) {
        os_eventq_run(os_eventq_dflt_get());
    }
    assert(0);

    return 0;
}

