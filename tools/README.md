# M5GO USB provisioning

`provision_core2.py` writes Wi-Fi and backend credentials over the M5GO USB
serial connection. It automatically looks for
`usbserial-588D0027491`; use `--port` to override that choice.

The script keeps its original filename for compatibility with existing setup
commands.

Install the only runtime dependency outside the repository, then run:

```sh
python3 -m pip install pyserial
python3 tools/provision_core2.py --ssid "Wi-Fi network"
```

The Wi-Fi password and API token are collected with hidden prompts. They are
never accepted as command-line arguments or printed. The backend defaults to
`http://192.168.0.46:8081`; override it with `--base-url` when needed.
Only `http://` URLs are accepted in this release because the firmware has no
certificate trust configuration yet. Use it only on the trusted LAN; bearer
authentication does not encrypt the token.

Provision only from a trusted computer through a trusted USB serial adapter.
The current challenge correlates one fresh transaction but does not authorize
it with a physical-button confirmation, so another local process with serial access is inside
this release's trust boundary.

For non-interactive use, `--secrets-stdin` accepts exactly two lines: the Wi-Fi
password followed by the API token. Pass `--ssid` in this mode. Feed stdin from
a trusted secret manager or another process; do not commit a secrets file or put
secret values in shell arguments.

Run the hardware-independent protocol tests with:

```sh
python3 -m unittest discover -s tools/tests -v
```
