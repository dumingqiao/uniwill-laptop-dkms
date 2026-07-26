# uniwilld JSON API

`uniwilld` exposes a newline-delimited JSON protocol over a Unix domain socket. A directly launched
daemon leaves fan control with the EC unless `--fan-control` is supplied. The packaged systemd unit
supplies this option, so an installed service starts with the manual curve loop enabled.
The default socket path is:

```text
/run/uniwilld.sock
```

Each request is one JSON object followed by `\n`. Each response is one JSON object followed by
`\n`.

Example:

```sh
printf '{"cmd":"status"}\n' | socat - UNIX-CONNECT:/run/uniwilld.sock
```

All successful responses contain:

```json
{"ok":true}
```

Errors use this shape:

```json
{"ok":false,"error":22,"message":"invalid fan control mode"}
```

`error` is the positive errno value returned by the service.

The server accepts multiple concurrent Unix socket clients. A client may use the legacy one-request
form, or subscribe and keep one full-duplex connection open for all subsequent commands and events.
The control center uses only the persistent form.

## Persistent full-duplex connection

Send `subscribe` as the first request:

```json
{"cmd":"subscribe"}
```

The daemon acknowledges the persistent connection:

```json
{"ok":true,"event":"subscribed","revision":42}
```

After that, the client may send any normal command on the same socket. Command responses remain in
request order. State events may appear between command responses and are identified by the `event`
field:

```json
{
  "ok": true,
  "event": "state_changed",
  "revision": 43,
  "snapshot": {
    "temp1_input": 52000,
    "fan1_input": 2300,
    "power_mode": "balanced",
    "active_profile": 2,
    "active_source": "ac",
    "lightbar": {},
    "batteries": []
  }
}
```

The daemon emits `state_changed` immediately after a successful mutating command and when its
hardware monitor detects changed temperatures, fan values, power source, battery data, keyboard
state, or other exported endpoints. Every event carries one complete daemon-side snapshot, so the
client does not need follow-up reads. Snapshot fields that are temporarily unavailable are `null`;
clients should retain the last valid value for those fields rather than clearing the UI. No
client-side polling is required.

If the socket disconnects, the client must discard any in-flight response association and establish
a new subscribed connection before sending more commands.

## Generic Endpoints

These commands expose all sysfs endpoints discovered by the daemon.

### list

Lists discovered endpoints and whether each one is writable.

Request:

```json
{"cmd":"list"}
```

Response:

```json
{"ok":true,"endpoints":[{"name":"temp1_input","writable":false},{"name":"pwm1","writable":true}]}
```

### get

Reads one endpoint by name.

Request:

```json
{"cmd":"get","name":"temp1_input"}
```

Response:

```json
{"ok":true,"name":"temp1_input","value":"52000"}
```

### set

Writes one writable endpoint by name, then reads it back.

Request:

```json
{"cmd":"set","name":"performance_profile","value":2}
```

String values are also accepted:

```json
{"cmd":"set","name":"pwm1","value":"120\n"}
```

Response:

```json
{"ok":true,"name":"performance_profile","value":"2"}
```

### get_all

Reads all currently discovered endpoints. Endpoints that fail to read are skipped.

Request:

```json
{"cmd":"get_all"}
```

Response:

```json
{"ok":true,"values":{"temp1_input":"52000","temp2_input":"48000","pwm1":"90"}}
```

## Status

### get_version

Returns the SemVer release version, independent build number, and combined build identity.

Request:

```json
{"cmd":"get_version"}
```

Response:

```json
{"ok":true,"name":"uniwilld","version":"0.1.0","build":"1","full":"0.1.0+1"}
```

### get_device_details

Returns privileged static hardware details. NVMe SMART is read through
`libblockdev-nvme`/libnvme and ATA SMART through `libblockdev-smart`/libatasmart; no command-line
SMART utility is launched. Filesystem free space is read through `libblockdev-fs` for each
partition, including supported unmounted filesystems. Unallocated partition-table capacity is
included in the disk's available capacity. Memory modules are read from SMBIOS type 17 records.

