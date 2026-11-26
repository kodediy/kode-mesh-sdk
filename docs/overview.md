# The Mesh SDK: overview

What the SDK drives, how it works, what surprises, the options, the console, what it costs an app and the layout of the tree. The guide for someone who writes an app is at [docs.kode.diy/modules/mesh](https://docs.kode.diy/modules/mesh); the two headers, `mesh.h` and `mesh_sys.h`, are the contract.

The Mesh module (KDMSH001) plugs into the expansion header J3 of the Kode Dot. It carries a Semtech LR2021 LoRa transceiver with two amplifiers and two SMA connectors - sub-GHz on J501, 2.4 GHz on J502 - and a Minew ME31GR02 GNSS receiver, whose second face is the header's I2C pair. The SDK covers what the module has, and only that.

## Two layers, as kode-sdk has

The same module through two headers, one for tasks and one for Active Objects, over one owner. Pick by one question: **does the caller wait, or react?**

| | `mesh.h`, the standard layer | `mesh_sys.h`, the extended layer |
|---|---|---|
| Caller | a task: `app_main()`, a task of your own | an Active Object |
| A call | **waits** for the owner's answer | **never waits** |
| Result | `bool`; `mesh_error()` says why | `kode_status_t`; a request's answer is a signal |
| What happens by itself | events **by name**: `mesh_event_add_listener()`, `mesh_event_wait()` | signals you subscribed to (`mesh_sys_subscribe()`) |
| Prefix | `mesh_` | `mesh_sys_` |

A `mesh.h` call made from an Active Object is refused, whatever it is, and `mesh_error()` says `mesh_radio_send was called from an Active Object (<ao>); mesh.h calls block - call mesh_sys_* instead`. Both layers go through the same owner, so a task and an Active Object can use the module at once.

From a task. This one TRANSMITS: an antenna or a 50 ohm load on J501 first.

```c
#include "kode.h"
#include "kode_log.h"
#include "mesh.h"

void app_main(void)
{
    if (!mesh_begin()) {                      // kode_init() is done for you; J3's supply on; the owner starts looking
        KLOG_E("app", "mesh: %s", mesh_error());
    } else if (mesh_wait_present(3000)) {     // true at once if it is there already
        // Only after the person said the antenna is on, and asked for a send:
        mesh_radio_antennas_confirm(true);   // until then every send is held to 0 dBm
        mesh_radio_begin(MESH_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_DBM_X10);  // 868 MHz, SF9, the region's cap (85 = 8.5 dBm)
        uint8_t hello[] = "hi";
        mesh_radio_send(hello, sizeof(hello) - 1);  // returns when the packet is gone; inside the duty cycle
    }
    kode_exit();                              // back to kodeOS
}
```

From an Active Object, a call asks and the answer is a signal. [`examples/send`](../examples/send) is that shape, whole - this is its heart:

```c
// in the initial transition (kode_init() has run):
(void)mesh_sys_begin();                       // returns at once; does not wait for a module
(void)mesh_sys_subscribe(&me->super);         // MESH_MODULE_FOUND_SIG and the other news

// in the state, when MESH_MODULE_FOUND_SIG arrives:
(void)mesh_sys_radio_begin(&me->super, MESH_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_DBM_X10);

// and its answer, later:
case MESH_RADIO_BEGIN_RSP_SIG: {
    mesh_sys_answer_t const *a = Q_EVT_CAST(mesh_sys_answer_t);
    if (KODE_OK == a->rc) { /* ... ask mesh_sys_radio_send(&me->super, data, n) */ }
    break;                                    // else a->why, in words: mesh_sys_why_text(a->why)
}
```

Four kinds of call in `mesh_sys.h`: **commands** (posted, no answer), **requests** (they take the asking Active Object first, `QActive *me`, and are answered by a signal `MESH_<VERB>_RSP_SIG`), **immediates** (a copy or a number, at once) and **news** (signals the SDK posts to every Active Object that subscribed). The header says the queue rule for answers.

## What it drives

The names are `mesh_<part>_<verb>()` in `mesh.h` and `mesh_sys_<part>_<verb>()` in `mesh_sys.h`; the table says them once, in the standard layer's.

| Part | Calls | Notes |
|---|---|---|
| The radio | `mesh_radio_begin()`, `mesh_radio_end()`, `mesh_radio_standby()` | A band (sub-GHz 868 or 2.4 GHz), a spreading factor (7..12; outside it the call is refused, as `mesh_radio_configure()` refuses it) and a power. The band's centre, bandwidth and amplifier row are the region's; the power is held to the region's cap, and `mesh_status().power_applied_dbm_x10` says what the setting really puts out. `mesh_radio_standby()` is fine with no band set. Begin configures and calibrates the chip. With no setting on it - found and not begun, or ended - the chip sleeps (0.47 uA against Standby RC's 1.21 mA, LR20xx datasheet table 3-4) and begin wakes it with its reset pulse; `status.radio_asleep` says so. |
| A whole setting | `mesh_radio_config_default()`, `mesh_radio_configure()` | Frequency, bandwidth, spreading factor, coding rate, preamble, sync word and power, for an app that speaks a network of its own. The default build takes only the region's setting; `CONFIG_MESH_LAB` takes what the chip has. |
| Moving between settings | `mesh_radio_calibrate()`, `mesh_radio_retune()` | For an app on several networks by turns: up to three frequencies calibrated once and kept, then another whole setting with no calibration, bands included. |
| Sending | `mesh_radio_send()`, `mesh_radio_send_preamble()`, `mesh_radio_send_wait_ms()` | One packet, 1..255 bytes. **Until `mesh_radio_antennas_confirm(true)` it goes out at `MESH_REGION_SAFE_DBM_X10` (0 dBm)**, whatever power was asked: a radio that seems weak is that (`mesh_status().power_capped`). `mesh_radio_send()` returns when the packet is gone; `mesh_sys_radio_send()` answers with the airtime. A packet on its way in does not hold a send back: ask `mesh_radio_channel_busy()` first. On the 868 band the SDK counts the duty cycle: when a send has no budget, it is refused (an Active Object's answer is `KODE_NO_RESOURCES` with `MESH_WHY_DUTY` and the wait in the answer) and `mesh_radio_send_wait_ms()` says how long until there is room. 2.4 GHz has no duty rule. |
| Receiving | `mesh_radio_receive()`, `mesh_radio_read()`, `mesh_radio_listen()` | The receiver on continuously, or for one packet within a set time. A packet raises the event `packet_received` (`MESH_PACKET_RECEIVED_SIG`) and `mesh_radio_read()` pops it, with its length, CRC verdict, RSSI and SNR, from a ring of four. A packet whose CRC failed is delivered with `crc_valid` false, for the app to judge. |
| Measuring | `mesh_radio_rssi()` | The channel's power, as the owner last sampled it (every `MESH_BSP_RSSI_MS` while the receiver is on). |
| Who is talking | `mesh_radio_channel_busy()`, `mesh_radio_cad()`, `mesh_radio_cad_receive()` | `mesh_radio_channel_busy()` says at once whether a packet is arriving or a search is running. `mesh_radio_cad()` is the chip's channel activity detection for this setting: it waits for the search and hands the verdict back (an Active Object is answered with `detected`). `mesh_radio_cad_receive()` searches for a preamble only and, when it sees one, goes on to receive the packet from the sync word, without detecting the preamble again (CAD_RX): for an app that visits several settings in turn with a few symbols on each. And the events `preamble_detected`, `header_received` and `mesh_status().arriving` as a packet comes in. |
| The antennas | `mesh_radio_antennas_confirm()` | The module ships with its antennas loose and the chip cannot tell a missing one, so until the app says they are on (after the person has said so) every transmit, on both bands and in every build, `CONFIG_MESH_LAB` included, is held to the **safe power**, 0 dBm (`MESH_REGION_SAFE_DBM_X10`, `CONFIG_MESH_SAFE_POWER_CDBM`). `mesh_status().power_capped` says when the setting in force goes out below what was asked because of it. If an amplifier trips its over-current alarm, its band is held there again (`antenna_suspected`) until the app confirms once more. It returns at once, with or without the module, and the owner puts the new power on a setting in force; the SDK keeps it in RAM only, and a module pulled out takes it with it, so an app gives it after every `mesh_begin()` and every `module_found`. |
| Test signals | `mesh_radio_test(mode, ms)` | An unmodulated carrier or a bare LoRa preamble for `ms` (at most `MESH_BSP_TEST_MAX_MS`), for an analyser. **Only in a build with `CONFIG_MESH_LAB`**: any other build refuses it, says "a test signal is a lab build's: this build has no CONFIG_MESH_LAB", and keys nothing. **Into a 50-ohm load or an analyser ONLY, never an antenna.** The SDK takes it off air at its time, on `mesh_stop()`, on `mesh_radio_standby()` and when the app restarts or goes home; a crash or a watchdog reset does not. |
| The GNSS | `mesh_gnss_begin()`, `mesh_gnss_end()`, `mesh_gnss_reset()` (a request: it restarts the receiver), `mesh_gnss_fix()`, `mesh_gnss_pps()` | The receiver on, its NMEA parsed into a fix - where, how high, how fast and which way, how many satellites, the type of fix and its dilutions, the UTC time, how old it is - and its 1PPS line level. `mesh_gnss_end()` puts a ME31GR02 to sleep with its datasheet's Sleep command (it draws about 17.5 mA running, read or not) and `mesh_gnss_begin()` wakes it with a byte on its UART, from a hot start; `status.gnss_asleep` says so. A board's rest puts it to sleep too, and its wake wakes it. |
| I2C pair | `mesh_i2c_lend()`, `mesh_i2c_return()` | J3's SDA and SCL, handed to the app for its own bus (an Active Object is answered with the two GPIOs). The SDK never masters the pair itself: it is the GNSS's second face, and mastering it fights the receiver. |
| Free lines | none | The module uses all fourteen J3 positions, and the SDK holds every one it can drive (DIO8 included, left alone) while a module is there, so kode-sdk's `kode_pin_*()` and `kode_sys_exp_*()` refuse them: they are lent to `"mesh"`. `mesh_header_position()` says which J3 position one of the module's lines is on. |
| The module | `mesh_begin()`, `mesh_is_present()`, `mesh_wait_present()`, `mesh_status()`, `mesh_stop()`, `mesh_end()`, `mesh_header_position()` | Found by its fingerprint and the radio's answer, and followed: unplugged, its positions are handed back. Another module on J3 (a Camera, a Robotics, a F*CK) is named in `mesh_status()`. `mesh_end()` is for an app that is done with the module and goes on running: everything off, the lines and J3's supply handed back, no more looking; `mesh_begin()` starts it again. An app that is leaving needs no call: kodeOS takes the supply off. |
| Why a call failed | `mesh_error()` | A line of text for the last call of the calling task that did not go through, in the app's words (no module, no setting yet, a packet on air, a value the region does not allow, a call from an Active Object...). Per task, like `kode_error()`, for the last four tasks that called `mesh.h`. To act on a failure, test the bool. An Active Object's answers carry a `why`, which `mesh_sys_why_text()` puts in words. |
| Events | `mesh_event_add_listener()`, `mesh_event_wait()` (tasks); `mesh_sys_subscribe()` (Active Objects) | By name or by signal: `module_found` / `MESH_MODULE_FOUND_SIG`, `module_lost`, `wrong_module`, `packet_received`, `listen_ended`, `radio_failed`, `test_ended`, `preamble_detected`, `header_received`, `header_failed`, and `setting_lost` when the module went away and `setting_restored` when it returned (the SDK re-applies the radio setting, the receiver and the GNSS it was holding). A listener runs on the SDK's own task `mesh_events`, never on the owner's; an Active Object's news are posted to its queue leaving a margin free. |

`mesh.h` is the contract of the standard layer: every call says whether it blocks or returns at once, and returns a `bool` (`true` is success: a check written as `!= KODE_OK` compiles and means the opposite), except five that cannot fail: `mesh_error()` returns a text, `mesh_header_position()` a position and `mesh_radio_send_wait_ms()` milliseconds, and the bool of `mesh_is_present()` and `mesh_gnss_pps()` is the answer itself. `mesh_sys.h` is the contract of the extended one: it says which calls are commands, requests, immediates and news, and what each returns (`KODE_OK`, `KODE_DENIED`, `KODE_NO_RESOURCES`, `KODE_TIMEOUT` and `KODE_IO_ERROR` - five). The headers' own tables say why a call is refused and what the SDK keeps when the module goes, the board sleeps or goes home, or the app calls `mesh_end()`.

## How it works

`mesh_begin()` (or `mesh_sys_begin()`) asks J3's supply on (kode-sdk's `kode_sys_exp_power(true)`, which needs `CONFIG_KODEOS_RADIO=y`; the rail's state when an app starts is not promised) and starts one Active Object, the **owner**: `AO_Mesh`, of kode-sdk's own kind (`kqp.h`), in the component `mesh_aos`. It looks every so often for a Mesh on J3 by its **fingerprint** - the four lines the module's pull resistors hold high: the I2C clock and data, the LoRa chip select and the LoRa reset - read against the P4's pull-downs over a few rounds. When the pattern matches, the owner lends J3's positions, takes an SPI and a UART controller from kode-sdk's books (`kode_sys_exp_ctrl()`), opens the LR2021 and asks its version: a radio that answers is the module, and that same answer is how the owner knows, later, that it is still there.

From then on it is the only code that touches the radio and the GNSS. Every 20 ms its time event reads the radio's interrupt line, moves a received packet into the ring, reads the GNSS's UART and spends the sub-GHz duty budget; the radio's interrupt line also posts it an event of its own, so a packet or the end of a listen is read at once. A request is an event from kode-sdk's pools (a command is posted leaving a margin of the queue free); a send's payload waits in one of two slots of the owner's, not in the event, so no request ever takes a block of the pool kodeOS keeps for installs.

The calls in `mesh.h` that set something up or send ask the owner and wait for its answer on the caller's task: they **block**, briefly (the longest, a calibration after `mesh_radio_begin()`, is under a second; a send, the packet's airtime). Call them from a task, never from an Active Object's step. The same requests from an Active Object are `mesh_sys.h`'s: the owner answers them by a signal when it is done, and the calling Active Object's step goes on. `mesh_sys_status_read()`, `mesh_sys_radio_read()` and the other immediates read what the owner left and return at once.

