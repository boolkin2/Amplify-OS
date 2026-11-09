MyOS - README
==============

Struktura proekta:
  /boot          - Zagruzchik i konfiguratsiya GRUB
  /kernel        - Yadro operatsionnoj sistemy
  /kernel/arch   - Arhitekturno-zavisimyj kod (x86)
  /kernel/drivers- Drajvery ustrojstv
  /kernel/memory - Upravlenie pamyat'yu
  /kernel/fs     - Fajlovaya sistema

Komandy sborki:
  make           - Sobrat' proekt
  make run       - Sobrat' i zapustit' v QEMU
  make clean     - Ochistit' proekt

Realizovannye etapy:
  Etap 1: Podgotovka okruzheniya
  Etap 2: Zagruzchik (GRUB Multiboot)
  Etap 3: Zashchishchyonnyj rezhim
  Etap 4: Yadro na C
  Etap 5: Preryvaniia (IDT/ISR/IRQ)
  Etap 6: Drajver klaviatury
  Etap 7: Tajmer PIT
  Etap 8: Upravlenie pamyat'yu
  Etap 9: Fajlovaya sistema (InitRD)
