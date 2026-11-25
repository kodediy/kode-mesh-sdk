/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_ao.c
 * @brief What the owner does with the signals its Active Object hands it, and
 *        with the system around it: J3's supply and the controllers kodeOS
 *        lends, the radio's interrupt, POWER_SLEEP_SIG and HOME_SIG.
 *
 * The Active Object is faked at mesh_ao.h (fakes/fake_mesh_ao.c), so a case
 * delivers a signal by queueing it and waits for the owner to have taken it.
 * mesh_ao.c itself, on QP/C, is tests/host_ao.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

/** @brief Wait for the owner to have taken one more queued item than @p before. */
static bool served_after(unsigned before)
{
    return WAIT_FOR(g_ao.served > before, 1000);
}

/** @brief Take the module away and give it back, so the owner opens it afresh. */
static void module_leaves_and_returns(void)
{
    h_module_away();
    h_module_back();
}

/* ---- Start, the supply and the controllers ------------------------------------- */

TEST_CASE("start: J3's supply is switched on once, and starting again does nothing more", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL_UINT(1U, g_exp.power_on_calls);   /* by mesh_aos_start(), before the first look */
    TEST_ASSERT_EQUAL_UINT(0U, g_exp.power_off_calls);  /* kodeOS switches it off when the app ends */

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL_UINT(1U, g_exp.power_on_calls);
}

TEST_CASE("open: the module's lines and the controllers kodeOS lends reach the drivers", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_TAG));

    /* Positions lend as themselves (fakes/kode.h), so a pin names its position. */
    TEST_ASSERT_EQUAL_UINT8(g_exp.spi_id, g_lr_pins.spi_host);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_LORA_CS, g_lr_pins.chip_select);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_LORA_SCK, g_lr_pins.clock);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_LORA_MOSI, g_lr_pins.mosi);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_LORA_MISO, g_lr_pins.miso);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_LORA_BUSY, g_lr_pins.busy);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_LORA_RESET, g_lr_pins.reset);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_LORA_DIO9, g_lr_pins.dio9);

    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_TRUE(g_gnss.is_open);
    TEST_ASSERT_EQUAL_UINT8(g_exp.uart_id, g_gnss.pins.uart_port);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_GNSS_TXD, g_gnss.pins.rxd);   /* the P4 listens to its TXD */
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_GNSS_RXD, g_gnss.pins.txd);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_GNSS_RESET, g_gnss.pins.reset);
    TEST_ASSERT_EQUAL_UINT8(MESH_BSP_POSITION_PPS, g_gnss.pins.pps);
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
}

TEST_CASE("open: the controllers it is given are the ones it uses, whichever they are", "[owner][ao]")
{
    harness_fresh();
    g_exp.spi_id  = 3U;   /* kodeOS lends whichever is free */
    g_exp.uart_id = 2U;
    module_leaves_and_returns();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));

    TEST_ASSERT_EQUAL_UINT8(3U, g_lr_pins.spi_host);
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_EQUAL_UINT8(2U, g_gnss.pins.uart_port);
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
}

/* ---- A module that cannot be taken yet: somebody else's lines, or not a Mesh -------- */

/** @brief One way a line or a controller can be somebody else's. */
typedef struct {
    char const *what;     /**< For the failure message.                         */
    void      (*busy)(void);
    void      (*free)(void);
    char const *needle;   /**< What the one log line says is missing.            */
} busy_way_t;

static void core_lines_busy(void)  { g_exp.refuse_lend = MESH_BSP_LOAN_TAG; }
static void pair_busy(void)        { g_exp.refuse_lend = MESH_BSP_LOAN_PAIR; }
static void lends_free(void)       { g_exp.refuse_lend = NULL; }
static void spi_busy(void)         { g_exp.spi_refused = true; }
static void spi_free(void)         { g_exp.spi_refused = false; }
static void uart_busy(void)        { g_exp.uart_refused = true; }
static void uart_free(void)        { g_exp.uart_refused = false; }
static void driver_busy(void)      { g_lr.open_rc = KODE_ERROR; }
static void driver_free(void)      { g_lr.open_rc = KODE_OK; }