The owner is one: both layers post their requests to the same `AO_Mesh`, a task's with a semaphore to wait on and an Active Object's with a done function the owner calls once, on its own task, when the request is answered. Its events go the same two ways: to named listeners on the SDK's task `mesh_events`, and as signals to every subscribed Active Object.

## Things that surprise

- **J3's supply is not promised until `mesh_begin()`.** kode-sdk does not promise +3.3V_EXT's state when an app starts. `mesh_begin()` asks it on over the link to the Dot's C5, which a Wi-Fi scan of the Dot's own can hold for up to some 40 s: `mesh_wait_present()` waits for that too, and `mesh_status().supply` says how it went. Without the radio (`CONFIG_KODEOS_RADIO=n`) J3 is always powered and nothing can switch it off: the module is found as soon as it is plugged.
- **An antenna before any transmit, and a load for a test signal.** The LR2021's amplifiers are guaranteed only into a load. `mesh_radio_send()` and `mesh_radio_test()` key one, so an 868 MHz antenna (or a 50-ohm terminator) belongs on the sub-GHz SMA (J501), and a 2.4 GHz one on J502, before transmitting on that band. Keying into an open connector can damage the amplifier.
- **0 dBm until the app confirms the antennas.** The module ships with its antennas loose, and the chip cannot tell a missing antenna from a hand around one. So an app that never calls `mesh_radio_antennas_confirm(true)` transmits at 0 dBm whatever it asks, `CONFIG_MESH_LAB` builds included; `mesh_status().power_capped` says so. Ask the person, keep the answer, and give it after every `mesh_begin()` and every `module_found` (a module pulled out takes the answer with it). An amplifier alarm (`radio_failed` with `MESH_RADIO_ERROR_AMPLIFIER_FAULT`) holds that band at 0 dBm again until the app confirms once more: ask whether an antenna is loose. The SDK never begins the radio again by itself, so a band that keeps tripping stays at 0 dBm.
- **The test signal is a lab build's.** A carrier is on air all the time it lasts, a hundred times the 868 band's 1 % limit, and the duty budget does not count it. So `mesh_radio_test()` exists only in a build with `CONFIG_MESH_LAB`, and goes into a 50-ohm load or an analyser and never an antenna. Any other build refuses it and keys nothing.
- **The power has a cap, and a request above it gets the cap.** `mesh_region.h` holds 868.0-868.6 MHz, 8.5 dBm, 1 % of the time, BW125, and 2400-2483.5 MHz, 4.5 dBm, BW812. The 8.5 and 4.5 dBm are nominal levels. Nothing above them goes out, so `mesh_radio_begin(..., 125)` (12.5 dBm) is held to 85. The numbers live in that one file. `CONFIG_MESH_LAB`, in `idf.py menuconfig` under "Mesh module SDK", lifts them as far as the top of the default amplifier row (19 dBm and 10 dBm), and with `CONFIG_MESH_LAB_PA_TABLE` up to the chip's own top (+22 dBm and +12 dBm, datasheet 7.4) on the datasheet's amplifier rows, at your own risk; a firmware built with it is not one to distribute, and its help says all that it lifts.
- **The duty cycle is counted, not just documented.** On 868 the SDK holds a budget of airtime: it holds 36 seconds, starts full, refills at 1 % of real time, and each send spends its airtime. In the long run that is 1 %; from a full budget, up to 72 seconds can go out in the first hour. A send with no budget is refused, not queued; `mesh_radio_send_wait_ms()` says the wait. The budget is the owner's: unplugging and plugging the module again does not refill it. It lives in RAM, so an app that restarts starts with it full: an app that needs "36 seconds in any hour", or that restarts often, counts its own airtime on top.
- **The radio has no antenna switch.** Sub-GHz and 2.4 GHz each have their own amplifier and SMA; a band is chosen at `mesh_radio_begin()`, and the chip is set for that path.
- **The I2C pair is the GNSS's.** The pair is the receiver's second face, and its stock firmware can drive the data line. The SDK never masters it; `mesh_i2c_lend()` hands the two GPIOs to an app that wants the bus and knows the risk.
- **Upright only, for now.** The SDK recognises the module the way up it is meant to sit. A module turned 180 degrees reads as "not a Mesh" rather than being driven by a guessed pin-map.
- **One module's SDK per app.** The owner holds J3 while the app runs, module or not, and `AO_Mesh` sits at `CONFIG_MESH_AO_PRIORITY` (default `KODE_PRIO_USER_HIGH`), where another module's SDK puts its own; the signals are in a range of their own (`CONFIG_MESH_SIG_OFFSET`, 1000).
- **Going home takes the radio off air.** On `HOME_SIG` (the bottom button held) and when the board goes to rest (kode-sdk's rest signal; with `CONFIG_KODE_LOWPOWER`), the owner puts the radio to standby and, once the chip confirms it, to sleep with its reset line high (a chip that will not confirm standby has its reset line held low instead); every restart on purpose holds it low as well (the radio driver's restart hook). A crash, a watchdog or the reset button runs none of that: in a build with `CONFIG_MESH_LAB`, a carrier from `mesh_radio_test()` then stays up until the module loses power, which is one more reason it goes into a load.
- **Bytes from the GNSS are lost while flash is written**, unless the app sets `CONFIG_UART_ISR_IN_IRAM=y`, as the examples and `tests/target` do. It costs about 8 KB of internal RAM.
- **Change modules with the Dot off**, or wait for `module_lost` before plugging in another.
- **The owner puts the setting back.** When the module returns or the board wakes, the SDK restores the radio setting, the receiver and the GNSS the app had running, and says so (`setting_restored`). An app should wait for that, not ask again. It never repeats a send or a test signal.
- **A call from the wrong layer is a refusal, not a crash.** `mesh.h` from an Active Object says so by name; `mesh_sys.h` from a task is fine (it never waits, so it is safe anywhere).
- **The drivers are not for an app.** The headers of `mesh_drv` can be reached from an app's sources, and they hold none of the region's limits: those are the owner's. An app that calls a driver leaves the region's limits behind, and must not.

