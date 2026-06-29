# M4F Baseline Firmware Template

Working M4F firmware showing recommended patterns for AM62 bramka development.

## What this template demonstrates

- **Async RPMsg communication** with Linux via callback pattern (no blocking recv)
- **Autonomous 1Hz tick** showing M4F doing work independently of Linux
- **Command parsing** (`ping`, `status`, generic echo)
- **Graceful shutdown handler** - REQUIRED for hot-reload workflow without reboot
- **Production-quality structure** with separated concerns and clear comments

## How to use this template

### Option A: New project from scratch

1. In CCS Theia, create new project from SDK example `ipc_rpmsg_echo`
2. Select board: `AM62X-SK`, core: `m4fss0-0`, runtime: `noRTOS`
3. Replace the generated `ipc_rpmsg_echo.c` with this file
4. Configure SysConfig (see below)
5. Build (Ctrl+Shift+B)

### Option B: Modify existing project

1. Backup your current `ipc_rpmsg_echo.c` (or whichever file has `main` function)
2. Copy this file content over
3. Adjust application logic in:
   - `handleLinuxMessage()` - command handling
   - `doPeriodicTick()` - your periodic work
   - Add new functions for sensors, control loops, etc.

## Required SysConfig configuration

In `example.syscfg`:

### IPC -> RPMessage with Linux
- Enable: **yes**
- Endpoint: `14` (matches `LINUX_CHRDEV_ENDPOINT` in code)
- Service name: `rpmsg_chrdev`
- Buffer size: `512` bytes
- Number of buffers: `256`

### TI Driver Porting Layer -> Debug Log
Enable these sinks:
- Error Zone ✓
- Warning Zone ✓
- **Info Zone ✓** (REQUIRED - without this, DebugP_log won't appear in trace0)
- Remote Core Trace ✓
- CCS Log ✓ (optional, for JTAG debug)
- UART Log ✓ (optional, MCU_USART0 @ 115200)

## Deploy workflow

After building:

```powershell
# From PowerShell with profile loaded (Deploy-M4F function):
Deploy-M4F
```

Or manually:
```bash
# From bramka SSH session:
m4f-reload [path_to_firmware.out]
```

## Verifying it works

After first deploy, check `/sys/kernel/debug/remoteproc/remoteproc0/trace0` should show:

```
[m4f0-0] 0.004631s : [M4F] Application starting...
[m4f0-0] 0.004868s : [M4F] Waiting for Linux ready...
[m4f0-0] 0.011548s : [M4F] Linux ready!
[m4f0-0] 0.011721s : [M4F] Endpoint 'rpmsg_chrdev' announced on EP 14
[m4f0-0] 0.014700s : [M4F] Shutdown handler registered    <-- key line
[m4f0-0] 0.017733s : [M4F] Ready for messages
[m4f0-0] 1.020837s : [M4F] Tick #1
```

The **"Shutdown handler registered"** line confirms our handler is in place.

Then test commands from Linux:
```bash
rpmsg_char_simple -r 9 -n 3
# Output should include "M4F got: hello there 0!" etc.

# Test ping command (requires consumer process for response):
cat /dev/rpmsg0 &        # background reader
echo -n "ping" > /dev/rpmsg0
# Should see "pong from M4F (tick=N)" in the cat output
```

## Critical test: hot reload without reboot

After flashing this firmware once, try running `Deploy-M4F` 5 times in a row.
**Every deploy should succeed**, no reboot needed between them.

If second deploy fails with `M4F won't stop`, the shutdown handler isn't
working correctly. Check `docs/M4F_SHUTDOWN.md` for diagnostic steps.

## Architecture notes

### Why callback pattern instead of blocking RPMessage_recv

This template uses `RPMessage_construct(...recvCallback...)` with a callback
function. Messages arrive asynchronously and main loop polls a flag.

Alternative pattern (used in SDK example) is blocking `RPMessage_recv` in a
dedicated FreeRTOS task. That requires FreeRTOS overhead. For noRTOS, callbacks
are simpler and more efficient.

### Why M4F sends ticks only to trace, not RPMsg by default

The commented `RPMessage_send` in `doPeriodicTick()` would push tick messages
to Linux every second. Currently commented because:

1. Without a Linux consumer reading `/dev/rpmsg0`, send may block or drop
2. For development, trace0 logs are easier to inspect

When you have a Go service running that consumes the rpmsg channel, uncomment
that block to test true server-push from M4F.

### Why uint8_t for gbShutdownRemotecoreID

To match official SDK example exactly. The cast `(uint8_t)remoteCoreId` in
the callback narrows from uint16_t but core IDs fit in 8 bits (max value
~7 for AM62X family).

### Why DebugP_log uses \r\n not \n

TI's DebugP works with both UART and trace0 sinks. UART terminals expect
CR+LF for proper line break. Just `\n` may render as continuation on same line.

## Extending this template

Common additions:

### Add a sensor sampling task

```c
static void sampleSensors(void)
{
    static uint64_t lastSample = 0;
    uint64_t now = ClockP_getTimeUsec();
    
    /* Sample every 100 ms */
    if ((now - lastSample) >= 100000ULL) {
        lastSample = now;
        /* Read I2C/SPI/ADC sensors here */
        /* Update sensor state */
    }
}
```

Then in main loop:
```c
while (!gbShutdown) {
    if (gRxBuffer.pending) handleLinuxMessage();
    doPeriodicTick();
    sampleSensors();          /* <-- added */
    ClockP_usleep(1000);
}
```

### Add control loop (e.g. PID)

```c
static void runControlLoop(void)
{
    static uint64_t lastIter = 0;
    uint64_t now = ClockP_getTimeUsec();
    
    /* Run control loop at fixed 1 kHz */
    if ((now - lastIter) >= 1000ULL) {
        lastIter = now;
        /* PID iteration here */
        /* Update PWM outputs */
    }
}
```

### Add asynchronous notification to Linux

```c
/* Triggered by sensor reading, alarm, etc. */
static void sendAlarmToLinux(const char *alarm)
{
    int len = strlen(alarm) + 1;
    RPMessage_send(alarm, len,
                   LINUX_CORE_ID, LINUX_CHRDEV_ENDPOINT,
                   RPMessage_getLocalEndPt(&gRecvMsgObject),
                   100);  /* 100 ms timeout */
}
```

Note: requires Linux side to be reading `/dev/rpmsg0` (e.g. via Go service)
otherwise message will be buffered and may block sender.
