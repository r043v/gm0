#! /bin/bash
#rm -f /var/run/media/m/CRP\ DISABLD\*
dd bs=1024 conv=nocreat,notrunc if=./pokitto/start.bin of=/var/run/media/m/CRP\ DISABLD/firmware.bin && sync && sudo umount /var/run/media/m/CRP\ DISABLD/