## Examples

One ESP-IDF project per part, each an app for kodeOS laid out as kode-sdk's examples are. The first two are tasks (`mesh.h`), the next three Active Objects (`mesh_sys.h`); each file says at its head which layer it uses and why.

| Example | Layer | What it shows |
|---|---|---|
| [`examples/module`](../examples/module) | `mesh.h`, a task | The module found on J3 and what the owner knows of it (`mesh_status()`), heard by name with listeners and `mesh_event_wait()`, and noticed when it leaves. Never transmits. |
| [`examples/gnss`](../examples/gnss) | `mesh.h`, a task | The receiver on and what it reports every two seconds: sentences, satellites and, once there is a fix, HDOP and UTC. The position itself is never logged. |
| [`examples/receive`](../examples/receive) | `mesh_sys.h`, an Active Object | A band set and the receiver on; every packet logged with its length, RSSI, SNR and CRC verdict. Never transmits. |
| [`examples/send`](../examples/send) | `mesh_sys.h`, an Active Object | One short packet a minute on 868 at the region's cap, waiting for the duty cycle. **Nothing is sent until the top button is pressed** (a second press stops it; a module pulled out stops it too); the press is also the person's word on the antenna (`mesh_sys_radio_antennas_confirm()`). **Transmits: an antenna or a 50-ohm load on J501 first.** |
| [`examples/console_actions`](../examples/console_actions) | `mesh_sys.h`, an Active Object | The module on kodeOS's console: 12 functions and 23 variables. A row calls a command or an immediate itself, or hands a request to the example's Active Object. |

