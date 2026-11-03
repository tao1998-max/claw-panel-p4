# mqtt_panel example

A minimal network App: connects to an MQTT broker, publishes a heartbeat and
renders commands sent to its command topic.

## Configure

Edit `launcher.json` `args`:

```json
{
  "broker": "mqtt://192.168.0.107:1884",
  "base_topic": "panel/demo"
}
```

- `broker`: `mqtt://host:port` or `mqtts://host:8883` (certificate bundle).
- `base_topic`: the app uses `<base>/status` (out) and `<base>/cmd` (in).

## Deploy

```bash
python scripts/upload_app.py --host http://<device-ip> \
  --app-dir assets/examples/mqtt_panel --volume sd --force
```

Then tap the "MQTT Panel" card (the upload script already refreshed the app
list). The device must be able to reach the broker (same LAN or internet).

## Try it

```bash
# watch status
mosquitto_sub -h 192.168.0.107 -p 1884 -t 'panel/demo/status' -v
# command the panel
mosquitto_pub -h 192.168.0.107 -p 1884 -t 'panel/demo/cmd' -m 'color:#132a4a'
mosquitto_pub -h 192.168.0.107 -p 1884 -t 'panel/demo/cmd' -m 'text:hello from mqtt'
mosquitto_pub -h 192.168.0.107 -p 1884 -t 'panel/demo/cmd' -m 'ping'
```

`ping` answers with `pong` on `<base>/status`. If the broker goes away the app
shows "disconnected, retrying" and reconnects every 5 s.

## Notes

- Runs from `/sdcard/apps/mqtt_panel/` (SD). To keep it on FAT use
  `--volume fat` instead; the App itself is small.
- The App holds one MQTT client (one socket) while running; see
  `references/networking.md` for the socket budget.
