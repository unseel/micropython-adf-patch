# micropython patch to work with esp-adf

This is micropython patch intended to integrate esp-adf to latest micropython.

## audio

The audio directory is copied from [micropython_adf](https://github.com/espressif/esp-adf/tree/master/micropython_adf) with some modifications.

## patch

__esp-adf__

Remove `sdcard` & `fatfs`

__micropython__

Include adf components

## build

Refer below link to build micropython with audio module.

[docker-micropython-tools-esp32-adf](https://github.com/unseel/docker-micropython-tools-esp32-adf)