Run every one with the module on J3 and the Dot on a USB-C cable. Their `sdkconfig.defaults` set `CONFIG_KODEOS_RADIO=y` with Wi-Fi, networking and channels off, as kode-sdk's `ir_remote` example does, and `CONFIG_UART_ISR_IN_IRAM=y`: copy those lines.

## Options

| Kconfig | Default | What it does |
|---|---|---|
| `CONFIG_MESH_AO_PRIORITY` | 0 (`KODE_PRIO_USER_HIGH`) | The QP/C priority of `AO_Mesh`. Two Active Objects at one level stop the board when the second starts: an app whose own AO needs the high level moves this one. |
| `CONFIG_MESH_SIG_OFFSET` | 1000 | Where this SDK's signals start, above `MAX_SIG` (your own are free below it). The Robotics, Camera and F*CK SDKs take 1100, 1200 and 1300. |
| `CONFIG_MESH_SAFE_POWER_CDBM` | 0 (0 dBm) | The power of every transmit until the app confirms the antennas, in hundredths of a dBm, from -10 dBm to the 2.4 GHz band's cap. |
| `CONFIG_MESH_LAB` | off | A build for testing the module. Lifts the region's limits: the power to the top of the default amplifier row (19 dBm on LF, 10 dBm on HF), the frequency over each of the chip's paths, and with `mesh_radio_configure()` the free numbers of the packet and the SDK's count of the 868 duty cycle; and it is the only build with the test signal. Its help says what is left. It is not a build for a firmware that is distributed. |
| `CONFIG_MESH_LAB_PA_TABLE` | off | With `CONFIG_MESH_LAB`, the datasheet's amplifier rows above the default one, up to +22 dBm and +12 dBm: at your own risk, as they can damage the amplifier. |
| `CONFIG_KODEOS_RADIO` (kode-sdk's) | `y` in the examples | Lets the SDK switch J3's supply, through the Dot's C5. With `n`, J3 is always powered and the module is found all the same. |
| `CONFIG_UART_ISR_IN_IRAM` (ESP-IDF's) | `y` in the examples | The GNSS's bytes survive a flash write. |

## The console

[`examples/console_actions`](../examples/console_actions) puts the module on kodeOS's console as rows a person or kode-cli can type: `function name="mesh_begin" arg=sub_ghz`, `variable name="mesh_present"`. As in kode-sdk, the rows are the app's: one that wants them copies the example's tables, or the rows it needs. A row's `run` is called on the console's task and may not wait, and calls only what `mesh_sys.h` makes safe there: a **command** (`mesh_rx`, `mesh_listen`, `mesh_standby`, `mesh_stop`) or an **immediate** (`mesh_radio_rssi`) it makes itself and answers with what the call returned; a **request** (`mesh_begin`, `mesh_radio_send`, `mesh_radio_test`, `mesh_gnss`) it posts to the example's Active Object, which asks the SDK, and the row answers `>ok` once accepted. What came of a request - its answer - goes to the log and to the variable `mesh_last`. The two rows that transmit, `mesh_radio_send` and `mesh_radio_test`, refuse until `mesh_tx_arm` has been given 1, and `mesh_stop` (a command, so it never waits behind a request) disarms. Arming is also the SDK's word on the antennas (`mesh_sys_radio_antennas_confirm()`): unarmed, the module stays at the safe power, 0 dBm. `mesh_radio_test` is answered with a refusal by the SDK unless the build sets `CONFIG_MESH_LAB`.

| Functions | Variables |
|---|---|
| `mesh_tx_arm`, `mesh_sf`, `mesh_power_x10`, `mesh_begin`, `mesh_rx`, `mesh_radio_rssi`, `mesh_listen`, `mesh_radio_send`, `mesh_radio_test`, `mesh_standby`, `mesh_stop`, `mesh_gnss` | `mesh_present`, `mesh_seen`, `mesh_tx_armed`, `mesh_ready`, `mesh_band_now`, `mesh_sf_now`, `mesh_power_now`, `mesh_sending`, `mesh_receiving`, `mesh_testing`, `mesh_fault`, `mesh_duty_ms`, `mesh_rx_count`, `mesh_rx_dropped`, `mesh_rx_waiting`, `mesh_tx_count`, `mesh_rssi_dbm`, `mesh_gnss_on`, `mesh_gnss_fix`, `mesh_gnss_sats`, `mesh_gnss_count`, `mesh_gnss_baud`, `mesh_last` |

## What it costs an app

| Resource | Taken |
|---|---|
| J3 | Positions 1-12 under `"mesh"`, and the I2C pair apart, while the module is held; the four fingerprint lines for a few milliseconds every look while it is not. |
| Controllers | One SPI host and one UART, from kode-sdk's books under the module's tag (`kode_sys_exp_ctrl()`), given back with the lines. |
| The owner | `AO_Mesh`: an 8 KB stack in internal RAM, a queue of 16 events, one pool event per request. A ring of four received packets (1 KB) and two send slots (510 B). |
| The standard layer's task | `mesh_events` (4 KB stack), made only when the app adds a listener or waits for an event; an app that uses only `mesh_sys.h` pays nothing for it. |
| Interrupts | One GPIO interrupt (the radio's DIO9), and the UART's. |
| A restart hook | One of ESP-IDF's five: the radio's reset line held low on a restart on purpose. |

`tests/target/ram_budget.txt` holds what an app that uses the module starts with in internal RAM, and `tools/check_ram.sh` keeps it there.

## Layout

The tree is kode-sdk's: one component per layer, each part in a folder of its own with its header in `include/` and its source in `src/`.

| Path | What it holds |
|---|---|
| `components/mesh` | The standard layer (`kode` in kode-sdk): `include/mesh.h`, what a task calls, with its own types, and `src/mesh.c`. |
| `components/mesh_sys` | The extended layer (`kode_sys` in kode-sdk): `include/mesh_sys.h`, what an Active Object calls, and `src/mesh_sys.c`. |
| `components/mesh_aos` | The owner (`kaos` in kode-sdk): what the two layers include in `core/include/mesh_aos.h`, and in `mesh/src` the Active Object `AO_Mesh` (`mesh_ao.c`), what its signals do (`mesh_aos.c`), the seam between them (`mesh_ao.h`), and the owner's rules in plain C (`mesh_plan.c`: power, duty cycle, fingerprint). |
| `components/mesh_drv` | The drivers (`kdrv` in kode-sdk), a folder each: `lr2021` (the radio over SPI), `lora` (LoRa's arithmetic, plain C), `gnss` (the receiver over UART), `nmea` (its sentences, plain C), `pins`. |
| `components/mesh_bsp` | The module described (`kbsp` in kode-sdk), in `board/include`: J3 positions and timings (`mesh_bsp.h`), the radio's limits (`mesh_region.h`) and the shared types (`mesh_types.h`). Only describes. |
| `examples/` | One project per part: `module` (presence, status and events: run it first; a task), `gnss` (a task), `receive` and `send` (Active Objects; send transmits) and `console_actions` (the module on kodeOS's console; an Active Object). |
| `tests/host` | The rules that are plain C, with Unity and nothing else, built for ESP-IDF's `linux` target: `mesh_plan`, `mesh_drv_lora` and `mesh_drv_nmea`. |
| `tests/host_drv` | The real LR2021 driver (`mesh_drv_lr2021.c`) over a model of the chip at the level of SPI frames, written from the datasheet: the timeout in every SetTx, the packet shape restored before a receive, the CRC verdict, the carrier's timer, the restart hook. |
| `tests/host_owner` | The real owner (`mesh_aos.c`, `mesh_aos_events.c`, `mesh_plan.c`) on FreeRTOS for the `linux` target, over a model of the radio that stays in Tx until something ends it, behind a stand-in for `AO_Mesh` (`main/fakes/`): module present, gone or the wrong one, that a test signal stops on every way out, the Active Object's way in (`mesh_aos_ask()`), the named events and their listeners and waiters, the deadlines, the standby ladder, the receiver left alone, the CRC verdict, a request that gave up never running twice, the payload slots, sleep and home. Its clock is the simulator's ticks. |
| `tests/host_ao` | `AO_Mesh` itself on kode-sdk's real QP/C, at the pinned release: its queue's and pool's margins, the tick, the radio's interrupt, the rest and wake signals, `HOME_SIG` and the level check. |
| `tests/host_standard` | `mesh.c`, the standard layer, over an owner that writes down what it is asked (`main/fakes/`): one case per return value of every public call, what the layer refuses (an Active Object's call by name), the events by name, and a call made in any state. |
| `tests/host_sys` | `mesh_sys.c`, the extended layer, on kode-sdk's real QP/C (real events, queues and pools) over an owner that only writes down what it is asked and answers when a case says so: one case per return value of every public call, the doors, the queue-depth rule, the news margin, and a call made in any state. |
| `tests/host_console` | `examples/console_actions` over a `mesh_sys.h` that writes down what it is asked, with kodeOS's console registry and its rules: every row taken, every function making the call its name says - a command or an immediate from its row, a request through the example's Active Object - the answers and the news on the log, the two that transmit refused until armed, and every variable a word the console sends, in every state. |
| `tests/host_lab` | The plan built with `CONFIG_MESH_LAB`: what that one switch lifts and what it leaves. `tests/host_lab_pa` is the same with `CONFIG_MESH_LAB_PA_TABLE` on: the datasheet's rows above the default one. |
| `tests/target` | The SDK built for the esp32p4 against the pinned `kode-sdk`, every call of `mesh.h` and `mesh_sys.h` named in `ALL[]` and linked, and its RAM budget (`ram_budget.txt`). |
| `tests/sanitize.cmake` | ASan and UBSan for the host suites, under `tools/host_tests.sh --sanitize`. |
| `tools/` | `host_tests.sh`, `check_layers.sh` (thirteen rules), `check_ram.sh`, `check_example_pins.sh`, `set_version.sh` (one version everywhere, for a release), and the checks' own tests. `check_misra.sh` and `build_cppcheck.sh` are the maintainer's, as in kode-sdk: they compare against a baseline that is not in this repository. |

The SDK builds with ESP-IDF v6.1 against kode-sdk v0.1.5. The components never pin kode-sdk: the app does.

## The checks

```sh
. <path-to-esp-idf>/export.sh
tools/host_tests.sh                    # the nine host suites, some minutes; --sanitize for ASan and UBSan
tools/check_layers.sh
tools/check_example_pins.sh
idf.py -C tests/target build           # some minutes the first time
tools/check_ram.sh tests/target/build/mesh_target.elf tests/target/ram_budget.txt
```

The host suites run on the FreeRTOS simulator of ESP-IDF's `linux` target. A test binary that has not finished after `MESH_HOST_TEST_MAX_S` seconds (600 by default) is stopped and the run fails.

## Working on the SDK itself

While you work on the SDK, `override_path: "<this repository>/components/mesh"` in place of the three lines of `kodediy/mesh` in your app's `main/idf_component.yml` points at a checkout. `mesh_sys`, `mesh_aos`, `mesh_drv` and `mesh_bsp` arrive through `mesh`'s own manifest. The examples' `CMakeLists.txt` and `sdkconfig.defaults` are the ones to copy: they fetch the Dot's settings from kode-sdk rather than repeating them.
