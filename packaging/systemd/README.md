# Running taserver with systemd

These steps use the installed Linux package and built-in Let's Encrypt support.
You do **not** need to download or copy a service unit from the source repository.

1. **Choose a hostname and configure networking.** Point a hostname you control
   (for example, `ta.example.org`) at the server. Allow inbound TCP **80** for
   certificate validation and **7677** for the game, plus outbound HTTPS **443**.
   If you publish an IPv6 address, it must reach this server too. Another service
   must not occupy port 80 when taserver validates its certificate.

2. **Copy your retail game data.** Replace `/path/to/retail-install` below with
   the actual directory containing your game's archives and maps:

   ```sh
   sudo mkdir -p /srv/ta-data
   sudo cp -a /path/to/retail-install/. /srv/ta-data/
   sudo chown -R root:root /srv/ta-data
   sudo chmod -R a+rX /srv/ta-data
   ```

   The service cannot read `/home` because of its sandbox. Do not use a symlink
   back into your home directory. Clients need matching base game data.

3. **Install the ACME override template.** For a first-time setup:

   ```sh
   sudo mkdir -p /etc/systemd/system/taserver.service.d
   sudo cp -i /usr/share/doc/ta-engine/systemd/taserver-acme.conf \
     /etc/systemd/system/taserver.service.d/acme.conf
   ```

   If you already have `acme.conf`, keep it and edit it instead of overwriting
   your configuration. The packaged unit is already installed at
   `/usr/lib/systemd/system/taserver.service`.

4. **Set your hostname before starting.** Open the copied override:

   ```sh
   sudoedit /etc/systemd/system/taserver.service.d/acme.conf
   ```

   Replace **`YOUR.SERVER.NAME`** after `--acme-domain` with your actual hostname
   (for example, `ta.example.org`), without `https://` or a port. The data path
   defaults to `/srv/ta-data`; use the settings template below to change it.
   Keep the empty `LoadCredential=` and `ExecStart=` lines; they replace the default manual-certificate configuration.
   Using `--acme-agree-tos` accepts the CA subscriber agreement.

5. **Enable and start the service.**

   ```sh
   sudo systemctl daemon-reload
   sudo systemd-analyze verify taserver.service
   sudo systemctl enable --now taserver
   ```

   If it was already running, also run `sudo systemctl restart taserver` to
   apply your changes. Systemd creates the private state directory automatically.

6. **Check startup, then connect.**

   ```sh
   sudo systemctl status taserver --no-pager
   sudo journalctl -u taserver -f
   ```

   For first-time ACME setup, wait for `ACME: certificate installed` before connecting.
   A listening service can still be waiting for issuance or a persisted retry deadline.
   Players then enter just your hostname in the multiplayer connection screen.
   Press Ctrl+C to leave the log viewer; this does not stop the server.

**Optional: change server defaults.** Copy and edit the settings template:

```sh
sudo mkdir -p /etc/systemd/system/taserver.service.d
sudo cp -i /usr/share/doc/ta-engine/systemd/taserver-settings.conf \
  /etc/systemd/system/taserver.service.d/settings.conf
sudoedit /etc/systemd/system/taserver.service.d/settings.conf
sudo systemctl daemon-reload
sudo systemctl restart taserver
```

The template lists the default paths, port, game/account limits, storage budgets
and systemd resource limits. Change individual values such as
`Environment="TA_SERVER_MAX_RUNNING_GAMES=8"` without rewriting the startup
command. It works with both certificate modes. Restart between matches, since
it disconnects active games. For examples, see [changing server defaults](https://github.com/pocketgeek/ta-engine/blob/main/docs/public-server.md#changing-server-defaults).

The server renews its certificate automatically and opens port 80 only during
validation. Local configuration stays in `/etc/systemd/system/taserver.service.d/`;
accounts, maps, replays and certificate state live under `/var/lib/taserver`.
Map and mod package checks run off the network loop with bounded admission and
work limits (see [resource controls](https://github.com/pocketgeek/ta-engine/blob/main/docs/public-server.md#resource-controls)). Until the first
certificate is issued, ACME retries automatically and the server refuses game
connections.

**Check who is playing.** On the server machine, run `taserver --status` (add
`--port 7678` for a different game port). It prints one JSON object with
`running_games`, `lobby_games` and `connected_clients`, then exits. It needs no
game data, account or certificate arguments, and works with TLS and ACME servers.
See [querying server status](https://github.com/pocketgeek/ta-engine/blob/main/docs/public-server.md#querying-server-status) for what each count includes.

To use an existing certificate instead of Let's Encrypt, or if something goes
wrong, see [Linux service setup](https://github.com/pocketgeek/ta-engine/blob/main/docs/public-server.md#linux-service-setup).
