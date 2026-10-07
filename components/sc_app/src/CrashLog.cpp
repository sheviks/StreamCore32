#include "CrashLog.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr uint32_t kMagic = 0x5C32106Bu;
constexpr size_t kRing = 16 * 1024;
constexpr size_t kPanic = 3 * 1024;
constexpr const char* kDir = "/sdcard/logs";
constexpr int kKeepReports = 30;

// survives software resets (panic, watchdog, esp_restart), not power-off
struct Persist {
  uint32_t magic;
  uint32_t bootCount;
  uint32_t head;      // next write position in ring
  uint32_t filled;    // bytes valid in ring (<= kRing)
  uint32_t panicLen;  // bytes in panic
  int64_t lastEpoch;  // last known wall clock (s), 0 = never set
  int64_t lastUptimeMs;
  char ring[kRing];
  char panic[kPanic];
};
__NOINIT_ATTR Persist s_p;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
FILE* s_origStdout = nullptr;

// report of the previous session (PSRAM), written to SD by poll()
std::string* s_report = nullptr;
std::string* s_bootLine = nullptr;
bool s_abnormal = false;
std::string s_resetText = "unknown";
bool s_haveCoreDump = false;
const char* s_fw = "";

void IRAM_ATTR ringPut(const char* buf, size_t n) {
  if (n > kRing) {  // keep the tail only
    buf += n - kRing;
    n = kRing;
  }
  portENTER_CRITICAL_SAFE(&s_mux);
  size_t first = std::min(n, (size_t)(kRing - s_p.head));
  memcpy(s_p.ring + s_p.head, buf, first);
  memcpy(s_p.ring, buf + first, n - first);
  s_p.head = (uint32_t)((s_p.head + n) % kRing);
  s_p.filled = (uint32_t)std::min(kRing, (size_t)s_p.filled + n);
  portEXIT_CRITICAL_SAFE(&s_mux);
}

std::string ringText() {
  std::string out;
  if (s_p.filled > kRing || s_p.head >= kRing)
    return out;
  out.reserve(s_p.filled);
  if (s_p.filled < kRing) {
    out.assign(s_p.ring, s_p.filled);
  } else {
    out.assign(s_p.ring + s_p.head, kRing - s_p.head);
    out.append(s_p.ring, s_p.head);
    size_t nl = out.find('\n');  // drop the cut first line
    if (nl != std::string::npos)
      out.erase(0, nl + 1);
  }
  // remove colour escapes / NULs for a clean text file
  std::string clean;
  clean.reserve(out.size());
  for (size_t i = 0; i < out.size(); i++) {
    char c = out[i];
    if (c == 0x1b) {  // ESC [ ... m
      while (i < out.size() && out[i] != 'm')
        i++;
      continue;
    }
    if (c == '\r' || c == 0)
      continue;
    clean += c;
  }
  return clean;
}

// stdout replacement: write through to the console, keep a copy
int stdoutWrite(void*, const char* buf, int n) {
  if (n <= 0)
    return n;
  ringPut(buf, (size_t)n);
  if (s_origStdout) {
    fwrite(buf, 1, (size_t)n, s_origStdout);
    fflush(s_origStdout);
  }
  return n;
}

const char* resetName(esp_reset_reason_t r, bool* abnormal) {
  *abnormal = true;
  switch (r) {
    case ESP_RST_POWERON:
      *abnormal = false;
      return "power on";
    case ESP_RST_EXT:
      *abnormal = false;
      return "external reset pin";
    case ESP_RST_SW:
      *abnormal = false;
      return "software restart (esp_restart)";
    case ESP_RST_DEEPSLEEP:
      *abnormal = false;
      return "wake from deep sleep";
    case ESP_RST_USB:
      *abnormal = false;
      return "USB reset";
    case ESP_RST_JTAG:
      *abnormal = false;
      return "JTAG reset";
    case ESP_RST_PANIC:
      return "CRASH (exception / abort / panic)";
    case ESP_RST_INT_WDT:
      return "CRASH (interrupt watchdog: interrupts blocked too long)";
    case ESP_RST_TASK_WDT:
      return "CRASH (task watchdog: a task hung)";
    case ESP_RST_WDT:
      return "CRASH (other watchdog)";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT (supply voltage dropped)";
    case ESP_RST_SDIO:
      return "SDIO reset";
    case ESP_RST_EFUSE:
      return "efuse error";
    case ESP_RST_PWR_GLITCH:
      return "power glitch";
    case ESP_RST_CPU_LOCKUP:
      return "CPU lockup";
    default:
      return "unknown";
  }
}

const char* excName(uint32_t c) {
  switch (c) {
    case 0: return "IllegalInstruction";
    case 2: return "InstructionFetchError";
    case 3: return "LoadStoreError";
    case 6: return "IntegerDivideByZero";
    case 9: return "LoadStoreAlignment";
    case 12: return "InstrPIFDataError";
    case 13: return "LoadStorePIFDataError";
    case 20: return "InstrFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "";
  }
}

