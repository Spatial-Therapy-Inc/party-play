# The profiling build only (README, 性能): LVGL's built-in profiler for `perf trace`. Never in a release.
CONFIG_LV_USE_PROFILER=y
CONFIG_LV_USE_PROFILER_BUILTIN=y
CONFIG_LV_PROFILER_BUILTIN_BUF_SIZE=4096
CONFIG_LV_PROFILER_BUILTIN_DEFAULT_ENABLE=n
CONFIG_LV_PROFILER_EVENT=n
CONFIG_LV_PROFILER_TIMER=n
CONFIG_LV_PROFILER_INDEV=n
CONFIG_LV_PROFILER_LAYOUT=n
# the default include path (lvgl/debugging/...) is not on the component's include path; the header is under include/
CONFIG_LV_PROFILER_INCLUDE="include/lvgl/debugging/profiler/lv_profiler_builtin.h"