Request:

```json
{"cmd":"get_device_details"}
```

The response contains a `storage` array with `available_bytes`, `available_percent`, firmware,
health, wear, spare capacity, temperature, power-on counters, unsafe shutdowns, media errors, and
bytes written when the hardware exposes them. `memory.slots` is the physical slot count and
`memory.devices` contains installed module capacity, locator, manufacturer, part number, memory
type, form factor, and transfer speed.

### status

Returns discovered paths and current fan/temperature values.

Request:

```json
{"cmd":"status"}
```

Response:

```json
{
  "ok": true,
  "hwmon": "/sys/class/hwmon/hwmon3",
  "platform": "/sys/bus/platform/devices/INOU0000:00",
  "temp1_input": "52000",
  "temp2_input": "48000",
  "fan1_input": "2400",
  "fan2_input": "2200",
  "pwm1": "90",
  "pwm2": "80",
  "pwm1_enable": "2",
  "pwm2_enable": "2",
  "fan_control": "auto",
  "curve_control": false,
  "system_power_mode": "balanced",
  "power_mode_auto_sync": true,
  "last_synced_power_profile": 2,
  "active_profile": 2,
  "active_source": "ac"
}
```

## Temperatures

The service uses these same readings for curve control:

- CPU curve: reads `temp1_input`, writes `pwm1`.
- GPU curve: reads `temp2_input`, writes `pwm2`.

### get_cpu_temp

Request:

```json
{"cmd":"get_cpu_temp"}
```

Response:

```json
{"ok":true,"sensor":"cpu","temp_millidegree":52000,"temp_c":52,"curve_fan":"cpu"}
```

### get_gpu_temp

Request:

```json
{"cmd":"get_gpu_temp"}
```

Response:

```json
{"ok":true,"sensor":"gpu","temp_millidegree":48000,"temp_c":48,"curve_fan":"gpu"}
```

### get_temps

Request:

```json
{"cmd":"get_temps"}
```

Response:

```json
{
  "ok": true,
  "cpu": {"available": true, "temp_millidegree": 52000, "temp_c": 52, "curve_fan": "cpu"},
  "gpu": {"available": true, "temp_millidegree": 48000, "temp_c": 48, "curve_fan": "gpu"}
}
```

## Fan RPM

These commands read the fan tachometer values exposed by hwmon.

### get_cpu_fan_rpm

Reads `fan1_input`, the main/CPU fan tachometer value.

Request:

```json
{"cmd":"get_cpu_fan_rpm"}
```

Response:

```json
{"ok":true,"fan":"cpu","sensor":"fan1_input","rpm":2400}
```

### get_gpu_fan_rpm

Reads `fan2_input`, the secondary/GPU fan tachometer value.

Request:

```json
{"cmd":"get_gpu_fan_rpm"}
```

Response:

```json
{"ok":true,"fan":"gpu","sensor":"fan2_input","rpm":2200}
```

### get_fan_rpms

Reads both fan tachometer values.

Request:

```json
{"cmd":"get_fan_rpms"}
```

Response:

```json
{
  "ok": true,
  "cpu": {"available": true, "sensor": "fan1_input", "rpm": 2400},
  "gpu": {"available": true, "sensor": "fan2_input", "rpm": 2200}
}
```

## Battery

Battery commands read Linux power-supply attributes from `/sys/class/power_supply/BAT*`.
Field availability varies by firmware and battery driver. Numeric fields keep kernel sysfs units:

- `capacity_percent`: percent.
- `energy_*_uwh`: microwatt-hours.
- `charge_*_uah`: microamp-hours.
- `voltage_*_uv`: microvolts.
- `power_now_uw`: microwatts.
- `current_now_ua`: microamps.
- `charge_control_end_threshold`: percent.

### get_battery

Reads one battery. If `name` is omitted, the first battery is used.

