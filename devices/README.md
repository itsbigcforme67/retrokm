# Device files

One file per kind of box, `devices/<model>.conf`. A `[device]` in
`retrokm.conf` picks one with `model = <model>`; anything written in the
`[device]` section after that wins over the file. See docs/SWITCHING.md.

```
[device matrix]
model = template-text
port = tcp:192.168.1.50:23     # or a serial device: /dev/serial/by-id/...
```

## Keys

For the hub:

| key | meaning |
|---|---|
| `driver` | `extron-sis` (Extron's SIS) or `text` (commands below) |
| `inputs`, `outputs` | how many |
| `baud`, `poll` | serial speed; seconds between re-reading the ties (0 = never) |
| `tie` | text driver: the command that ties `{in}` to `{out}` |
| `untie` | text driver, optional: the command that shows nothing on `{out}` |
| `read` | text driver, optional: asks what `{out}` shows |
| `reply` | text driver, optional: a line that says a tie, like `input {in} -> output {out}` |
| `tie_command`, `read_command` | Extron SIS: the tie and read characters (`!` / `%` by default) |

Commands take C escapes: `\r`, `\n`, `\t`, `\\`, `\xHH`. In `reply`,
`{in}` and `{out}` match numbers, a space matches any run of spaces, `*`
matches anything, letters match either case, and the line may have more
before it. A box with no `reply` is taken at its word: the hub remembers
the tie it sent.

For people and the planner (the hub skips these): `name`, `kind`
(`switch`, `converter`, `monitor`), `signal` (what passes through: `vga`,
`sog`, `component`, `svideo`, `composite`, `hdmi`), `price` (a rough range
and the month it was checked), `notes`.

## Adding one

Copy `template-text.conf`, fill in the commands from the box's manual (look
for "RS-232 protocol" or "API"), and try it: the hub's control socket takes
`tie <device> <in> <out>` and `devices` shows what the hub thinks is tied.
Send it back as a pull request so the next person has it.
