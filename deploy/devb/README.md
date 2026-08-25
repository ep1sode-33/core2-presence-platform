# devb deployment

The presence API is installed at `/opt/m5-presence`, keeps its SQLite database
in `/var/lib/m5-presence`, and listens only on `192.168.0.46:8081` plus
`100.117.242.46:8081`. The existing environmental API on port 8080 is not part
of this unit.

From an unpacked repository checkout on a first deployment:

```sh
cd /opt/m5-presence
make backend-install PYTHON=python3
install -m 0600 /dev/null /etc/m5-presence.token
openssl rand -out /etc/m5-presence.token -hex 32
install -m 0644 deploy/devb/m5-presence-api.service \
  /etc/systemd/system/m5-presence-api.service
systemd-analyze verify /etc/systemd/system/m5-presence-api.service
systemctl daemon-reload
systemctl enable --now m5-presence-api.service
```

Do not regenerate `/etc/m5-presence.token` during an ordinary code update; it
must remain synchronized with the M5GO credential. Rotation is a deliberate
separate operation. The service fails closed if the credential is absent or
empty.

Verification:

```sh
systemctl --no-pager --full status m5-presence-api.service
ss -ltnp 'sport = :8081'
curl --fail http://192.168.0.46:8081/v1/healthz
curl --fail http://100.117.242.46:8081/v1/healthz
```

All endpoints below `/v1/devices/...` require the bearer token; only
`/v1/healthz` is intentionally public on the two private interfaces.
