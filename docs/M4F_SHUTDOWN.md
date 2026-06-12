# M4F Graceful Shutdown Handler - REQUIRED for development workflow

## Why this matters

Linux remoteproc framework uses a handshake protocol to stop a remote core
(M4F in our case). When you run:

```bash
echo stop > /sys/class/remoteproc/remoteproc0/state
```

Linux:
1. Sends a mailbox notification (`IPC_NOTIFY_RP_MBOX_SHUTDOWN`) to M4F
2. Waits up to 10 seconds for M4F to send `IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK`
3. If no ACK arrives, returns `-EBUSY` (errno 16) and refuses to stop

**Without a shutdown handler in M4F firmware, every firmware deploy requires
a full system reboot.** This makes development painful and breaks OTA updates
in production.

## The bug we hit (and how we found the fix)

We initially tried the obvious order in our shutdown sequence:

```c
// WRONG ORDER - ACK never reaches Linux
Drivers_close();
IpcNotify_sendMsg(coreId, IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                  IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK, 1u);
System_deinit();
__asm__ __volatile__ ("wfi" : : : "memory");
```

Result: M4F received shutdown notification, sent ACK, but Linux timed out
anyway. Mysterious - logs showed `Shutdown ACK sent to Linux` followed by
`notify_shutdown_rproc: timeout waiting for rproc completion event`.

The fix was found by reading the official SDK example
`ipc_rpmsg_echo_linux.c`. The correct order is:

```c
// CORRECT ORDER - matches SDK example
Drivers_close();
System_deinit();           // <-- BEFORE sending ACK
IpcNotify_sendMsg(coreId, IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                  IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK, 1u);
__asm__ __volatile__ ("wfi" : : : "memory");
```

Hypothesis: `System_deinit()` resets internal SDK structures including
interrupt vectors and mailbox FIFO state. Sending ACK BEFORE deinit puts
the message into a buffer that subsequent cleanup might discard or route
incorrectly. Sending AFTER deinit ensures the message goes through the
"production" channel that Linux remoteproc is listening on.

This is undocumented but empirically verified.

## Required implementation in M4F firmware

### 1. Includes

```c
#include <drivers/ipc_notify.h>
#include <drivers/ipc_rpmsg.h>
#include <kernel/dpl/SystemP.h>
```

### 2. Shutdown state globals

```c
static volatile uint8_t gbShutdown = 0u;
static volatile uint8_t gbShutdownRemotecoreID = 0u;
```

### 3. Shutdown callback

Called by SDK when Linux sends shutdown notification.
**Keep it short** - it runs in interrupt context.

```c
static void ipc_rp_mbox_callback(uint16_t remoteCoreId, uint16_t clientId,
                                  uint32_t msgValue, void *args)
{
    if (clientId == IPC_NOTIFY_CLIENT_ID_RP_MBOX) {
        if (msgValue == IPC_NOTIFY_RP_MBOX_SHUTDOWN) {
            gbShutdown = 1u;
            gbShutdownRemotecoreID = (uint8_t)remoteCoreId;
            /* If using blocking RPMessage_recv elsewhere, call
             * RPMessage_unblock(&yourEndpoint) here to wake those tasks.
             * For callback-based RPMsg (no blocking recv), no unblock needed.
             */
        }
    }
}
```

### 4. Register callback (after `RPMessage_announce`)

```c
int32_t status = IpcNotify_registerClient(IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                                           ipc_rp_mbox_callback,
                                           NULL);
DebugP_assert(status == SystemP_SUCCESS);
```

### 5. Modify main loop to exit on shutdown

```c
while (!gbShutdown) {
    /* your existing logic */
    ClockP_usleep(1000);
}

doShutdown();  /* never returns */
```

### 6. Shutdown sequence

**CRITICAL: order matters - System_deinit BEFORE sending ACK.**

```c
static void doShutdown(void)
{
    DebugP_log("[M4F] Shutdown requested by core %u\r\n",
                (unsigned int)gbShutdownRemotecoreID);
    DebugP_log("[M4F] Closing drivers, deinit system, sending ACK\r\n");
    
    Drivers_close();
    System_deinit();      /* MUST be before ACK send! */
    
    if (gbShutdownRemotecoreID != 0) {
        IpcNotify_sendMsg(gbShutdownRemotecoreID,
                          IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                          IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK,
                          1u);
    }
    
    /* Halt CPU - wait for interrupt forever (none ever comes) */
    while (1) {
        __asm__ __volatile__ ("wfi" "\n\t": : : "memory");
    }
}
```

## Reference implementation

See `examples/m4f-baseline/ipc_rpmsg_echo.c` for a complete working firmware
with shutdown handler implemented correctly.

## Verifying shutdown handler works

After flashing firmware with shutdown handler, test by running deploy twice
without reboot:

```powershell
Deploy-M4F       # first deploy works (M4F was offline)
Deploy-M4F       # second deploy should ALSO work
Deploy-M4F       # and a third, fourth, etc.
```

If second deploy fails with `M4F won't stop (state: running)`, the firmware
lacks shutdown handler. Check the trace0 log:

```bash
cat /sys/kernel/debug/remoteproc/remoteproc0/trace0 | tail -10
```

You should see entries like:
```
[M4F] Shutdown requested by core 0
[M4F] Closing drivers, deinit system, sending ACK
```

If you see those but Linux still times out, check shutdown order - ACK must
come AFTER System_deinit, not before.

If you don't see those at all, the callback isn't being called - check
that `IpcNotify_registerClient` returned `SystemP_SUCCESS`.

## What NOT to do

### DON'T use unbind/bind as workaround

Tempting because it forces M4F to release without shutdown ACK:

```bash
# DON'T DO THIS
echo 5000000.m4fss > /sys/bus/platform/drivers/k3-m4-rproc/unbind
echo 5000000.m4fss > /sys/bus/platform/drivers/k3-m4-rproc/bind
```

**Problem**: Linux remoteproc never reuses numbers. After unbind/bind,
M4F moves from `remoteproc0` to `remoteproc4` (or higher). All scripts
hardcoding `remoteproc0` break, and the only way to restore is reboot.

### DON'T skip Drivers_close() / System_deinit()

If you skip these and just send ACK + WFI, the IPC infrastructure on M4F
isn't properly torn down. Next firmware load may inherit stale state and
crash mysteriously.
