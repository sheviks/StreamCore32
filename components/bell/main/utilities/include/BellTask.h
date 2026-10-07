#ifndef BELL_TASK_H
#define BELL_TASK_H

#include <atomic>
#include <cstdint>
#include <new>
#include <string>
#include <vector>
#include "BellUtils.h"
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace bell {
class Task {
 public:
  std::string TASK;
  int stackSize, core;
  bool runOnPSRAM;
  Task(std::string taskName, int stackSize, int priority, int core,
       bool runOnPSRAM = true) {
    this->TASK = taskName;
    this->stackSize = stackSize;
    this->core = core;
    this->runOnPSRAM = runOnPSRAM;
#ifdef ESP_PLATFORM
    this->priority = priority;
    if (this->priority <= ESP_TASK_PRIO_MIN)
      this->priority = ESP_TASK_PRIO_MIN + 1;
#endif
  }
  virtual ~Task() {
    while (isRunning.load() > 0) {
      BELL_SLEEP_MS(10);
    };
  }

  bool startTask() {
#ifdef ESP_PLATFORM
    const size_t stack_words =
        (this->stackSize + sizeof(StackType_t) - 1) / sizeof(StackType_t);
    if (runOnPSRAM) {
      // Every run gets its own stack + TCB (freed after the task ended), so
      // a task can be started again while the previous run is still exiting.
      auto* run = new (std::nothrow) PsramRun{this, nullptr, nullptr};
      if (!run)
        return false;
      run->stack =
          (StackType_t*)heap_caps_malloc(stack_words * sizeof(StackType_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      run->tcb = (StaticTask_t*)heap_caps_calloc(
          1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!run->stack || !run->tcb) {
        printf("[Task %s] stack/TCB alloc failed\n", TASK.c_str());
        freeRun(run);
        return false;
      }
      isRunning.fetch_add(1);
      if (xTaskCreateStaticPinnedToCore(taskEntryFuncPSRAM, this->TASK.c_str(),
                                        stack_words, run, this->priority,
                                        run->stack, run->tcb,
                                        this->core) == NULL) {
        isRunning.fetch_sub(1);
        freeRun(run);
        return false;
      }
      return true;
    } else {
      printf("task on internal %s", this->TASK.c_str());
      esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
      cfg.stack_size = stackSize;
      cfg.inherit_cfg = true;
      cfg.thread_name = this->TASK.c_str();
      cfg.pin_to_core = core;
      cfg.prio = this->priority;
      esp_pthread_set_cfg(&cfg);
    }
#endif
    isRunning.fetch_add(1);
#if _WIN32
    thread = CreateThread(NULL, stackSize,
                          (LPTHREAD_START_ROUTINE)taskEntryFunc, this, 0, NULL);
    if (thread == NULL)
      isRunning.fetch_sub(1);
    return thread != NULL;
#else
    if (!pthread_create(&thread, NULL, taskEntryFunc, this)) {
      pthread_detach(thread);
      return true;
    }
    isRunning.fetch_sub(1);
    return false;
#endif
  }

 protected:
  virtual void runTask() = 0;

 private:
#if _WIN32
  HANDLE thread;
#else
  pthread_t thread;
#endif
  std::atomic<int> isRunning{0};
#ifdef ESP_PLATFORM
  int priority;
  struct PsramRun {
    Task* self;
    StaticTask_t* tcb;
    StackType_t* stack;
  };
  static void freeRun(PsramRun* run) {
    if (run->stack)
      heap_caps_free(run->stack);
    if (run->tcb)
      heap_caps_free(run->tcb);
    delete run;
  }

  static void taskEntryFuncPSRAM(void* arg) {
    PsramRun* run = (PsramRun*)arg;
    Task* self = run->self;
    self->runTask();

    // stack + TCB are still in use until the IDLE task cleaned up the
    // deleted task: free them a bit later
    TimerHandle_t timer =
        xTimerCreate("cleanup", pdMS_TO_TICKS(5000), pdFALSE, run,
                     [](TimerHandle_t xTimer) {
                       freeRun((PsramRun*)pvTimerGetTimerID(xTimer));
                       xTimerDelete(xTimer, portMAX_DELAY);
                     });
    xTimerStart(timer, portMAX_DELAY);
    self->isRunning.fetch_sub(1);
    vTaskDelete(NULL);
  }
#endif

  static void* taskEntryFunc(void* This) {
    ((Task*)This)->runTask();
    ((Task*)This)->isRunning.fetch_sub(1);
    return NULL;
  }
};
}  // namespace bell

#endif