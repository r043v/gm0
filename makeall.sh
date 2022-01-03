#! /bin/bash
for game in ./roms/gb/*.gb
do
  game=$(basename "$game")
  game=${game%.*}
  echo "process $game"
  [ -f "roms/header/$game.h" ] || bin2c -o "roms/header/$game.h" -n embedrom "./roms/gb/$game.gb"
  rm -f BUILD/main.o
  rm -f BUILD/gm0.o
  make -j4 rom="$game" #> /dev/null 2>&1
  ./crc BUILD/gm0.bin
  cp BUILD/gm0.bin "./roms/bin/$game.bin"
  cp BUILD/gm0.bin "/var/run/media/m/gb/gb/$game.bin"
done

sync && sudo umount /var/run/media/m/gb