static const busy_way_t BUSY_WAYS[] = {
    { "core lines",  core_lines_busy, lends_free, "one of its radio and GNSS lines" },
    { "I2C pair",    pair_busy,       lends_free, "its I2C pair" },
    { "SPI",         spi_busy,        spi_free,   "every SPI or every UART controller" },
    { "UART",        uart_busy,       uart_free,  "every SPI or every UART controller" },
    { "driver open", driver_busy,     driver_free, "the SPI bus or the GPIO interrupt" },
};

/** @brief The module is back, but @p way keeps the owner from taking it, for several looks; then it is free. */
static void busy_for_a_while_then_free(const busy_way_t *way, bool and_again)
{
    harness_fresh();
    h_module_away();
    uint8_t const seen = h_status().seen;

    way->busy();
    unsigned const attempts = g_exp.lends_of_module;
    h_module_back();

    /* It tries again at every look - not once and never again. */
    TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR((g_exp.lends_of_module - attempts) >= 4U, 8000U), way->what);
    TEST_ASSERT_FALSE_MESSAGE(h_status().present, way->what);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h_events(MESH_AOS_EVENT_WRONG_MODULE), way->what);   /* it is not a wrong module */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h_events(MESH_AOS_EVENT_PRESENT), way->what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(seen, h_status().seen, way->what);                      /* and nothing is concluded */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_log_matches(way->needle), way->what);           /* said once, with what is missing */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_log_matches("a Mesh's fingerprint is on J3 but"), way->what);
    TEST_ASSERT_FALSE_MESSAGE(fake_loaned(MESH_BSP_LOAN_TAG), way->what);                   /* nothing kept from the failed tries */
    TEST_ASSERT_FALSE_MESSAGE(fake_loaned(MESH_BSP_LOAN_PAIR), way->what);

    /* Free at last: the next look takes it. */
    way->free();
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_aos_wait_present(5000U), way->what);
    TEST_ASSERT_TRUE_MESSAGE(h_status().present, way->what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MESH_MODULE_MESH, h_status().seen, way->what);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h_events(MESH_AOS_EVENT_PRESENT), way->what);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h_events(MESH_AOS_EVENT_WRONG_MODULE), way->what);
    TEST_ASSERT_TRUE_MESSAGE(fake_loaned(MESH_BSP_LOAN_TAG), way->what);
    TEST_ASSERT_TRUE_MESSAGE(fake_loaned(MESH_BSP_LOAN_PAIR), way->what);

    if (and_again) {
        /* Taken once, so a later busy spell is said again: once more. */
        h_module_away();
        way->busy();
        unsigned const again = g_exp.lends_of_module;
        h_module_back();
        TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR((g_exp.lends_of_module - again) >= 3U, 8000U), way->what);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_log_matches("a Mesh's fingerprint is on J3 but"), way->what);
        way->free();
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_aos_wait_present(5000U), way->what);
    }
}

TEST_CASE("open: a refused lend of the radio and GNSS lines is somebody else's, not a wrong module", "[owner][ao][busy]")
{
    busy_for_a_while_then_free(&BUSY_WAYS[0], true);
}

TEST_CASE("open: a refused lend of the I2C pair gives the radio lines back, and is not a wrong module", "[owner][ao][busy]")
{
    busy_for_a_while_then_free(&BUSY_WAYS[1], false);
}

TEST_CASE("open: no free SPI controller - not a wrong module, tried at every look, taken when one is free", "[owner][ao][busy]")
{
    busy_for_a_while_then_free(&BUSY_WAYS[2], false);
}

TEST_CASE("open: no free UART controller - not a wrong module, tried at every look, taken when one is free", "[owner][ao][busy]")
{
    busy_for_a_while_then_free(&BUSY_WAYS[3], false);
}