Request:

```json
{"cmd":"get_battery"}
```

or:

```json
{"cmd":"get_battery","name":"BAT0"}
```

Response:

```json
{
  "ok": true,
  "battery": {
    "name": "BAT0",
    "manufacturer": "Example",
    "model_name": "Pack",
    "serial_number": "1234",
    "technology": "Li-ion",
    "status": "Charging",
    "capacity_percent": 76,
    "health": "Good",
    "cycle_count": 42,
    "charge_control_end_threshold": 80,
    "energy_now_uwh": 45600000,
    "energy_full_uwh": 60000000,
    "energy_full_design_uwh": 73000000,
    "charge_now_uah": null,
    "charge_full_uah": null,
    "charge_full_design_uah": null,
    "voltage_now_uv": 12000000,
    "voltage_min_design_uv": 11550000,
    "power_now_uw": 24000000,
    "current_now_ua": null
  }
}
```

### get_batteries

Reads all batteries.

Request:

```json
{"cmd":"get_batteries"}
```

Response:

```json
{"ok":true,"batteries":[{"name":"BAT0","status":"Discharging","capacity_percent":76}]}
```

The real response includes the same fields as `get_battery`.

### set_battery_charge_limit

Writes `charge_control_end_threshold`. If `name` is omitted, the first battery is used.
Valid thresholds are `1` through `100`.

Request:

```json
{"cmd":"set_battery_charge_limit","threshold":80}
```

or:

```json
{"cmd":"set_battery_charge_limit","name":"BAT0","value":80}
```

Response:

```json
{"ok":true,"battery":{"name":"BAT0","charge_control_end_threshold":80}}
```

## Fan Control

### get_fan_control

Returns whether the daemon is using its curve loop or the EC is controlling fans automatically.

Request:

```json
{"cmd":"get_fan_control"}
```

Response:

```json
{"ok":true,"mode":"auto","curve_control":false,"pwm1_enable":"2","pwm2_enable":"2"}
```

### set_fan_control

Switches fan control mode.

`manual` enables the daemon's curve loop and writes `1` to `pwm1_enable` and `pwm2_enable`.
`auto` disables the daemon's curve loop and writes `2` to `pwm1_enable` and `pwm2_enable`.
When manual mode is active, daemon shutdown and the kernel's five-second PWM-update watchdog both
return fan control to the EC. The packaged service starts in `manual`; a direct invocation without
`--fan-control` starts in `auto`.

Request:

```json
{"cmd":"set_fan_control","mode":"auto"}
```

or:

```json
{"cmd":"set_fan_control","mode":"manual"}
```

Response:

```json
{"ok":true,"mode":"auto","curve_control":false,"pwm1_enable":"2","pwm2_enable":"2"}
```

### tick

Runs one fan-control tick immediately. If fan control is `auto`, this is a no-op.

Request:

```json
{"cmd":"tick"}
```

Response:

```json
{"ok":true}
```

## Fan Boost

### get_fan_boost

Request:

```json
{"cmd":"get_fan_boost"}
```

Response:

```json
{"ok":true,"fan_boost":false,"fan_control":"manual","restore_pending":false,"restore_mode":"auto"}
```

### set_fan_boost

When enabling boost, the service saves the current fan-control mode, switches fans to EC automatic
mode, then enables boost.

When disabling boost, it disables boost first and restores the saved fan-control mode.

Request:

```json
{"cmd":"set_fan_boost","value":true}
```

Response:

```json
{"ok":true,"fan_boost":true,"fan_control":"auto","restore_pending":true,"restore_mode":"manual"}
```

Disable:

```json
{"cmd":"set_fan_boost","value":false}
```

## Fan Curves

Fan curves are persisted by the daemon per power source and restored automatically at service
startup and whenever AC/battery state changes. Pass `"source":"ac"` or
`"source":"battery"`; omitting `source` targets the currently active power source.

Fan names:

- CPU/main fan: `cpu`, `main`, or `0`.
- GPU/secondary fan: `gpu`, `secondary`, or `1`.

Curve point rules:

- `temp` is degrees Celsius.
- `pwm` is `0` to `255`.
- Points must be sorted by increasing `temp`.
- Up to 16 points are accepted.
- Values between points use linear interpolation.
- The last point must be PWM `255` at or below 100 degrees Celsius.

### get_curve

Request:

```json
{"cmd":"get_curve","fan":"cpu","source":"ac"}
```

Response:

```json
{"ok":true,"curve":{"fan":"cpu","points":[{"temp":35,"pwm":0},{"temp":50,"pwm":70}]}}
```

### set_curve

Request:

```json
{
  "cmd": "set_curve",
  "fan": "cpu",
  "source": "ac",
  "points": [
    {"temp": 35, "pwm": 0},
    {"temp": 50, "pwm": 70},
    {"temp": 70, "pwm": 150},
    {"temp": 90, "pwm": 255}
  ]
}
```

Response:

```json
{"ok":true,"curve":{"fan":"cpu","points":[{"temp":35,"pwm":0},{"temp":50,"pwm":70},{"temp":70,"pwm":150},{"temp":90,"pwm":255}]}}
```

## Persistent Profile Slots

The daemon stores three profile slots. Each slot contains separate `ac` and `battery` branches;
each branch independently selects a hardware/system power level and an EC fan level. The state is
atomically persisted in `/var/lib/uniwilld/state.conf` and restored on service startup.
The selected slot is persisted independently from the power level stored in that slot. Applying a
slot writes its branch's `hardware_power_mode`, updates the chassis indicator to that applied power
level, applies its fan mode, and syncs the Linux system power profile.
The same versioned state file also stores manual-curve enablement, AC/battery CPU and GPU curves,
passive-cooling state, and Whisper/benchmark fan-mode selections.

### get_profiles

```json
{"cmd":"get_profiles"}
```

`get_profile_config` is an alias. The response includes all six branches plus the active slot and
detected power source.

### set_profile_config

Power and fan fields may be changed together or independently:

```json
{
  "cmd": "set_profile_config",
  "profile": 2,
  "source": "battery",
  "power_mode": "battery_saver",
  "fan_mode": "quiet"
}
```

Profiles are `1` (performance), `2` (balanced), and `3` (battery saver). `source` accepts `ac`,
`battery`, or `dc`. Both mode fields accept `performance`, `balanced`, or `battery_saver` and their
documented aliases. Updating the active branch applies it immediately; otherwise it is applied the
next time that slot and source become active.

## Active Profile

`activate_profile` selects and persists a slot, then applies that slot's branch for the current
AC/battery source. The selected slot and all six branch configurations are independent persisted
values.

`get_power_mode` reads the currently indicated/applied power level. `set_power_mode` changes and
persists the active slot's current-source power level, then immediately applies it. Applying a
branch sets the EC hardware power mode (PL1/PL2, GPU D-State, CTGP/Dynamic Boost policy), the
chassis mode indicator, the EC fan mode, and the Linux system power profile.

### get_power_mode

Request:

```json
{"cmd":"get_power_mode"}
```

Response:

```json
{"ok":true,"profile":2,"mode":"standard"}
```

### set_power_mode

Modes:

- `performance`: profile `1`.
- `standard`, `balanced`, or `balance`: profile `2`.
- `quiet` or `silent`: profile `3`.

Request:

```json
{"cmd":"set_power_mode","mode":"performance"}
```

Numeric values are also accepted:

```json
{"cmd":"set_power_mode","profile":2}
```

Response:

```json
{"ok":true,"profile":1,"mode":"performance"}
```

### activate_profile

```json
{"cmd":"activate_profile","profile":2}
```

The response contains the same persisted state object as `get_profiles`.

## System Power Mode

These commands operate only the system power profile over D-Bus. They require
`power-profiles-daemon` to provide the `org.freedesktop.UPower.PowerProfiles` service.

