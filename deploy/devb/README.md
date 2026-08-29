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

Before the first restart onto database schema v3, install the backup unit and
run one successful backup while the old API is still serving traffic. Store
that one-off snapshot in the separate
`/var/lib/m5-presence-backup/pre-schema-v3` directory so normal rotation cannot
prune it. Keep it until the new service, console, and continuing device ingest
have all been verified. An old application checkout cannot open the schema-v3
database; a rollback across this boundary therefore restores the pre-migration
snapshot as well as the old code.

Verification:

```sh
systemctl --no-pager --full status m5-presence-api.service
ss -ltnp 'sport = :8081'
curl --fail http://192.168.0.46:8081/v1/healthz
curl --fail http://100.117.242.46:8081/v1/healthz
```

All device-data and console-data endpoints require the bearer token.
`/v1/healthz`, the static `/console` shell and assets, and FastAPI's API schema
and documentation are public on the two private interfaces; none contains a
credential or device telemetry.

## SQLite backups

`backup_presence_db.py` creates a transactionally consistent snapshot with
Python's SQLite online backup API. This is safe while the presence API is
writing in WAL mode: the script opens the source read-only, asks SQLite to
assemble the snapshot, runs full `PRAGMA integrity_check` and
`PRAGMA foreign_key_check` against the result, fsyncs it, and only then
atomically renames it into place. Every error exits nonzero; a snapshot or
integrity-check failure never publishes its temporary database.

Do **not** replace this with `cp /var/lib/m5-presence/presence.db ...` while the
API is running. A plain copy can omit committed data that is still in the WAL
sidecar or produce an inconsistent snapshot.

Install and enable the daily timer:

```sh
install -m 0644 deploy/devb/m5-presence-backup.service \
  /etc/systemd/system/m5-presence-backup.service
install -m 0644 deploy/devb/m5-presence-backup.timer \
  /etc/systemd/system/m5-presence-backup.timer
systemd-analyze verify \
  /etc/systemd/system/m5-presence-backup.service \
  /etc/systemd/system/m5-presence-backup.timer
systemctl daemon-reload
systemctl enable --now m5-presence-backup.timer
```

Before the schema-v3 API restart, create the non-rotating migration snapshot:

```sh
install -d -m 0700 /var/lib/m5-presence-backup/pre-schema-v3
python3 /opt/m5-presence/deploy/devb/backup_presence_db.py \
  --backup-dir /var/lib/m5-presence-backup/pre-schema-v3 \
  --daily-retention 1 --weekly-retention 0
```

Use the exact snapshot path printed by the command for these two gates. The
schema query must print `2`; the backup command itself must already have printed
both integrity checks as `ok`.

```sh
python3 -c 'import sqlite3,sys; c=sqlite3.connect(f"file:{sys.argv[1]}?mode=ro&immutable=1", uri=True); print(c.execute("PRAGMA user_version").fetchone()[0])' \
  /var/lib/m5-presence-backup/pre-schema-v3/presence-daily-YYYYMMDDTHHMMSSZ.sqlite3
sha256sum \
  /var/lib/m5-presence-backup/pre-schema-v3/presence-daily-YYYYMMDDTHHMMSSZ.sqlite3
```

The timer runs daily around 03:15 local time, with up to 15 minutes of jitter.
Timestamps in backup filenames are UTC. By default, Sunday runs are classified
as weekly snapshots; other runs are daily snapshots. The newest 14 daily and 8
weekly files are retained independently in
`/var/lib/m5-presence-backup`. Rotation only touches files matching
`presence-(daily|weekly)-YYYYMMDDTHHMMSSZ.sqlite3`.

This is an independent root-managed state directory. The API's dynamic service
user can write `/var/lib/m5-presence` but cannot replace or delete the sibling
backup directory.

Retention and the weekly day can be overridden with a systemd drop-in:

```ini
[Service]
Environment="PRESENCE_BACKUP_DAILY_RETENTION=21"
Environment="PRESENCE_BACKUP_WEEKLY_RETENTION=12"
Environment="PRESENCE_BACKUP_WEEKDAY=sun"
```

Both retention values are counts. Daily retention must be at least 1. Setting
weekly retention to 0 disables the weekly class, so every run is retained as a
daily snapshot. The backup also refuses to start unless the destination has
space for one current database plus a 1 GiB free-space reserve. Override the
reserve with `PRESENCE_BACKUP_MIN_FREE_BYTES` if capacity planning calls for a
different value. After changing a drop-in, run `systemctl daemon-reload`.

Run and inspect one backup before relying on the timer:

```sh
systemctl start m5-presence-backup.service
systemctl --no-pager --full status m5-presence-backup.service
journalctl -u m5-presence-backup.service -n 20 --no-pager
systemctl list-timers m5-presence-backup.timer --no-pager
ls -lh /var/lib/m5-presence-backup
```

The oneshot has no network access and can write only below
`/var/lib/m5-presence-backup`. It runs as root so it can read the database owned
by the API's dynamic systemd user; backup files and directories are restricted
to mode `0600` and `0700`, respectively.

The standalone backup tests exercise a live WAL database, integrity-check
and foreign-key failure, capacity reserve, abandoned-temp cleanup, atomic
publication, retention, and CLI failure status:

```sh
python3 -m unittest discover -s deploy/devb/tests -v
```

These generations remain on devb's disk. They protect against a bad migration
or accidental database replacement, but not against loss of that disk; an
off-host copy still needs a separately chosen destination.

## Schema-v3 rollback

Rollback across the schema boundary is a database restore, not just a code
checkout. Stop the API first, quarantine the v3 database and both possible WAL
sidecars, install the exact validated v2 snapshot through a same-directory
temporary file, preserve the old database ownership, then switch code and
restart:

```sh
systemctl stop m5-presence-api.service
install -d -m 0700 /var/lib/m5-presence/schema-v3-quarantine
mv -- /var/lib/m5-presence/presence.db \
  /var/lib/m5-presence/schema-v3-quarantine/presence.db
if test -e /var/lib/m5-presence/presence.db-wal; then
  mv -- /var/lib/m5-presence/presence.db-wal \
    /var/lib/m5-presence/schema-v3-quarantine/presence.db-wal
fi
if test -e /var/lib/m5-presence/presence.db-shm; then
  mv -- /var/lib/m5-presence/presence.db-shm \
    /var/lib/m5-presence/schema-v3-quarantine/presence.db-shm
fi
install -m 0600 \
  /var/lib/m5-presence-backup/pre-schema-v3/presence-daily-YYYYMMDDTHHMMSSZ.sqlite3 \
  /var/lib/m5-presence/presence.db.restore
chown --reference=/var/lib/m5-presence/schema-v3-quarantine/presence.db \
  /var/lib/m5-presence/presence.db.restore
sync -f /var/lib/m5-presence/presence.db.restore
mv -- /var/lib/m5-presence/presence.db.restore \
  /var/lib/m5-presence/presence.db
cd /opt/m5-presence
git switch --detach PRE_UPGRADE_COMMIT
make backend-install PYTHON=python3
systemctl start m5-presence-api.service
```

Replace both uppercase placeholders with the exact recorded snapshot timestamp
and pre-upgrade commit. Keep the quarantined v3 files until the rollback has
been verified through health, authenticated reads, and new device ingest.