TEST_CASE("open: the radio driver cannot be opened - not a wrong module, and told once", "[owner][ao][busy]")
{
    busy_for_a_while_then_free(&BUSY_WAYS[4], true);
}

TEST_CASE("open: a fingerprint whose radio does not answer is a wrong module - told once, not at every look", "[owner][ao][busy]")
{
    harness_fresh();
    h_module_away();

    g_lr.reset_fails = true;   /* the lines are ours and free, and nothing answers */
    unsigned const resets = g_lr.n_reset;
    h_module_back();

    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_WRONG_MODULE) == 1U, 5000U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)MESH_MODULE_OTHER, h_event_arg(MESH_AOS_EVENT_WRONG_MODULE));
    TEST_ASSERT_EQUAL_UINT8(MESH_MODULE_OTHER, h_status().seen);
    TEST_ASSERT_FALSE(h_status().present);

    /* It is looked at again and again - and says nothing more. */
    TEST_ASSERT_TRUE(WAIT_FOR((g_lr.n_reset - resets) >= 4U, 8000U));
    TEST_ASSERT_EQUAL_UINT(1U, h_events(MESH_AOS_EVENT_WRONG_MODULE));
    TEST_ASSERT_EQUAL_UINT(1U, fake_log_matches("its radio did not answer"));
    TEST_ASSERT_FALSE(fake_loaned(MESH_BSP_LOAN_TAG));      /* what was lent went back every time */
    TEST_ASSERT_FALSE(fake_loaned(MESH_BSP_LOAN_PAIR));
    TEST_ASSERT_EQUAL_UINT(0U, fake_log_matches("a Mesh's fingerprint is on J3 but"));   /* it is not a busy line */

    /* The radio answers after all: taken. */
    g_lr.reset_fails = false;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_EQUAL_UINT(1U, h_events(MESH_AOS_EVENT_PRESENT));
    TEST_ASSERT_EQUAL_UINT8(MESH_MODULE_MESH, h_status().seen);
}

/* ---- The radio's interrupt -------------------------------------------------------- */

TEST_CASE("interrupt: with the radio ready and DIO9 high the chip is read at once, not at the tick", "[owner][ao]")
{
    harness_fresh();
    g_ao.tick_on = false;   /* only the interrupt can wake the owner now */
    unsigned const ticks = g_ao.ticks;

    uint8_t const hello[5] = { 'H', 'E', 'L', 'L', 'O' };
    lr_fake_inject(hello, 5U, true);
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_dio9());

    unsigned const served = g_ao.served;
    fake_ao_interrupt();
    TEST_ASSERT_TRUE(served_after(served));

    TEST_ASSERT_EQUAL_UINT(ticks, g_ao.ticks);          /* no tick ran */
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_take);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_read);
    TEST_ASSERT_EQUAL_UINT32(1U, h_status().packets_received);
    TEST_ASSERT_EQUAL_UINT(1U, h_events(MESH_AOS_EVENT_PACKET));
    TEST_ASSERT_EQUAL_UINT(1U, g_ao.interrupts);
}

TEST_CASE("interrupt: with DIO9 low the line is trusted and the chip is not read", "[owner][ao]")
{
    harness_fresh();
    g_ao.tick_on = false;

    TEST_ASSERT_FALSE(mesh_drv_lr2021_dio9());   /* a tick has served it already */
    unsigned const served = g_ao.served;
    fake_ao_interrupt();
    TEST_ASSERT_TRUE(served_after(served));

    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_take);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_read);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_PACKET));
}

TEST_CASE("interrupt: before the radio is set up it is left for the tick", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_FALSE(h_status().radio_ready);
    g_ao.tick_on = false;

    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    unsigned const served = g_ao.served;
    fake_ao_interrupt();
    TEST_ASSERT_TRUE(served_after(served));

    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_take);                                  /* not read ... */
    TEST_ASSERT_BITS_HIGH(MESH_DRV_LR2021_INTERRUPT_SEND_DONE, g_lr.irq);     /* ... and not lost */
}