std::string fmtTime(int64_t epoch) {
  if (epoch < 1600000000)
    return "unknown";
  time_t t = (time_t)epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char b[32];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
  return b;
}

std::string fmtUptime(int64_t ms) {
  int64_t s = ms / 1000;
  char b[48];
  snprintf(b, sizeof b, "%lldh %02lldm %02llds", (long long)(s / 3600),
           (long long)(s / 60 % 60), (long long)(s % 60));
  return b;
}

std::string coreDumpText() {
  std::string out;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  if (esp_core_dump_image_check() != ESP_OK)
    return out;
  s_haveCoreDump = true;
  esp_core_dump_summary_t* sum =
      (esp_core_dump_summary_t*)calloc(1, sizeof(esp_core_dump_summary_t));
  if (!sum)
    return out;
  if (esp_core_dump_get_summary(sum) == ESP_OK) {
    char b[160];
    snprintf(b, sizeof b, "task:  %.16s\npc:    0x%08lx\n", sum->exc_task,
             (unsigned long)sum->exc_pc);
    out += b;
    snprintf(b, sizeof b, "cause: %lu %s   address: 0x%08lx\n",
             (unsigned long)sum->ex_info.exc_cause, excName(sum->ex_info.exc_cause),
             (unsigned long)sum->ex_info.exc_vaddr);
    out += b;
    out += "Backtrace:";
    for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; i++) {
      snprintf(b, sizeof b, " 0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
      out += b;
    }
    out += sum->exc_bt_info.corrupted ? "  |<-CORRUPTED\n" : "\n";
    out +=
        "(decode: xtensa-esp32s3-elf-addr2line -pfiaC -e "
        "build/StreamCore32-esp32.elf <addresses>)\n";
    snprintf(b, sizeof b, "app ELF sha256: %.16s\n", (const char*)sum->app_elf_sha256);
    out += b;
  }
  free(sum);
#endif
  return out;
}

void eraseCoreDump() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  esp_core_dump_image_erase();
#endif
}

bool clockValid() {
  return time(nullptr) > 1600000000;
}

void ensureDir() {
  struct stat st;
  if (stat(kDir, &st) != 0)
    mkdir(kDir, 0775);
}

// next free report number, deletes the oldest beyond kKeepReports
int nextReportNumber() {
  std::vector<int> nums;
  if (DIR* d = opendir(kDir)) {
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
      int n;
      if (sscanf(e->d_name, "crash-%d.txt", &n) == 1)
        nums.push_back(n);
    }
    closedir(d);
  }
  std::sort(nums.begin(), nums.end());
  while ((int)nums.size() >= kKeepReports) {
    char p[64];
    snprintf(p, sizeof p, "%s/crash-%04d.txt", kDir, nums.front());
    unlink(p);
    nums.erase(nums.begin());
  }
  return nums.empty() ? 1 : nums.back() + 1;
}

}  // namespace

// ---- panic handler output (linked with --wrap, see CMakeLists.txt) ----------
extern "C" {
void __real_panic_print_str(const char* str);
void __real_panic_print_hex(int h);
void __real_panic_print_dec(int d);

static void IRAM_ATTR panicPut(const char* s, size_t n) {
  if (s_p.magic != kMagic)
    return;
  for (size_t i = 0; i < n && s_p.panicLen < kPanic; i++)
    s_p.panic[s_p.panicLen++] = s[i];
}
void IRAM_ATTR __wrap_panic_print_str(const char* str) {
  if (str) {
    size_t n = 0;
    while (str[n])
      n++;
    panicPut(str, n);
  }
  __real_panic_print_str(str);
}
void IRAM_ATTR __wrap_panic_print_hex(int h) {
  static DRAM_ATTR const char hex[] = "0123456789abcdef";  // usable with cache off
  char b[10];
  for (int i = 0; i < 8; i++)
    b[i] = hex[((unsigned)h >> (28 - 4 * i)) & 0xf];
  panicPut(b, 8);
  __real_panic_print_hex(h);
}
void IRAM_ATTR __wrap_panic_print_dec(int d) {
  char b[12];
  int n = 0;
  unsigned v = d < 0 ? (unsigned)-d : (unsigned)d;
  char t[12];
  int k = 0;
  do {
    t[k++] = '0' + v % 10;
    v /= 10;
  } while (v && k < 11);
  if (d < 0)
    b[n++] = '-';
  while (k)
    b[n++] = t[--k];
  panicPut(b, n);
  __real_panic_print_dec(d);
}
}

