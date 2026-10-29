# Xbox presser

Presses the original Xbox's power button: an M5Stack Atom Lite in an Atom Motion v1.1
base (servo driver at I2C 0x38 on G25/G21, INA226 power monitor at 0x40, 18350 battery),
with a servo on the base's port 1 taped beside the button. It is separate from the Xbox
pad (../xboxpad/) so it stays powered, from a USB or Grove charger, while the console is
off; the pad is powered by the console's USB like any controller.

    ./xbox_power.py state | on | off

`on` / `off` press only when needed, then check: the Xbox is on when the pad answers
and the console is polling it (its report count climbs), or its MAC answers on the LAN.
A press can miss (tape, battery), so each retry presses a little longer.

## The servo

It is a continuous-rotation one (the value is speed: about 90 stops it), so a press is
timed: spin toward the button (`servo push`, for `press <ms>`), spin back as long plus
`servo backms` so the arm just reaches a backstop (the plastic above the eject button),
then stop (`servo stop`). Settings are saved on the board. 380-410 ms clicked on 29 Oct.

## Commands (UDP 24854, or USB serial at 115200)

    echo press | nc -u -w1 xboxpress.local 24854
    show | i2c | power | servo <port 1-4> <deg> | servo stop|push|back <deg>
    servo backms <ms> | press [ms] | reboot

`power` reads the INA226: bus volts and shunt millivolts (negative while charging).
The base charges only with its switch on.

## Build

    cp include/secrets.h.example include/secrets.h      (WiFi)
    pio run -e atomlite -t upload --upload-port /dev/ttyUSB1    (first time, USB)
    pio run -e ota -t upload --upload-port xboxpress.local      (after that)