/* ---- The board's rest (AO_LowPower) and HOME_SIG -------------------------------------------------- */

/**
 * @brief Carrier up, the system speaks; the radio is off air, asleep with its reset line high
 *        - or, with @p standby_fails, held in reset - and begun afresh later.
 */
static void off_air_and_held(void (*publish)(void), char const *log_needle, bool standby_fails)
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 20000));
    TEST_ASSERT_TRUE(h_status().testing);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    unsigned const holds = g_lr.n_hold;
    unsigned const sleeps = g_lr.n_sleep;

    unsigned const served = g_ao.served;
    if (standby_fails) {
        g_lr.standby_fails = 1000U;   /* the stop and the sleep both find a chip that will not confirm it */
        g_lr.reset_fails   = true;
    }
    publish();
    TEST_ASSERT_TRUE(served_after(served));

    if (standby_fails) {
        TEST_ASSERT_TRUE(g_lr.held);                    /* in reset: a chip that missed the standby cannot stay keyed */
        TEST_ASSERT_GREATER_THAN_UINT(holds, g_lr.n_hold);
        TEST_ASSERT_FALSE(g_lr.asleep);
        TEST_ASSERT_FALSE(h_status().radio_asleep);
        g_lr.standby_fails = 0U;
        g_lr.reset_fails   = false;
    } else {
        TEST_ASSERT_TRUE(g_lr.asleep);                  /* confirmed in standby, then asleep */
        TEST_ASSERT_FALSE(g_lr.held);                   /* its reset line high: nothing through R502 */
        TEST_ASSERT_EQUAL_UINT(holds, g_lr.n_hold);
        TEST_ASSERT_GREATER_THAN_UINT(sleeps, g_lr.n_sleep);
        TEST_ASSERT_TRUE(h_status().radio_asleep);
        TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_asleep_calls);
    }
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_FALSE(h_status().testing);
    TEST_ASSERT_FALSE(h_status().sending);
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_FALSE(h_status().radio_ready);          /* no setting any more */
    TEST_ASSERT_TRUE(h_status().present);               /* the module is still there: only the radio is down */
    TEST_ASSERT_TRUE(fake_log_seen(log_needle));

    /* Nothing goes on air until the app begins the radio again, which lets the
     * line go before it writes anything. */
    uint8_t d[4] = { 1, 2, 3, 4 };
    unsigned const sent = g_lr.n_send;
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(d, 4U, 2000U));
    TEST_ASSERT_EQUAL_UINT(sent, g_lr.n_send);
    unsigned const resets = g_lr.n_reset;
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_GREATER_THAN_UINT(resets, g_lr.n_reset);
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_FALSE(g_lr.asleep);
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_FALSE(h_status().radio_asleep);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_asleep_calls);
}

TEST_CASE("rest: a round for ASLEEP takes a carrier off air and puts the radio to sleep, reset line high", "[owner][ao]")
{
    off_air_and_held(fake_ao_publish_rest, "going to rest", false);
}

TEST_CASE("home: HOME_SIG takes a carrier off air and puts the radio to sleep, reset line high", "[owner][ao]")
{
    off_air_and_held(fake_ao_publish_home, "bottom button was held", false);
}

TEST_CASE("rest: a chip that will not confirm standby is held in reset instead, as before", "[owner][ao]")
{
    off_air_and_held(fake_ao_publish_rest, "going to rest", true);
}

TEST_CASE("home: a chip that will not confirm standby is held in reset instead, as before", "[owner][ao]")
{
    off_air_and_held(fake_ao_publish_home, "bottom button was held", true);
}

