#pragma once
// ============================================================================
//  CrashLog — why did the device restart?  Written to the SD card.
//
//  • every line printed to the console (ESP_LOG, SC32_LOG, BELL_LOG, printf)
//    is also copied into a 16 KB ring in RAM that survives a reset
//    (no init section: kept over panic / watchdog / esp_restart, lost on
//    power-off)
//  • the panic handler's text ("Guru Meditation Error ...", register dump)
//    is captured as well, and the core dump summary (task, PC, backtrace)
//    is read from the coredump partition at the next boot
//  • after the restart, once the SD card is mounted (and the clock is set,
//    or after 60 s at the latest):
//      /sdcard/logs/boot.log          one line per start with the reason
//      /sdcard/logs/crash-NNNN.txt    full report for abnormal restarts
//                                     (panic, watchdog, brownout, ...):
//                                     reason, crash summary, panic output
//                                     and the last log lines before it
//    The 30 newest reports are kept.
//
//  Cost at runtime: one memcpy per printed line (no SD / flash access).
// ============================================================================
#include <string>

namespace crashlog {

/** First thing in app_main (internal stack: reads the core dump). */
void earlyInit(const char* firmwareVersion);

/** Call periodically (every few seconds, any task): writes the pending
 *  report as soon as the SD card is there. */
void poll(bool sdMounted);

/** Short description of the last reset ("power on", "crash", ...). */
std::string lastResetReason();

}  // namespace crashlog
