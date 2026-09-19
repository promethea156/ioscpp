# Captures

Raw USB captures behind the reset in [`../04-blockers.md`](../04-blockers.md). The two runs
are the same device (iOS 18.7.8, `<device-serial>`) on the same host and the same
`USBPcap1` root hub, and differ only in the host TLS stack.

- `failing-run.pcapng` — an `ioscpp_usb_example` run, with the device on `libusb0` and
  `usbmuxd`/Apple services stopped. The device reset after the ClientHello, which was the
  host certificate's zero-length serial.
- `reference-run.pcapng` — a `pymobiledevice3 lockdown info` run, with the device on Apple's
  driver and *Apple Mobile Device Service* running. The device answers with a ServerHello.
- `failing-clienthello.bin` — the ClientHello from `failing-run.pcapng`, as written by
  `IOSCPP_DUMP`.
- `reference-clienthello.bin` — the ClientHello from `reference-run.pcapng`.
- `reference-serverhello.bin` — the device's ServerHello from `reference-run.pcapng`.

Decode the ClientHellos and compare them:

```powershell
python tools/compare-clienthello.py parse docs/captures/reference-clienthello.bin
python tools/compare-clienthello.py diff docs/captures/reference-clienthello.bin docs/captures/failing-clienthello.bin
```

Find the frames in the capture:

```powershell
tshark -r docs/captures/reference-run.pcapng -Y 'usb.src == "host" && usb.data_len > 200'
tshark -r docs/captures/reference-run.pcapng -Y 'usb.src == "device" && usb.data_len > 40'
```

The reference run needs Apple's stack; the steps are in
[`../09-platform-setup.md`](../09-platform-setup.md), under *Reaching a reference through
Apple Mobile Device Support*.
