# 聚会玩 / Party Play

设备平放在桌子中间，2–8 个人围着玩的五个离线小游戏：摇骰子、幸运转盘、抢拍、传炸弹、十秒挑战。一个 Tikpal 应用（Tikpal Voice 的扩展固件），用 Tikpal 开发包做的。

不联网、不开蓝牙、不录音、不碰 TF 卡。用户同意记录以后，玩够 3 局或 3 分钟的一场，离开时经 Tikpal 主固件的发件箱记一条 `party.session`。

1.0.7 起在这个仓库（开发包 1.9），以前在 Tikpal 的主仓库里（`yaykit/firmware/party-play`，到 1.0.7 为止）。

## 构建和测试

```bash
./kit build              # 编译，再跑主机测试
./kit install --slot 2   # USB 写进测试用 Tikpal Voice 的位置 2，写完直接打开
./kit logs               # 看日志；./kit snap 截图，./kit heap 看内存
./kit build --release    # 提交用的那份（不带开发命令）
./kit check              # 提交前的检查
```

- `devkit/` 是开发包（git 子模块），钉在 1.9，别改它。
- 主机测试（不要设备、不要 ESP-IDF）：`./kit test`，就是 `test/run.sh`；`bash test/run.sh wav` 另把每种音效写到 `test/out/` 试听（不提交）。主机测试连带编译 yaykit_motion 的核心（`devkit/firmware/components/yaykit_motion/yk_motion_core.c`）。
- 只编应用位置镜像（`yaykit-v1`，分区表是开发包的），带应用回滚。镜像约 1.7 MB。
- 量绘制时间的 profile 构建：`idf.py -B build-profile -D SDKCONFIG=build-profile/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;devkit/config/firmware.defaults;devkit/config/app.defaults;sdkconfig.profile" build`，打开 LVGL 自带的 profiler。设备上 `perf trace full 2` 或 `perf trace live 1500`，把 `PERF_TRACE_BEGIN` 到 `PERF_TRACE_END` 之间的输出存下来，用 `tools/trace_summary.py` 按函数汇总。发布版不带。
- `tools/heap_probe.py <口> <标签> <out.json>`：不重启设备，读几次 `heap` 和 `yk info`。

## 代码

游戏规则全是纯 C，主机测试覆盖；画面只读模型。

| 文件 | 内容 |
|---|---|
| `pp_play.c` | 整个游戏的状态：场景、朝向（中间的东西转向谁）、菜单、入座、五个游戏、设置、战绩、主持人什么时候出来、粒子、闲置和电量 |
| `pp_seats.c` | 座位、八色（按座位顺序明暗交替）、扇区 |
| `pp_dice.c` | 骰子物理（每秒 240 步）、翻面、落定；翻面少于 3 次的骰子落定时重新均匀抽一面 |
| `pp_wheel.c` | 公平的转盘：先抽人，再在他扇区中间 70% 挑停点，匀减速停在那里 |
| `pp_react.c` | 抢拍：随机等、假信号（第 3 轮起）、抢跑、同时、先到 3 分 |
| `pp_bomb.c` | 传炸弹：隐藏时长（点火 2 秒后每秒机会差不多，滴答慢时也会炸）、自己走的滴答钟、不动时催和加速、20 条小惩罚洗牌不重复 |
| `pp_ten.c` | 十秒挑战：淡出、评分、多人轮流 |
| `pp_record.c` | 返回结果（通用小结）、`party.session` 记录和精彩瞬间 |
| `pp_launch.c` | 启动参数 `game`、`players`、`dice` |
| `pp_gesture.c` | 轻触、沿边缘拨和甩、按朝向换算的四个方向滑 |
| `pp_motion_map.c` | yaykit_motion 的事件和状态翻成游戏的输入：骰子受的力、拿着、在动、拿的人方向 |
| `pp_synth.c` | 合成音效（模态声、滤波噪声、滑音、限幅），没有采样素材 |
| `pp_ui.c` | 画面：一个自绘舞台，文字逐字旋转转向那个人，骰子面预先画好再旋转缩放，只重画变化的区域；主持人是 yaykit_ui 的 `yk_yay` |
| `pp_sound.c`、`main.c` | 设备端：接到 yaykit_speak 混音的合成声部、启动顺序、姿态、触摸和侧键、设置、退出时的结果和记录、串口命令 |