namespace crashlog {

void earlyInit(const char* fw) {
  s_fw = fw;
  esp_reset_reason_t rr = esp_reset_reason();
  bool abnormal = false;
  s_resetText = resetName(rr, &abnormal);
  s_abnormal = abnormal;

  bool valid = s_p.magic == kMagic && s_p.head < kRing && s_p.filled <= kRing &&
               s_p.panicLen <= kPanic && rr != ESP_RST_POWERON;
  if (!valid) {
    memset(&s_p, 0, sizeof(s_p));
    s_p.magic = kMagic;
  }
  s_p.bootCount++;

  // report about the previous session
  std::string prevLog = valid ? ringText() : std::string();
  std::string panicText = valid ? std::string(s_p.panic, s_p.panicLen) : std::string();
  std::string dump = coreDumpText();
  int64_t lastEpoch = valid ? s_p.lastEpoch : 0;
  int64_t lastUp = valid ? s_p.lastUptimeMs : 0;

  s_bootLine = new std::string();
  {
    char b[200];
    snprintf(b, sizeof b, " boot #%lu  fw %s  reset: %s", (unsigned long)s_p.bootCount,
             fw, s_resetText.c_str());
    *s_bootLine = b;
    if (valid) {
      *s_bootLine += "  (previous run: " + fmtUptime(lastUp) + ", last alive " +
                     fmtTime(lastEpoch) + ")";
    }
  }
  if (abnormal || !dump.empty()) {
    s_abnormal = true;
    std::string& r = *(s_report = new std::string());
    r.reserve(prevLog.size() + panicText.size() + 2048);
    r += "==== StreamCore32 restart report ====\n";
    r += "firmware:      " + std::string(fw) + "\n";
    r += "boot:          #" + std::to_string(s_p.bootCount) + "\n";
    r += "reset reason:  " + s_resetText + "\n";
    if (valid) {
      r += "previous run:  " + fmtUptime(lastUp) + " uptime, last alive " +
           fmtTime(lastEpoch) + "\n";
    } else {
      r += "previous run:  no data (power was off)\n";
    }
    r += "\n---- crash summary (core dump) ----\n";
    r += dump.empty() ? "(none)\n" : dump;
    r += "\n---- panic handler output ----\n";
    r += panicText.empty() ? "(none)\n" : panicText + "\n";
    r += "\n---- last log lines before the restart ----\n";
    r += prevLog.empty() ? "(none)\n" : prevLog;
  }

  // new session
  s_p.head = 0;
  s_p.filled = 0;
  s_p.panicLen = 0;
  s_p.lastUptimeMs = 0;

  // copy everything printed to stdout into the ring
  s_origStdout = stdout;
  FILE* f = funopen(nullptr, nullptr, stdoutWrite, nullptr, nullptr);
  if (f) {
    setvbuf(f, nullptr, _IOLBF, 256);
    _GLOBAL_REENT->_stdout = f;
    stdout = f;
  }
  printf("crashlog: boot #%lu, reset reason: %s%s\n", (unsigned long)s_p.bootCount,
         s_resetText.c_str(), s_haveCoreDump ? " (core dump found)" : "");
}

void poll(bool sdMounted) {
  // keep the "last alive" information fresh (for the next report)
  s_p.lastUptimeMs = esp_timer_get_time() / 1000;
  if (clockValid())
    s_p.lastEpoch = time(nullptr);

  if (!s_bootLine || !sdMounted)
    return;
  // wait for the clock (SNTP) so the files carry a date; 60 s at most
  if (!clockValid() && esp_timer_get_time() < 60LL * 1000 * 1000)
    return;

  ensureDir();
  std::string now = fmtTime(time(nullptr));
  std::string reportName;
  if (s_report) {
    char p[64];
    snprintf(p, sizeof p, "%s/crash-%04d.txt", kDir, nextReportNumber());
    if (FILE* f = fopen(p, "w")) {
      fprintf(f, "written:       %s\n", now.c_str());
      fwrite(s_report->data(), 1, s_report->size(), f);
      fclose(f);
      reportName = p;
      printf("crashlog: report written to %s\n", p);
    }
  }
  if (FILE* f = fopen("/sdcard/logs/boot.log", "a")) {
    fprintf(f, "%s%s%s\n", now.c_str(), s_bootLine->c_str(),
            reportName.empty() ? "" : ("  -> " + reportName.substr(13)).c_str());
    fclose(f);
  }
  delete s_bootLine;
  s_bootLine = nullptr;
  delete s_report;
  s_report = nullptr;
  if (s_haveCoreDump) {
    s_haveCoreDump = false;
    // flash erase needs an internal stack: hand it to a short task
    xTaskCreate([](void*) {
      eraseCoreDump();
      vTaskDelete(nullptr);
    }, "cd_erase", 3072, nullptr, 1, nullptr);
  }
}

std::string lastResetReason() {
  return s_resetText;
}

}  // namespace crashlog
