# M5GO recovery runbook

This runbook keeps USB serial as the recovery boundary for every v0.7 failure.
Wireless update paths write only an application slot; they never rewrite the
bootloader, partition table, NVS settings, or LittleFS.

## Before recovery

1. Connect the M5GO directly over trusted USB.
2. Confirm the target is `/dev/cu.usbserial-588D0027491` and that the ESP32 MAC
   is `2c:bc:bb:81:eb:60` before writing.
3. Save the current serial log and note the reset reason, running build ID, OTA
   phase, and last error from the diagnostics page when available.
4. Do not erase the entire flash merely to repair Wi-Fi, an API URL, or one bad
   application image. Full erase also destroys NVS credentials and durable
   LittleFS telemetry.

## Invalid Wi-Fi, backend URL, or device token

Re-run USB provisioning. It atomically writes an inactive settings slot and
switches ownership only after every field verifies:

```sh
python3 tools/provision_core2.py --ssid "Wi-Fi network"
```

The tool reuses the existing per-device development-OTA secret. Add
`--rotate-ota-secret` only when deliberately revoking the old OTA credential.
After the reported restart, hold B for diagnostics and verify the LAN IP,
backend result, and clock status.

## Isolated telemetry socket failure

Do not erase flash or reprovision a device whose settings still validate. Once
the API is reachable again, the uploader discards any incomplete telemetry
connection, uses bounded backoff, opens a clean dedicated keep-alive session,
and resumes the exact immutable LittleFS envelopes without disturbing its
separate control/health/log session. Confirm new telemetry acknowledgements and
a falling spool count before attempting any stronger recovery.

## Backend restart or path outage

A server restart can invalidate both persistent sessions. Each request still
uses bounded backoff and retries its immutable work; repeated bounded-operation
transport failures may close both sockets and recycle the station connection.
Once the API path recovers, confirm both control polling and new telemetry
acknowledgements before attempting flash erasure or reprovisioning.

## Broken development or production OTA client

Build and install the ordinary serial environment. This is also the bootstrap
path for the first OTA-capable v0.7 image:

```sh
PLATFORMIO_CORE_DIR="$PWD/.platformio" ./.venv/bin/pio run -e m5go-serial
PLATFORMIO_CORE_DIR="$PWD/.platformio" ./.venv/bin/pio run \
  -e m5go-serial --target upload
```

Serial recovery is intentionally outside the wireless release-counter rule.
After boot, provision again only if diagnostics says settings are missing or
invalid; an application-only upload normally leaves NVS and LittleFS intact.

## Two unusable application slots or a boot loop

1. Keep the device on USB and capture the boot log at 115200 baud.
2. Use `m5go-serial` to reinstall the last known-good application, bootloader,
   partition table, and OTA selector produced by the repository build.
3. Do **not** run a whole-flash erase first. If the serial loader cannot connect,
   hold the hardware reset/boot controls to enter the ROM downloader and retry.
4. If a full erase becomes unavoidable, first make a complete 16 MB flash
   backup and record its SHA-256. Treat the operation as destructive: it removes
   Wi-Fi/API/OTA credentials, configuration, and queued telemetry.
5. Re-provision over USB, then verify diagnostics locally before selecting any
   production release again.

The delayed OTA validator confirms a pending image only after 30 seconds of
healthy local main-loop, worker, storage, filesystem, and display operation.
An early hard failure requests bootloader rollback. Temporary Wi-Fi, devb,
weather, or optional-sensor loss is not a rollback gate.

An interrupted sequential update can leave the inactive slot partly erased or
written, but it does not change the current boot selection. The next production
OTA starts again at offset zero and incrementally overwrites that inactive slot.

## Bootloader or partition-table changes

These changes are never distributed over OTA. Review the new offsets against
`default_16MB.csv`, make a full flash backup, and install only by trusted USB.
A partition-layout change can make existing NVS, LittleFS, and core-dump data
unreadable even without a chip erase, so export any needed telemetry first.

## Development OTA window

Development OTA is closed by default. Open diagnostics with a 1.5-second B
hold, then hold A+C together for 3 seconds. The screen must show a 120-second
countdown before `m5go-ota` can connect. Use the per-device secret stored by the
USB provisioning tool; never place it directly in shell history. A remote
command may ask for this window, but cannot bypass the physical A+C gesture.

A successfully validated development image does not claim that the previously
confirmed production release is still running. Returning to the signed
production channel requires a verified release with a strictly higher release
counter; reinstalling the old counter wirelessly remains rejected as a
downgrade. USB serial recovery remains available independently of that wireless
counter rule.

## Production signing-key recovery

The device accepts at most the compiled current and next P-256 public keys. The
private key stays outside the repository and devb. A lost or compromised key is
recovered by installing, over USB or through a still-trusted signed image, a
firmware build whose compiled trust set contains the replacement public key.
Never accept a public key supplied inside an OTA manifest as a new trust root.

## Post-recovery checks

- diagnostics works with devb offline;
- brightness still follows `PRESENT=255`, `COOLDOWN=60`, `IDLE=0`;
- the firmware build ID matches the retained ELF and release manifest;
- health and telemetry receive authenticated acknowledgements;
- upload automatically reconnects and drains after a devb service restart;
- no persistent `errno 11`, HTTPClient `-3`/`-11`, or watchdog reset appears;
- queued transition/feedback records drain without critical drops; and
- development OTA is unreachable after its countdown closes.