## 用到的公共组件

| 组件 | 用法 |
|---|---|
| yaykit_app | `yk_app_start()` / `yk_app_ready()`；`yk_app_on_leave()`：双击侧键开应用环回去、`app exit` 时声音 10 ms 渐弱，`yk_app_set_result_summary()` 写一句话和局数、人数、分钟三个数字；值得记的一场离开时 `yk_app_record("party.session", …)`；亮度、息屏、音量、语言跟主固件的设置走；电量保护 |
| yaykit_board | 屏、触摸（抢拍每一轮 `board_touch_fast_start(5)`，按下时刻带 esp_timer 时间戳）、电量 |
| yaykit_motion | `yk_motion_start()`（224 Hz FIFO、共享标定）、`yk_motion_take()` 取事件、`yk_motion_state()`、`yk_motion_holder(500)` |
| yaykit_speak | `yk_speak_init_cfg()` 24 kHz、5 ms 一块，一个 `yk_sfx_play_synth()` 声部渲染整个 `pp_synth` 的混音；游戏里 `yk_speak_hold_amp(true)` |
| yaykit_ui | `yk_yay` 主持人（透明底，自己会眨眼，`yk_yay_rotate()` 转向那个人）、第一次进来的提示箭头 `yk_hint`、`snap` |
| yaykit_voicecode | 系统下拉里「在手机上打开」亮的应用码（`app.json` 的 `voiceCode`，1 号） |
| yaykit_link、yaykit_key | 读位置、侧键；不开蓝牙 |

## 控制台

- `party status`、`party go menu|seats|dice|wheel|react|bomb|ten|settings|scores|charge`
- `party tap <x> <y>`、`party swipe l|r|u|d`、`party rim <度> [角速度]`、`party key`、`party lift`（不用传感器也能掷）
- `party players <n>`、`party dice <n>`、`party lang zh|en`、`party quick on|off`（炸弹 1–13 秒）、`party battery <0-2> [pct]`、`party face down|up`、`party result`
- `party react false <座位> <毫秒>`、`party react bypart on|off`
- `sfx <编号> [力度]`、`perf [reset]`、`perf light`、`snap`、`heap`、`app status|exit|recovery`；组件的 `motion …`、`speak …`
- `yk info`、`yk cmd <名字> [json]`：和 yaykit_link 一样的格式，只走 USB，`./kit info` 能用；`device.id` 照 yaykit_link 的算法从 MAC 算出来（和主固件显示的一样）
- 开发版（`CONFIG_YAY_DEV`）：`yay tap|double|hold|launch party-play '{"game":"wheel","players":5}'`

日志前缀：`YAYKIT_PARTY_PLAY_READY`、`PARTY_SCENE`、`PARTY_SAY`、`PARTY_RESULT`、`PARTY_RECORD`、`PARTY_LAUNCH`、`PARTY_GO_SHOWN`、`PARTY_REACT_JUDGED`、`PARTY_FALSE_START`、`PERF`、`PERF_LIGHT`、`PERF_SLOW`。

## 1.0.7

版本说明（给用户看的）：

- 中文：传炸弹更刺激了：什么时候炸和滴答快慢不再有关系，滴答还很慢的时候也可能炸。拿到就快传！
- English: Hot Bomb is more thrilling: when it goes off no longer follows the ticking, so it can blow even while it still ticks slowly. Pass it on fast!

改了什么：

- 传炸弹：隐藏时长从「15–40 秒均匀」改成「2 秒 + 均值 22 秒的指数分布，截在 50 秒」；滴答和引线走自己的钟，25 秒从 0.62 秒一下加快到 0.12 秒一下，从不看隐藏时长。快速模式 1–13 秒、均值 5 秒。
- 搬到这个仓库、换成开发包 1.9（主固件 0.1.14 起，`minYay` 0.1.14、`apiLevel` 4），重编带上的行为变化：
  - 从顶边往下拉是系统下拉（Wi-Fi、电量、手机，「在手机上打开」亮 1 号应用码）；
  - 应用环只放应用；
  - 音效的音量档跟主固件 0.1.14 的表走。
- 设置换到自己的命名空间 `yk_partyplay`：从 1.0.6 升上来，人数、骰子数、小惩罚开关、语言、十秒的模式会回到默认一次。
