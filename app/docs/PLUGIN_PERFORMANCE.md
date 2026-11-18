# Headroom plugin performance: several instances in one DAW

Goal: 3–4 Headroom instances (AU in Logic Pro, or VST3) running beside a song and other processing, with no
dropouts. This document covers how the engine's parallel render pool behaves inside a DAW, what changed to make
it DAW-safe, and what was measured. See [PLUGIN.md](PLUGIN.md) for the plugin itself.

## The pieces

- **One engine per instance.** Each AU/VST3 instance owns a `RealtimeAudioServer` in hosted mode. Instances
  share nothing on the render path except read-only caches and the render pool.
- **RtPool** (`app/src/core/RtPool.hpp`) is a process-wide fork/join pool of real-time worker threads
  (`hardware_concurrency − 1`, Mach time-constraint policy). `RackGraph` forks a split's second lane to it;
  `CircuitUnit` forks the right channel of a stereo input. Forks nest (a lane containing a stereo circuit).
  Four plugin instances in one DAW share one pool.
- **Circuit units** are on by default and oversample 2–4×, so a circuit amp is the dominant cost of a rig.

## What changed (DAW safety)

### 1. Join reclaims unstarted work; it never runs other instances' work

Before, `join()` *helped*: while its own tasks were unfinished it popped and ran any queued task, including
other instances' tasks. With four instances on four host threads, one instance's render thread could end up
running another instance's lane, so one track paid for another and its block time depended on the others.