TEST_CASE("rest: a packet on air is taken off it, its call is answered cut short, and the receiver comes back only on AWAKE", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    uint8_t d[6] = { 6, 5, 4, 3, 2, 1 };
    TEST_ASSERT_EQUAL(KODE_OK, h_send(d, 6U, 2000U));
    TEST_ASSERT_TRUE(h_status().sending);
    unsigned const receives = g_lr.n_receive;

    unsigned const served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(served_after(served));

    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_FALSE(h_status().sending);
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send_answer(1000U, NULL));   /* the packet never went: the call is told so */
    TEST_ASSERT_EQUAL_UINT(0U, h_status().packets_sent);
    TEST_ASSERT_EQUAL_UINT(receives, g_lr.n_receive);   /* nothing turned it on again by itself */
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_SETTING_LOST, MESH_SETTING_RADIO | MESH_SETTING_RECEIVER));
}

TEST_CASE("rest: with the radio ended it sleeps already, stays asleep, and is begun afresh later", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_TRUE(g_lr.asleep);

    unsigned const served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(served_after(served));
    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_asleep_calls);
    TEST_ASSERT_TRUE(h_status().present);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_LOST));   /* there was nothing to lose */

    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 450));
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

/** @brief The system says AWAKE and waits until the owner has taken it. */
static void awake_and_wait(void)
{
    unsigned const served = g_ao.served;
    fake_ao_publish_awake();
    TEST_ASSERT_TRUE(served_after(served));
}

TEST_CASE("rest: AWAKE puts back the setting and the receiver, and sends nothing", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const sends = g_lr.n_send;
    unsigned const tests = g_lr.n_test;
    unsigned const served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(served_after(served));
    TEST_ASSERT_FALSE(h_status().radio_ready);
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_SETTING_LOST, MESH_SETTING_RADIO | MESH_SETTING_RECEIVER));
    h_events_clear();

    awake_and_wait();
    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_TRUE(st.receiving);
    TEST_ASSERT_EQUAL_UINT(9U, st.spreading_factor);
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_SETTING_RESTORED, MESH_SETTING_RADIO | MESH_SETTING_RECEIVER));
    TEST_ASSERT_EQUAL_UINT(sends, g_lr.n_send);   /* never a send ... */
    TEST_ASSERT_EQUAL_UINT(tests, g_lr.n_test);   /* ... and never a test */
    TEST_ASSERT_FALSE(st.sending);
    TEST_ASSERT_FALSE(st.testing);
}

TEST_CASE("rest: AWAKE twice does no harm, and AWAKE with no rest does nothing", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const resets = g_lr.n_reset;
    awake_and_wait();                                  /* the board never rested */
    TEST_ASSERT_EQUAL_UINT(resets, g_lr.n_reset);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_RESTORED));

    unsigned const served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(served_after(served));
    awake_and_wait();
    unsigned const after_first = g_lr.n_reset;
    unsigned const restored = h_events(MESH_AOS_EVENT_SETTING_RESTORED);
    TEST_ASSERT_EQUAL_UINT(1U, restored);
    awake_and_wait();                                  /* AO_LowPower repeats AWAKE after an abandoned round */
    TEST_ASSERT_EQUAL_UINT(after_first, g_lr.n_reset);
    TEST_ASSERT_EQUAL_UINT(restored, h_events(MESH_AOS_EVENT_SETTING_RESTORED));
    TEST_ASSERT_TRUE(h_status().receiving);
}

TEST_CASE("rest: what the app ended before the wake is not put back", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(served_after(served));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));   /* the app lets go while the board rests */
    h_events_clear();
    awake_and_wait();
    TEST_ASSERT_FALSE(h_status().radio_ready);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_RESTORED));
}

TEST_CASE("rest: a second round (OFF after ASLEEP) keeps what the first took", "[owner][ao]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(served_after(served));
    served = g_ao.served;
    fake_ao_publish_rest();                            /* an OFF round from ASLEEP: nothing is held any more */
    TEST_ASSERT_TRUE(served_after(served));
    h_events_clear();
    awake_and_wait();
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_TRUE(h_status().receiving);
}
