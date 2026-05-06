---
name: pi-syslog
description: Pull / tail the ratgdo syslog from the Pi. Use when verifying firmware behavior post-flash, investigating crashes, or running soak checks.
---

# Pi syslog access

ratgdo forwards syslog over UDP 5140 to the Pi at `100.121.96.114`. Logs persist at:
- `/var/log/ratgdo.log` (current day)
- `/var/log/ratgdo.log.1` (yesterday)
- `/var/log/ratgdo.log.2.gz` and earlier (compressed archive)

## Common queries

### Live tail
```bash
ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'tail -f /var/log/ratgdo.log'
```

### Last N lines
```bash
ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'tail -200 /var/log/ratgdo.log'
```

### Search across last 24h
```bash
ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'cat /var/log/ratgdo.log /var/log/ratgdo.log.1 | grep -E "<pattern>"'
```

### Reboot / crash analysis
```bash
ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'grep -E "Start \(setup\)|Crash reason|IllegalInstruction|panic" /var/log/ratgdo.log /var/log/ratgdo.log.1'
```

### Heap trend
```bash
ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'grep -E "heap=|free heap" /var/log/ratgdo.log | tail -50'
```

## Soak check pattern (overnight health)

After a deploy, schedule a check at +2.5h, +5h, +24h:
1. Reboot count since boot — should be 1 (the OTA reboot itself). >1 = panic loop.
2. Heap trend — should be stable, not monotonically decreasing.
3. SSE subscriber count — should stabilize, not grow unbounded.
4. WiFi RSSI — should be steady, not flapping.
5. HomeKit pair count — should match expected controllers.