Now every queued task carries a claim word (in its ring cell, tagged with the ring position so a reused cell
can't be confused with an old one). Exactly one party wins the claim: a worker that dequeues it, or the forking
thread when it reaches `join()`. So:

- If no worker has started a task by the time the caller finishes its own part, the caller runs it inline.
  The worst case is serial rendering plus the cost of a queue push. That is the "serial when parallelism
  doesn't pay" fallback, and it adapts per fork: tiny buffers, workers asleep, or the host saturating the
  cores all end in the caller reclaiming the work.
- A caller only ever runs its own tasks. A worker that dequeues a task its joiner already reclaimed drops it.
- No deadlock with nesting: a task is either unstarted (its joiner reclaims it) or running on a thread that
  makes progress.

### 2. Bounded, adaptive spinning

- Workers start with no spin budget. They only spin right after running a task, because audio work arrives in
  bursts. With no parallel work they sleep on an atomic wait.
- Each worker's spin budget doubles when a spin finds work (up to 50 µs) and halves when it doesn't. Below
  2 µs the worker stops spinning. A wake-up raises the budget to at least 15 µs.
- At most two workers spin at once. The others sleep.
- `fork()` wakes a sleeper only when the queue holds more tasks than there are spinning workers, so most forks
  cost no system call. The sleep/fork handshake is a Dekker-style pair of seq_cst fences, so no wake-up is lost.
- `join()` spins for 10 µs on work still running on a worker, then sleeps. The worker wakes it only if it is
  actually asleep.

### 3. Backing off when the host is saturating the cores

A credit counter goes up when a worker runs a task and down (by 2) when a joiner has to reclaim one. Below
−16, `fork()` refuses 7 of every 8 calls, so the caller runs the task inline without waking anyone, and lets
the 8th through as a probe. Workers that aren't being scheduled therefore stop being woken just to compete
with the host's own threads, and parallelism returns once the probes start succeeding. Inline and worker
execution produce identical output, so backing off never changes the sound.

### 4. Host audio workgroup

- **AU (Logic):** `HeadroomAU` implements `kAudioUnitProperty_RenderContextObserver`. The OS fetches the block
  once and calls it on the render thread before the first render in a new context. The block hands
  `context->workgroup` to `PluginCore::setRenderWorkgroup`, which forwards it to the pool.
  - That call is real-time safe: one `os_retain`, an atomic exchange, a counter bump and a wake-up.
  - Each worker notices the change before its next task, leaves its old workgroup and joins the new one with
    `os_workgroup_join`. Every worker joins: a pool worker is effectively an auxiliary render thread of
    whichever instance forked the task.
  - Workers taking tasks are capped at `os_workgroup_max_parallel_threads(wg) − 1`, leaving one thread for
    the host's own render thread. Workers above the cap stay parked.
  - With several instances, each holds a slot in the pool (32 slots), and workers follow the first occupied
    slot. In Logic all instances report the same workgroup.
  - Old workgroups are released on a control thread (`prepare`, `suspend`, instance teardown). A hazard
    counter makes sure no worker is between reading a slot and retaining it.
  - Verified: hr_host renders from threads joined to an `AudioWorkIntervalCreate` workgroup, and the OS
    called our observer by itself. All 11 workers joined, and `max_parallel_threads` reported 8, so 7
    workers take tasks.
- **VST3:** the VST3 SDK (3.7.x, in `third_party/vst3sdk`) has no render-context or workgroup API. On
  `setActive(true)` in realtime mode, the VST3 offers the workers the **system default output device's IO
  workgroup** (`kAudioDevicePropertyIOThreadOSWorkgroup`). On macOS 11+ hosts run `process()` in that
  workgroup when they use the default device. A host on another device only means the workers are accounted
  to a different deadline. `HEADROOM_VST3_WORKGROUP=0` turns this off.

### 5. Lifetime: the plugin binary can be unloaded

Workers were detached threads that lived for the whole process. In a plugin, the host may unload the binary
(VST3 module unload, plugin rescans), so a detached thread would then be running unmapped code. Now:

- `PluginCore` calls `retainUsers()` when it is constructed and `releaseUsers()` when it is destroyed. The last
  release stops and joins the workers, and the next instance starts them again.
- The static pool's destructor also joins them.
- The app never releases the pool, so its workers live for the process as before.
- `test_plugin_core`'s "all engine threads joined" check now passes. Before, it failed with 13 threads against
  a baseline of 1.

### Knobs

- `HEADROOM_RTPOOL_WORKERS=N` sets the worker count (0 = always serial), read once at start-up.
- `RtPool::setEnabled(false)` turns pooling off at run time (tests, null references).

## Null test (output unchanged)

`test_plugin_core` runs four instances of a stereo split rig ("Funk Rhythm Snap": 1176 circuit, split with
the Twin Reverb circuit and a cab, so forks nest three deep) with different block partitions per instance. It
compares the pool's output against the serial reference:

| comparison against serial (pool off, one instance after another) | max abs difference |
|---|---|
| same again (rig determinism) | 0 |
| **pool on, one instance after another** | **0 (bit-identical)** |
| pool off, 4 instances rendering at once on 4 threads | 0 in 6 of 9 runs; 5.36e-7 (−125 dBFS) in 3 |
| pool on, 4 instances at once | same as the row above |

The pool itself is bit-exact. The intermittent few-ulp difference also shows up **with the pool off**, whenever
instances render at the same moment. It was traced to the cabinet convolution (a cab-only rig reproduces it;
the circuit units alone don't). It is below the project's −120 dBFS null bar and is not caused by this work.
When it appears it is always exactly the same value, 5.36e-7, which points to an alternative code path
(possibly Accelerate's vDSP choosing a different kernel under contention) rather than a data race. That is
unconfirmed. The test asserts bit-exactness for the pool and < 1e-6 for concurrent instances.

## Measurements

### Method

`hr_host` (`app/plugin/tests/hr_host.mm`) is the test host. `--bundle` loads a built `Headroom.component` into
the process under a private component description, so nothing is installed. `--perf` runs the deadline test:

```
app/build-plugin/hr_host --bundle app/build-plugin/Headroom.component --perf \
    [--rigs A,B] [--n 1,2,3,4] [--buf 64,128,256,512] [--host par|ser|both] [--secs S] \
    [--load K] [--rtload K:F] [--nowg] [--rigc file.rig[:in_ch]] [--dump out.f32] [--trace out.txt]
```

- **Pacing.** Real time at 48 kHz. Every cycle (one buffer period, paced with `mach_wait_until`) each instance
  renders one block through `AudioUnitRender`. Each configuration runs 1 s unmeasured, then 3 s measured.
- **`host=par`** models a DAW's process threads with one track per instance (Logic's playback tracks). It
  uses one Mach time-constraint host thread per instance, all joined to a host workgroup made with
  `AudioWorkIntervalCreate`. A cycle master starts and finishes the workgroup interval.
- **`host=ser`** renders every instance in turn on one real-time thread, like instances in series on one
  channel strip.
- **Reported, as % of the buffer deadline:**
  - per-instance render time, mean and worst;
  - total: the sum of the instance means;
  - cycle time, from the cycle's scheduled start until every instance (and any real-time load) is done,
    p99 and worst;
  - missed deadlines.
- **Load:**
  - `--load K`: K default-priority threads burning cores;
  - `--rtload K:F`: K more real-time threads in the host workgroup, each burning F of every cycle, like other
    tracks' plugins.
- **Rigs:**
  - **A**: "00 JCM800 British Crunch": TS808 and JCM800 2203 circuits, matched cab, plate. Mono guitar in,
    stereo out.
  - **B**: "Funk Rhythm Snap": 1176 circuit, then a split with the Twin Reverb circuit and a cab in one lane
    and an amp in the other. Stereo in, so forks nest three deep.
  - "Comfortably Numb Solo" was tried first but renders at about 7× real time, because the Hiwatt DR103
    circuit falls into the Newton fallback.
- **A/B.** The "old" numbers are the same engine snapshot, built with the previous `RtPool` (helping join,
  fixed 2000/4000-iteration spins, no workgroup). The same `hr_host` ran old and new back to back.
- **Machine.** M4 Pro, 12 cores (8 performance, 4 efficiency), 24 GB. The machine was busy throughout
  (load average 13–24, including a running Headroom.app at about 120 % CPU, a circuit test and ngspice), so
  every number here is pessimistic compared with a quiet machine.

### Rig A (circuit amp chain, mono in): parallel host, old → **new**

Rig A has no parallel sections (mono circuits, no split), so the pool is idle and the two builds are
equivalent. The differences are run-to-run noise.

| buffer (deadline) | instances | per-instance mean | per-instance worst | total, all instances | worst cycle | p99 cycle | missed deadlines |
|---|---|---|---|---|---|---|---|
| 64 (1.33 ms) | 1 | 42% → **45%** | 62% → **65%** | 42% → **45%** | 63% → **65%** | 57% → **59%** | 0 → **0** of 2250 |
| 64 (1.33 ms) | 2 | 41% → **43%** | 63% → **63%** | 82% → **86%** | 64% → **64%** | 56% → **59%** | 0 → **0** of 2250 |
| 64 (1.33 ms) | 3 | 41% → **44%** | 61% → **65%** | 122% → **132%** | 62% → **66%** | 57% → **60%** | 0 → **0** of 2250 |
| 64 (1.33 ms) | 4 | 43% → **44%** | 106% → **65%** | 171% → **177%** | 108% → **66%** | 61% → **62%** | 1 → **0** of 2250 |
| 128 (2.67 ms) | 1 | 41% → **43%** | 56% → **58%** | 41% → **43%** | 57% → **58%** | 52% → **55%** | 0 → **0** of 1125 |
| 128 (2.67 ms) | 2 | 41% → **39%** | 59% → **55%** | 82% → **79%** | 59% → **55%** | 55% → **52%** | 0 → **0** of 1125 |
| 128 (2.67 ms) | 3 | 42% → **41%** | 58% → **56%** | 127% → **122%** | 59% → **57%** | 57% → **54%** | 0 → **0** of 1125 |
| 128 (2.67 ms) | 4 | 42% → **42%** | 61% → **58%** | 166% → **167%** | 62% → **59%** | 58% → **56%** | 0 → **0** of 1125 |
| 256 (5.33 ms) | 1 | 43% → **41%** | 54% → **53%** | 43% → **41%** | 54% → **53%** | 53% → **51%** | 0 → **0** of 562 |
| 256 (5.33 ms) | 2 | 43% → **40%** | 56% → **56%** | 86% → **79%** | 56% → **56%** | 54% → **51%** | 0 → **0** of 562 |
| 256 (5.33 ms) | 3 | 41% → **42%** | 55% → **55%** | 123% → **127%** | 55% → **56%** | 52% → **54%** | 0 → **0** of 562 |
| 256 (5.33 ms) | 4 | 41% → **41%** | 61% → **54%** | 166% → **165%** | 61% → **54%** | 55% → **52%** | 0 → **0** of 562 |
| 512 (10.67 ms) | 1 | 41% → **40%** | 52% → **50%** | 41% → **40%** | 52% → **50%** | 50% → **48%** | 0 → **0** of 281 |
| 512 (10.67 ms) | 2 | 40% → **41%** | 52% → **52%** | 80% → **82%** | 52% → **52%** | 51% → **51%** | 0 → **0** of 281 |
| 512 (10.67 ms) | 3 | 41% → **43%** | 51% → **55%** | 124% → **130%** | 52% → **55%** | 51% → **54%** | 0 → **0** of 281 |
| 512 (10.67 ms) | 4 | 41% → **41%** | 55% → **54%** | 166% → **164%** | 55% → **54%** | 54% → **53%** | 0 → **0** of 281 |

**Four instances fit at every buffer size, including 64 samples. The worst cycle is at most 66 % of the
deadline, and no deadline was missed.** Each instance costs about 40–45 % of one core.

### Rig B (stereo split rig, nested parallelism): parallel host, old → **new**

| buffer (deadline) | instances | per-instance mean | per-instance worst | total, all instances | worst cycle | p99 cycle | missed deadlines |
|---|---|---|---|---|---|---|---|
| 64 (1.33 ms) | 1 | 63% → **59%** | 184% → **186%** | 63% → **59%** | 184% → **186%** | 99% → **95%** | 21 → **15** of 2250 |
| 64 (1.33 ms) | 2 | 65% → **60%** | 193% → **186%** | 130% → **120%** | 194% → **187%** | 128% → **102%** | 157 → **27** of 2250 |
| 64 (1.33 ms) | 3 | 72% → **64%** | 204% → **221%** | 216% → **192%** | 206% → **222%** | 147% → **120%** | 561 → **101** of 2250 |
| 64 (1.33 ms) | 4 | 85% → **77%** | 231% → **207%** | 338% → **307%** | 233% → **209%** | 182% → **164%** | 1221 → **616** of 2250 |
| 128 (2.67 ms) | 1 | 61% → **55%** | 128% → **115%** | 61% → **55%** | 128% → **115%** | 95% → **85%** | 8 → **2** of 1125 |
| 128 (2.67 ms) | 2 | 61% → **53%** | 166% → **110%** | 122% → **107%** | 166% → **110%** | 117% → **89%** | 44 → **2** of 1125 |
| 128 (2.67 ms) | 3 | 68% → **63%** | 177% → **135%** | 205% → **190%** | 178% → **136%** | 132% → **113%** | 152 → **30** of 1125 |
| 128 (2.67 ms) | 4 | 77% → **71%** | 213% → **188%** | 309% → **285%** | 214% → **188%** | 165% → **144%** | 400 → **102** of 1125 |
| 256 (5.33 ms) | 1 | 63% → **60%** | 118% → **112%** | 63% → **60%** | 119% → **113%** | 98% → **93%** | 4 → **2** of 562 |
| 256 (5.33 ms) | 2 | 64% → **60%** | 125% → **116%** | 129% → **120%** | 126% → **116%** | 106% → **100%** | 7 → **4** of 562 |
| 256 (5.33 ms) | 3 | 67% → **63%** | 129% → **125%** | 202% → **189%** | 129% → **126%** | 114% → **106%** | 20 → **14** of 562 |
| 256 (5.33 ms) | 4 | 79% → **70%** | 159% → **148%** | 314% → **279%** | 159% → **149%** | 128% → **123%** | 78 → **47** of 562 |
| 512 (10.67 ms) | 1 | 62% → **57%** | 107% → **99%** | 62% → **57%** | 107% → **99%** | 98% → **88%** | 2 → **0** of 281 |
| 512 (10.67 ms) | 2 | 59% → **58%** | 101% → **100%** | 118% → **116%** | 101% → **100%** | 96% → **92%** | 2 → **0** of 281 |
| 512 (10.67 ms) | 3 | 74% → **68%** | 134% → **124%** | 221% → **204%** | 135% → **124%** | 122% → **118%** | 22 → **13** of 281 |
| 512 (10.67 ms) | 4 | 79% → **75%** | 139% → **134%** | 314% → **302%** | 139% → **134%** | 136% → **128%** | 51 → **37** of 281 |

For reference, rig B with the pool off (`HEADROOM_RTPOOL_WORKERS=0`) at 128 samples, one instance, averages
**117 %** of the deadline and misses 965 of 1125 cycles. The pool is what makes this rig run in real time at
all.

With 2–4 instances at 64–128 samples, the new pool cuts misses by 2–6×, and from 44 to 2 for 2 instances at
128. It also lowers the mean and worst cycle in nearly every row. Misses remain with 3–4 instances,
and they are not a scheduling problem:

- One instance alone already peaks at 115 % of a 128-sample deadline.
- `--trace` shows the peaks come in bursts of 10–15 consecutive blocks (30–40 ms) at the riff's pick attacks.
  The Twin Reverb circuit needs more Newton iterations there: the median block takes 1.54 ms, the bursts
  2.3–3.7 ms.
- Getting 4 rig-B instances clean at 128 samples needs a cheaper worst case in the circuit engine, which was
  reported to its owner. It is not something the pool can schedule away.

### Serial host (instances in series on one thread): old → **new**

Rig A:

| buffer (deadline) | instances | per-instance mean | per-instance worst | total, all instances | worst cycle | p99 cycle | missed deadlines |
|---|---|---|---|---|---|---|---|
| 128 (2.67 ms) | 1 | 40% → **41%** | 56% → **57%** | 40% → **41%** | 56% → **57%** | 51% → **51%** | 0 → **0** of 1125 |
| 128 (2.67 ms) | 2 | 40% → **40%** | 54% → **63%** | 79% → **79%** | 101% → **111%** | 97% → **101%** | 4 → **13** of 1125 |
| 512 (10.67 ms) | 1 | 41% → **40%** | 50% → **49%** | 41% → **40%** | 50% → **49%** | 48% → **48%** | 0 → **0** of 281 |
| 512 (10.67 ms) | 2 | 39% → **39%** | 50% → **50%** | 78% → **78%** | 95% → **96%** | 94% → **95%** | 0 → **0** of 281 |

Rig B:

| buffer (deadline) | instances | per-instance mean | per-instance worst | total, all instances | worst cycle | p99 cycle | missed deadlines |
|---|---|---|---|---|---|---|---|
| 128 (2.67 ms) | 1 | 56% → **55%** | 126% → **123%** | 56% → **55%** | 126% → **123%** | 88% → **85%** | 3 → **2** of 1125 |
| 128 (2.67 ms) | 2 | 56% → **54%** | 126% → **125%** | 111% → **108%** | 195% → **196%** | 158% → **161%** | 939 → **874** of 1125 |
| 512 (10.67 ms) | 1 | 57% → **56%** | 100% → **104%** | 57% → **56%** | 100% → **104%** | 90% → **85%** | 1 → **1** of 281 |
| 512 (10.67 ms) | 2 | 57% → **56%** | 93% → **103%** | 113% → **113%** | 166% → **169%** | 162% → **164%** | 245 → **239** of 281 |

On one thread the instances' times add up, so about two rig-A instances or one rig-B instance fit. In Logic,
instances on separate tracks render in parallel (table above). Rig A has no forks, so its 4 → 13 at 2×128 is
noise at the edge of the deadline (the mean total is 79 %).

### Under load: 4 instances at 128 samples, parallel host, old → **new**

| load | rig | worst cycle | p99 cycle | missed deadlines | pool (new) |
|---|---|---|---|---|---|
| 8 burner threads + 4 real-time threads at 30 % of every cycle | A | 83 % → **75 %** | 79 % → **67 %** | 0 → **0** of 2250 | no forks |
| same | B | 236 % → **228 %** | 165 % → **154 %** | 1203 → **766** of 2250 | 31036 forks: 21135 on workers, 9901 reclaimed inline |
| 12 burner threads (every core) | A | 57 % → **59 %** | 53 % → **53 %** | 0 → **0** of 2250 | no forks |
| same | B | 216 % → **180 %** | 165 % → **153 %** | 439 → **116** of 2250 | 31036 forks: 29992 on workers, 1044 reclaimed inline |

- **Rig A: four instances under synthetic load, no deadline misses.** That is 6 s of audio per run, with 8
  default-priority burners plus 4 real-time threads in the host workgroup, or with 12 burners.
- **Rig B: misses fall by 36–74 % compared with the old pool.** They are still caused by the rig's own
  per-block peaks.
  - Under real-time load, a third of rig B's forks were reclaimed and run inline: the workers couldn't be
    scheduled in time, and the caller didn't wait for them.
  - The backoff never triggered, because more than two thirds of forks still paid off.

### What this means for Logic

- **Rig A,** a circuit amp chain with cab and reverb, is the typical guitar chain: about 40–45 % of one core per
  instance. Four instances run at any buffer size, beside other real-time load.
- Logic renders playback tracks in parallel on its process threads, often with a larger process buffer.
  Only the live (record-enabled, selected) track runs at the I/O buffer size. "4 instances at 128 samples,
  parallel host" is therefore the hard case.
- **Rig B** costs about 55–60 % of one core per instance, plus about 2× peaks on pick attacks. Two instances
  are clean at 128 samples, and four need 256 samples or more on a loaded machine. Its worst case belongs to
  the circuit engine.

## Series-unit pipelining (2026-11-10)

A rack's series units used to render one after another on one core, so a chain of five or six units cost the sum of their parts on that core. Parallel lanes and stereo circuit channels already forked to RtPool. `RackGraph` now also renders each run of consecutive series units, in the top-level chain and inside each split lane, as a wavefront over sub-blocks:
- Unit A renders sub-block 1.
- Unit B renders sub-block 1 on another core while A renders sub-block 2.
- And so on, within the same `process` call.
This adds no latency, and the output is bit-identical to rendering the units one after another: every unit sees the same samples in the same order, and only the call boundaries move.

**Mechanism**
- **Stages.** Up to 3 contiguous stages along a run's series chain, cut to minimise the largest stage by each unit's measured cost (a smoothed per-block time; any cut is bit-identical, so this only balances the cores). A run through a split adds up to 3 stages for lane B (2026-11-11, below), so one run uses at most 6.
  - Sub-blocks are 16 to 64 frames (a quarter of the block); `HEADROOM_PIPE_SUB` overrides the size.
  - The caller renders the first stage. The stages are ordered so that each waits only on earlier ones, and forked last-first, so `RtPool::join` reclaims an unstarted stage before an unstarted one that waits on it.
  - A stage waits for its input by spinning briefly, then sleeping on the producer's frame counter (a sleeper count, so several stages may wait on one counter).
- **Which units.** `IComponent::subBlockSafety()` and `monoPreserving()` decide.
  - Units with a mono and a stereo path (circuit units, the drive pedals) take the path the rack decides for the whole host block (`AudioBlock::mono_input`, 2026-11-11 below), so they sub-block whatever their input.
  - The legacy waveshaper reads its smoothers' end-of-call values on channel 1, so it is never sub-blocked.
  - NAM, MIDI units, hosted plugins and containers are never sub-blocked.
  - Pipelining is skipped when the block carries MIDI, when the host transport is playing, and in offline calibration renders.
- **Readouts.** Readouts stay as they were:
  - Unit meters accumulate per sub-block in sample order and commit once per block (`UnitMeterBank::accumulate` / `commit`).
  - Dynamics gain reduction and the circuit units' LFO and gain-reduction lamps update once per host block, over the whole block (`AudioBlock::host_offset` / `host_frames`).
- **Workgroup.** `AudioIO` joins RtPool's workers to the output device's IO-thread workgroup (`kAudioDevicePropertyIOThreadOSWorkgroup`). The standalone app never did. Without it, a stage on an efficiency core made the render about 2x slower than one core in about half of the harness runs.
- **Off switch.** `HEADROOM_PIPE=0`, or `RackGraph::setPipelining(false)`, renders sequentially (the reference for tests).

**Convolution made deterministic.** The partitioned convolver (cabinets, IR reverbs) shared one static vDSP FFT setup. Two convolvers on two cores (two cabs in parallel lanes, or a cab and a reverb in pipelined stages) gave different results from sample 1024 on, at about -127 dB, so today's lane parallelism was already not bit-identical.
- Accelerate's 2048-point `vDSP_fft_zrip` also varies with what other threads or processes are doing, even with its own setup and temp buffers (20 of 20 runs under concurrent same-size FFTs).
- The convolver now runs entirely on the portable `rig::vdsp` subset: plain C++, the same packing and scale, and its own setup per object. Its output no longer depends on the core or on concurrent work.
- The cab's output changes against before only at rounding level. Its cost stays small (11 µs per 256-frame block per cab).

**Proof**
- Byte-identical null, sequential against pipelined, at 256 and 128 frames:
  - all 84 deterministic shipped rigs (of 101; the other 17 are Showcase instrument rigs whose two sequential renders already differ);
  - the user rig Clean '61 Strat Stereo.
- rig_tests, test_graph_runtime, test_effect_handoffs, test_param_drag, test_circuit_unit, test_circuit_engine and test_aliasing pass.
- Harness: `pipebench` (scratch; `null`, `time`, `units` invariance, `procload`).

**Result on Clean '61 Strat Stereo.** The rig has two parallel lanes, each 1176 → TS9DX → RE-201 → amp → cab → 480L. At 256 frames on a loaded machine, with a real-time caller and the pool in an audio work interval:
- sequential: mean 82 % of the 5.33 ms budget (238 % in one run during load spikes), p99 5.0 ms, max 6.1-6.6 ms;
- pipelined: mean 56-65 %, p99 3.6-4.2 ms, max 4.7-7.4 ms.

### Through stereo units and splits (2026-11-11)

The real-time audit (`work/circuit-netlists/PRESET_REALTIME.md`, "Rigs that drop out only because of chain or lane
structure") found that a pipelined run stopped at every unit that made the signal stereo, or might, and at every split.
Eleven rigs lost their real-time margin that way. Three changes remove those cuts, and each keeps the output byte-identical
to sequential rendering.

**1. Units declare channel-identical behaviour (`IComponent::monoPreserving`).** Only the circuit units declared it before.
Every unit type was audited, by reading its render code and by measuring it: identical channels in, then a check that
the output channels were identical in each block where the unit said so.

| Declares it now | Why |
|---|---|
| Noise Gate, Limiter and the other dynamics units | one linked detector, one gain for both channels |
| EQ Graphic, EQ Parametric, EQ Shelving (studio EQ), the generic EQ / filter units, Tone Match | the same filters on the same state per channel |
| Amp-Matched Cab, Classic Amp-Matched Cab, Studio Mic Room, Cab Room, IR Loader, the Cabinet unit | whenever the mixed IR is the same on both sides (`PartitionedIR::sameAs`; the Amp-Matched Cabs always, the others with Stereo / Width off, the Cabinet also with its doubler and room off) |
| IR reverbs | only with a mono impulse |
| Legacy amps | unless their built-in chorus or spring tank is on |
| Drive pedals, Transpose, Trance Gate, NAM, Pitch to MIDI, MIDI FX, routing and pass-through units, plugin placeholders | the same arithmetic on both channels, or one rendered channel copied, or a copy |
| Looper | while empty, stopped or recording (a playing loop may hold a stereo take) |
| Circuit units | the mono path copies left to right; **no longer** a stereo-output circuit (`io.outputR`: the JC-120, the Leslie, stereo choruses), which makes two sides |

Delays, modulation, the FDN reverbs, the doubler, instruments, the legacy waveshaper and hosted plugins keep the default
(no). The waveshaper looked like a candidate once its smoothers settle, but the per-block check found differing channels.
A wrong "no" only costs CPU: a circuit unit after it renders stereo.

**2. Mono or stereo is decided once per host block (`AudioBlock::mono_input`).** A circuit unit (and a drive pedal)
used to compare its input channels on every call, so in a pipeline it could only be sub-blocked while its input was
proven mono. A unit in the middle of a pipelined run can't see its whole block's input when its first sub-block renders,
so the decision now comes from what is known before anything renders. `RackGraph` checks whether the rack's input block
is one signal on both channels, then carries that through the chain: past a unit only if it declares `monoPreserving`;
into both lanes of a split; out of a split's mix only if both lanes are. Each unit gets `mono_input` = 1 (render mono) or
0 (render stereo) for the whole host block, in sequential and pipelined rendering alike, so both take the same path.
- The traits are read before the unit renders, in both modes.
- Units called outside the rack (`mono_input` = -1) still compare the channels per call.
- **What changes in sound:** the decision equals the old per-call check except in two cases.
  - **After an undeclared unit that outputs identical channels.** A circuit unit after it now renders stereo for the whole block, where the old check rendered mono whenever the samples happened to match. Examples are a silent or centred instrument, or a reverb fed silence. The old check flipped between the two paths with the right channel's circuit state left stale; now it stays stereo, with decorrelated noise, at twice that unit's CPU.
  - **Right after the rack's input turns from stereo to mono.** A declared unit's two channels can still hold different tails, such as a cabinet's convolution. The circuit unit after it then renders mono, from the left channel, through that tail.
  - The old-engine comparison below lists the rigs where either happened.

**3. A run continues through a split.** A top-level run may now hold one enabled split whose lanes contain only
pipelinable units.
- **Stages.** The series chain is cut into up to 3 stages: the units before the split, a *tap* (the split's input), lane A's units, the mix, then the units after it. Lane B either renders inside the stage that mixes or gets up to 3 stages of its own. Those stages read the tap and hand their output to the mix sub-block by sub-block, so up to 6 stages run at once.
- **Ordering.** The stages are ordered so each waits only on earlier ones: the chain up to the tap's stage, lane B, then the rest. Running them in that order on one thread therefore always completes, and the inline and reclaim paths are deadlock-free.
- **Balance.** The cut minimises the largest stage. On a tie it uses fewer stages, then the more even plan, so the caller's own first stage isn't near-empty.
- **When it doesn't cross.** If the units beside the split carry under 10 % of the run's measured cost, the split renders as before: two lanes in parallel, each with its own pipeline. Coupling the lanes at every sub-block for the mix only cost time there; Clean '61 Strat Stereo measured 55 % → 62 % at 256 frames before this rule was added.
- **Shared arithmetic.** The Split Mix arithmetic is one function (`mixSplit`) for whole blocks and sub-blocks.
- **Unchanged.** Splits inside lanes and container children render as before.

**CrossOver splits are frequency splits now.** The CrossOver (50000 / 51000 / 52000) used to mix exactly like a parallel
split. Its "f" did nothing.
- **The split.** At the split's input there is now a Linkwitz-Riley 4th-order band split: two cascaded 2nd-order Butterworth sections per band, bilinear with prewarping, double-precision state. The low band feeds lane A ("Pan Low") and the high band lane B ("Pan High"). The bands are in phase, and their sum is an allpass.
- **"f".** It maps to `RackGraph::crossoverHz(f)` = 20 Hz × 1000^f (20 Hz to 20 kHz, log; 0.5 = 632 Hz, 0.3 = 159 Hz). It glides about 30 ms, one step per host block.
- **Invert.** The CrossOver Mix reads Invert as a switch whose untouched 0.5 is off, so the bands sum flat by default. A parallel split still inverts lane B at 0.5 and up, unchanged.
- **Real-time behaviour.** The filters are allocation-free, updated before the block renders, and run in sample order. In a pipeline they run at the tap, so they are byte-identical pipelined.
- **Rigs affected.** None today. Library / Guitar + MIDI / **Synth Body** was the only rig with a CrossOver, at f = 0.3 (159 Hz). It was rebuilt on 2026-11-11 on a parallel split with an EQ Shelving, so no shipped or user rig uses the CrossOver now.
- **Tests (`rig_tests`, "CrossOver split").**
  - With both lanes empty, low plus high is flat within 0.01 dB from 25 Hz to 15 kHz and at fc, for f = 0.3, 0.5 and 0.75.
  - Each lane alone is -6.02 dB (±0.05) at `crossoverHz(f)`, flat (±0.3 dB) at 3x away and below -44 dB at 4x into the stop band.
  - A parallel split with Invert off mixes exactly 2x the input, bit for bit.
  - A crossover rig with an EQ before the split and an EQ in lane B renders byte-identically pipelined and sequential, at 64 and 256 frames.

**Proof.** The scratch harness `pipenull` ran with `HEADROOM_NOISE_FIXED_SEED=1` on an isolated copy of the data folder.
It renders the app's render path: meters armed, /api/diag attached, the pool live.
- **Byte-identical null, pipelined against sequential.**
  - **Rigs:** every rig, 128 in all: 31 Showcase, 80 Library and 17 user presets.
  - **Signal:** 1 s each of the DI playlist at 64, 128 and 256 frames, then 0.5 s each with a stereo input (right = left delayed and scaled), then 0.5 s mono again. That exercises both decisions and the stereo-to-mono turn.
  - **Result:** 128 of 128 rigs are byte-identical, every block at every size.
  - 111 rigs were compared in one process against two sequential renders. 110 of them pipelined, and 9 crossed a split.
  - The other 17 are Showcase instrument rigs whose two sequential renders differ inside one process: the live pitch-to-MIDI state is process-wide. They were rendered one per process (sequential, sequential, pipelined), and all 17 are identical; 7 of them pipeline.
- **Old engine against new, both sequential.** The same tree was built without and with this work, and every rig was rendered once with each.
  - **90 of 128 are identical.** The other 38 differ only in the two ways described under 2:
    - 24 rigs have an undeclared stereo unit (delay, flanger, widener, reverb, Micro-Shift, instrument) ahead of a circuit unit, and its channels matched at start-up, in silence, or with a centred note. This group covers the SSL-EQ user rigs, Pink Floyd, Lead, Best Clean, the Guitar + MIDI hybrids and the tremolo and chorus Showcase rigs.
    - 11 rigs differ only after the stereo-to-mono turn.
    - 3 rigs differ only in their first segment (start-up).
  - No difference is caused by pipelining.
- **Suites.**
  - rig_tests passes, including the new CrossOver test.
  - test_rack_snapshots, test_graph_runtime, test_effect_handoffs and test_param_drag (CTest, with rtguard) pass.
  - test_plugin_core with `HEADROOM_NOISE_FIXED_SEED=1` passes. Its render-pool null on a stereo split rig is 0 against serial in every mode.

**Result** (`app/tools/preset_rt`, the app's render path on a real-time caller in an audio work interval, 10 s per size;
before and after are the same tree without and with this work, built side by side, run interleaved rig by rig; the
machine was heavily loaded, 1-minute load average 72–92 on 12 cores, so compare columns, not absolute
values). Mean / p99 / max % of the deadline / missed deadlines:

| Rig | 64 frames: before → **after** | 128 frames | 256 frames | load avg (before / after) |
|---|---|---|---|---|
| Best Clean | 169 / 207 / 279 / 7500 → **110 / 134 / 162 / 6540** | 167 / 214 / 229 / 3750 → **114 / 172 / 189 / 3179** | 164 / 184 / 198 / 1875 → **116 / 161 / 181 / 1693** | 74 / 71 |
| Stockholm Chainsaw | 120 / 168 / 227 / 6096 → **95 / 137 / 152 / 3011** | 123 / 164 / 183 / 3324 → **95 / 128 / 146 / 1447** | 119 / 150 / 164 / 1625 → **93 / 120 / 130 / 591** | 74 / 79 |
| Pink Floyd | 123 / 156 / 203 / 7487 → **105 / 144 / 210 / 4562** | 120 / 148 / 163 / 3744 → **100 / 130 / 171 / 1707** | 120 / 138 / 144 / 1873 → **93 / 111 / 122 / 341** | 79 / 77 |
| Lead | 125 / 162 / 217 / 7462 → **96 / 129 / 178 / 2579** | 122 / 147 / 171 / 3749 → **97 / 121 / 137 / 1237** | 119 / 139 / 147 / 1873 → **92 / 107 / 114 / 233** | 72 / 75 |
| Clean | 94 / 121 / 169 / 1119 → **71 / 95 / 151 / 60** | 92 / 115 / 163 / 174 → **66 / 88 / 132 / 17** | 95 / 119 / 166 / 199 → **62 / 81 / 122 / 6** | 75 / 73 |
| Clean - Gretsch | 91 / 115 / 161 / 455 → **63 / 86 / 144 / 31** | 88 / 111 / 152 / 74 → **61 / 84 / 127 / 13** | 90 / 110 / 153 / 51 → **61 / 84 / 129 / 6** | 82 / 77 |
| Clean - Les Paul | 91 / 115 / 164 / 405 → **61 / 84 / 137 / 26** | 89 / 115 / 153 / 82 → **60 / 80 / 121 / 11** | 88 / 111 / 149 / 32 → **58 / 79 / 118 / 6** | 79 / 75 |
| Clean '61 Strat | 89 / 104 / 159 / 122 → **60 / 71 / 124 / 12** | 86 / 95 / 143 / 31 → **58 / 68 / 109 / 4** | 86 / 96 / 133 / 17 → **57 / 64 / 106 / 1** | 78 / 72 |
| Gretsch Lead | 100 / 123 / 131 / 3413 → **72 / 81 / 104 / 2** | 96 / 121 / 134 / 510 → **70 / 76 / 89 / 0** | 97 / 104 / 113 / 388 → **67 / 75 / 89 / 0** | 77 / 81 |
| Lead - Tele | 103 / 117 / 133 / 5161 → **70 / 78 / 97 / 0** | 102 / 114 / 136 / 2674 → **68 / 76 / 93 / 0** | 102 / 110 / 124 / 1435 → **67 / 76 / 83 / 0** | 87 / 92 |
| Envelope Funk Quack | 93 / 137 / 210 / 1271 → **68 / 109 / 169 / 109** | 94 / 135 / 178 / 704 → **67 / 102 / 143 / 40** | 91 / 126 / 166 / 157 → **66 / 98 / 130 / 17** | 85 / 80 |
| Sunset Strip Lead | 116 / 139 / 149 / 6915 → **100 / 116 / 123 / 4564** | 112 / 128 / 132 / 3057 → **100 / 114 / 122 / 2429** | 110 / 124 / 128 / 1567 → **100 / 113 / 118 / 1184** | 77 / 79 |
| Pride and Joy Rhythm | 97 / 108 / 115 / 2829 → **68 / 76 / 82 / 0** | 97 / 106 / 113 / 937 → **63 / 68 / 72 / 0** | 93 / 103 / 109 / 101 → **62 / 65 / 67 / 0** | 82 / 78 |
| Hysteria Bass | 150 / 193 / 237 / 7500 → **126 / 167 / 191 / 7114** | 148 / 180 / 193 / 3750 → **124 / 154 / 176 / 3663** | 147 / 173 / 183 / 1875 → **127 / 166 / 177 / 1795** | 78 / 84 |
| Clean '61 Strat Stereo | 63 / 92 / 137 / 45 → **64 / 96 / 141 / 60** | 61 / 88 / 129 / 17 → **59 / 88 / 130 / 18** | 61 / 91 / 121 / 10 → **61 / 89 / 127 / 10** | 75 / 73 |
| Comfortably Numb Solo | 91 / 135 / 167 / 2939 → **89 / 137 / 211 / 2907** | 89 / 127 / 142 / 1318 → **88 / 129 / 144 / 1400** | 86 / 117 / 124 / 506 → **86 / 119 / 128 / 553** | 76 / 72 |
| Funk Rhythm Snap | 63 / 118 / 218 / 181 → **53 / 102 / 214 / 88** | 61 / 117 / 205 / 79 → **51 / 100 / 170 / 38** | 61 / 108 / 177 / 33 → **52 / 93 / 159 / 16** | 74 / 72 |

- **Every rig the audit named got faster.** Mean load fell by:
  - 25–35 points for the six SSL-EQ user rigs, Pride and Joy Rhythm and Envelope Funk Quack, which now sit at 57–72 %;
  - 18–30 points for Stockholm Chainsaw, Lead and Pink Floyd;
  - about 50 points for Best Clean.
- **Deadline misses fell from hundreds or thousands to a handful.**
  - Lead - Tele misses none at any size.
  - Gretsch Lead and Pride and Joy Rhythm miss none at 128 and 256 frames. At 64 frames Gretsch Lead misses 2 and Pride and Joy Rhythm none.
  - Clean, Clean - Gretsch, Clean - Les Paul, Clean '61 Strat and Envelope Funk Quack miss 1 to 109 per run. Those are amp bursts on a machine at load 70–90.
- **Still over the deadline:** Best Clean (110–116 %), Hysteria Bass (124–127 %), Sunset Strip Lead (100 %), Pink Floyd and Lead (92–105 %), and Stockholm Chainsaw (93–95 %).
  - Each has one unit that takes most of a core. In Best Clean it is the AC30 rendering stereo after the delay. The others are the SVT, the Hot-Rod Plexi, the SLO-100 and the HM-2.
  - With 4 sub-blocks, a 3-stage pipeline's critical path is (4 + 2)/4 = 1.5 times its largest stage: the fill and drain the audit describes.
  - The next levers are finer sub-blocks (`HEADROOM_PIPE_SUB`) and cheaper units.
- **Split rigs.** Funk Rhythm Snap, with a 1176 before its split, dropped from 61–63 % to 51–53 %, and Hysteria Bass from 147–150 % to 124–127 %. In Clean '61 Strat Stereo and Comfortably Numb Solo, the units beside the split carry under 10 % of the work, so those splits render as before; both measured the same within noise.

## Tests

- **`test_plugin_core`** (CTest `plugin_core`): all checks pass. It now covers:
  - the render-pool null test and the workgroup join/leave;
  - every pool worker joined when the last instance is destroyed. This check failed before (13 threads
    against a baseline of 1).
- **`hr_host --bundle app/build-plugin/Headroom.component`**: all checks pass. `hr_host` without `--bundle`
  still tests the installed AU.
- **SPICE gate (`HEADROOM_NGSPICE` unset):**
  - `test_circuit_engine` doesn't link `RtPool`. Its failures in this tree come from circuit work that was
    in progress at the time (Dimension D, op-amp rails, Klon).
  - `test_circuit_unit` uses the pool. Its only failure is a stale embedded copy of `ehx-small-stone`
    (a concurrent JSON edit).
  - The pool's own guarantee, bit-exact output against serial, is the null test above.
- **`test_param_drag` with rtguard preloaded:** PASS, with 0 heap calls and 0 locks on the audio thread.