Supported system modes:

- `performance`, `perf`, `high-performance`, or `high_performance`: D-Bus profile `performance`.
- `balanced`, `balance`, or `standard`: D-Bus profile `balanced`.
- `power-saver`, `powersave`, `power_saver`, `quiet`, `silent`, or `save-power`: D-Bus profile
  `power-saver`.

### get_system_power_mode

Request:

```json
{"cmd":"get_system_power_mode"}
```

Response:

```json
{"ok":true,"mode":"balanced"}
```

### set_system_power_mode

Request:

```json
{"cmd":"set_system_power_mode","mode":"performance"}
```

Response:

```json
{"ok":true,"mode":"performance"}
```

## Fan Mode

This selects the performance, standard, quiet, whisper, or benchmark curve and updates the driver's
`fan_mode` platform attribute. It is distinct from `set_fan_control`, which switches between the
daemon's manual curve loop and EC automatic control.

In manual mode, the selected service-side curve is applied immediately. In automatic mode, the EC
firmware preset remains responsible for fan speeds and hardware safeguards.

Benchmark has exclusive ownership of fan speed until another fan mode is explicitly selected.
WhisperMode similarly suspends the userspace curve while its firmware quiet policy is selected.
The daemon keeps the manual-curve preference and accepts/persists curve updates during either mode,
but does not write PWM values; the saved curve resumes as soon as performance, standard, or quiet is
selected. Passive cooling is independent and does not change this priority.

### get_fan_mode

Request:

```json
{"cmd":"get_fan_mode"}
```

Response:

```json
{"ok":true,"fan_mode":2,"mode":"standard"}
```

### set_fan_mode

Modes:

- `performance` or `perf`: fan mode `1`.
- `standard`, `balanced`, or `balance`: fan mode `2`.
- `quiet` or `silent`: fan mode `3`.
- `whisper`: fan mode `4`.
- `benchmark` or `boost`: fan mode `5`.

Request:

```json
{"cmd":"set_fan_mode","mode":"quiet"}
```

Numeric values are also accepted:

```json
{"cmd":"set_fan_mode","fan_mode":2}
```

Response:

```json
{"ok":true,"fan_mode":3,"mode":"quiet","source":"ac","persisted":true,"applied":true}
```

## Keyboard Toggles

Boolean values can be JSON booleans, `0`/`1`, or strings like `on`, `off`, `enable`, and `disable`.

### get_fn_lock

Request:

```json
{"cmd":"get_fn_lock"}
```

Response:

```json
{"ok":true,"fn_lock":true}
```

### set_fn_lock

Request:

```json
{"cmd":"set_fn_lock","value":true}
```

### get_touchpad_toggle

Reads the driver's `touchpad_toggle_enable` attribute.

Request:

```json
{"cmd":"get_touchpad_toggle"}
```

Response:

```json
{"ok":true,"touchpad_toggle_enable":true}
```

### set_touchpad_toggle

Sets the driver's `touchpad_toggle_enable` attribute.

Request:

```json
{"cmd":"set_touchpad_toggle","value":false}
```

Response:

```json
{"ok":true,"touchpad_toggle_enable":false}
```

Convenience commands:

```json
{"cmd":"enable_touchpad_toggle"}
```

```json
{"cmd":"disable_touchpad_toggle"}
```

### set_touchpad_state

Sends the desktop's actual enabled state to the `i2c-UNIW0001:00` precision touchpad firmware.
This is the state/indicator operation, not the separate firmware hotkey-enable switch above.
`sync_touchpad_state` is accepted as an alias.

Request:

```json
{"cmd":"set_touchpad_state","enabled":false}
```

Response:

```json
{"ok":true,"touchpad_enabled":false,"disabled_led":true}
```

### get_super_key

Aliases: `get_super_lock`.

Request:

```json
{"cmd":"get_super_key"}
```

Response:

```json
{"ok":true,"super_key_enabled":true}
```

### set_super_key

Aliases: `set_super_lock`.

Request:

```json
{"cmd":"set_super_key","value":false}
```

## Lightbar

### get_lightbar

Request:

```json
{"cmd":"get_lightbar"}
```

Response:

```json
{
  "ok": true,
  "brightness": "160",
  "max_brightness": "200",
  "multi_intensity": "255 80 0",
  "rainbow": true,
  "rainbow_mode": true,
  "breathing_in_suspend": true,
  "sleep_breathing": true,
  "source": "ac",
  "state": {
    "global_enabled": true,
    "active_source": "ac",
    "sources": {
      "ac": {
        "enabled": true,
        "brightness": 160,
        "red": 255,
        "green": 80,
        "blue": 0,
        "rainbow": true,
        "breathing": true
      },
      "battery": {
        "enabled": true,
        "brightness": 96,
        "red": 80,
        "green": 150,
        "blue": 255,
        "rainbow": false,
        "breathing": false
      }
    }
  }
}
```

### set_lightbar

Supported fields:

- `brightness`: LED brightness.
- `red`, `green`, `blue`: RGB multi-intensity values.
- `rainbow` or `rainbow_mode`: maps to the driver's `rainbow_animation`.
- `breathing`, `breathing_in_suspend`, or `sleep_breathing`: maps to the driver's suspend breathing switch.
- `global_enabled`: master lightbar enable state.
- `source`: `ac` or `battery`; each source has an independent persisted configuration.
- `enabled`: whether the lightbar is enabled for the selected source.

Set brightness and color:

```json
{"cmd":"set_lightbar","source":"ac","global_enabled":true,"enabled":true,"brightness":160,"red":255,"green":80,"blue":0}
```

Enable rainbow and sleep breathing:

```json
{"cmd":"set_lightbar","source":"battery","rainbow_mode":true,"sleep_breathing":true}
```

Response:

```json
The response includes the active hardware readback plus the complete persisted `state` object shown
above. When the selected source is active, the new configuration is applied immediately. Otherwise
it is applied automatically when the power source changes.
```

## Keyboard Backlight

These commands control the ITE 8291 RGB keyboard backlight registered by the kernel module. The
brightness range is 0 through 50. AC and battery branches are persisted independently and applied
automatically when the power source changes. The hardware controller supports `solid`, `breathing`,
`wave`, `random`, `rainbow`, `ripple`, `marquee`, `raindrop`, `aurora`, and `fireworks`.

### get_keyboard_backlight

```json
{"cmd":"get_keyboard_backlight"}
```

```json
{"ok":true,"source":"ac","state":{"available":true,"global_enabled":true,"active_source":"ac","sources":{"ac":{"enabled":true,"brightness":50,"red":255,"green":112,"blue":46,"effect":"aurora","speed":3,"direction":"left","reactive":true},"battery":{"enabled":true,"brightness":25,"red":92,"green":128,"blue":255,"effect":"solid","speed":5,"direction":"left","reactive":false}}}}
```

`available` indicates that the loaded kernel driver exposes the hardware-effect attributes.

### set_keyboard_backlight

Save and apply a complete AC profile:

```json
{"cmd":"set_keyboard_backlight","source":"ac","global_enabled":true,"enabled":true,"brightness":42,"color":"bf38f0","effect":"aurora","speed":3,"direction":"left","reactive":true}
```

RGB components can be used instead of `color`:

```json
{"cmd":"set_keyboard_backlight","source":"battery","red":40,"green":100,"blue":255,"effect":"breathing","speed":6}
```

The optional leading `#` is accepted. `direction` is `left`, `right`, `up`, or `down`; it is used
by the wave effect. `reactive` is supported by random, ripple, aurora, and fireworks.

## Discovery

### reload

Rediscovers sysfs endpoints.

Request:

```json
{"cmd":"reload"}
```

Response:

```json
{"ok":true,"endpoints":24}
```
