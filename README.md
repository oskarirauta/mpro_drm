# MPro USB Display Driver

Linux-kerneliajuri VoCore MPro USB-näytöille (Vendor ID `0xc872`, Product ID
`0x1004`). Tukee näytön piirtoa DRM/KMS-rajapinnan kautta, taustavalon
säätöä standardin Linux backlight class -laitteena ja kahden sormen
kosketusta multitouch-input-laitteena.

## Tuetut laitteet

| Screen ID    | Malli       | Kuvaus              | Resoluutio  |
|--------------|-------------|---------------------|-------------|
| `0x00000004` | MPRO-4      | MPro 4"             | 480×800     |
| `0x00000104` | MPRO-4      | MPro 4"             | 480×800     |
| `0x00000b04` | MPRO-4      | MPro 4"             | 480×800     |
| `0x00000304` | MPRO-4IN3   | MPro 4.3"           | 480×800     |
| `0x00000005` | MPRO-5      | MPro 5"             | 480×854     |
| `0x00001005` | MPRO-5H     | MPro 5" (OLED)      | 720×1280    |
| `0x00000007` | MPRO-6IN8   | MPro 6.8"           | 800×480     |
| `0x00000403` | MPRO-3IN4   | MPro 3.4" (round)   | 800×800     |
| `0x0000000a` | MPRO-10     | MPro 10"            | 1024×600    |

Tarkka Screen ID näkyy `dmesg`-viesteissä ja
`/sys/bus/usb/drivers/mpro/*/screen_id`-tiedostossa moduulin latauksen
jälkeen.

## Riippuvuudet

- Linux-kerneli 6.18 tai uudempi
- Kerneli-konfiguraatio: `CONFIG_DRM`, `CONFIG_DRM_GEM_SHMEM_HELPER`,
  `CONFIG_DRM_KMS_HELPER`, `CONFIG_BACKLIGHT_CLASS_DEVICE`,
  `CONFIG_INPUT`, `CONFIG_USB`, `CONFIG_MFD_CORE`,
  `CONFIG_LZ4_COMPRESS` (vain jos LZ4-pakkausta käytetään)
- Kerneli-headerit (`/lib/modules/$(uname -r)/build`)

## Kääntäminen

```sh
make
```

Lopputuloksena syntyy neljä moduulia:

- `mpro.ko` — pääajuri (USB probe, MFD-rekisteröinti)
- `drm/mpro_drm.ko` — DRM/KMS-näyttöajuri
- `backlight/mpro_backlight.ko` — taustavalon ohjain
- `touchscreen/mpro_touchscreen.ko` — kosketusnäytön input-ajuri

Asentaminen:

```sh
sudo make modules_install
sudo depmod -a
```

Muista myös rekisteröidä DKMS:n kautta jos haluat että moduulit
käännetään automaattisesti kerneli-päivityksissä — tämä on **ei vielä
tuettu** mutta tulossa.

## Käyttö

### Lataaminen

Kun MPro on liitetty USB:hen ja moduulit on asennettu, ne latautuvat
automaattisesti udev-mekanismin kautta. Manuaalinen lataus:

```sh
sudo modprobe mpro
```

apsi-moduulit (`mpro_drm`, `mpro_backlight`, `mpro_touchscreen`)
latautuvat automaattisesti MFD-mekanismin kautta.

Tarkista että kaikki latautui:

```sh
lsmod | grep mpro
dmesg | grep -i mpro
```

Pitäisi näkyä jotain tähän tapaan:

```
mpro: Detected MPro 6.8"
mpro_drm: [drm] fb1: mprodrmfb frame buffer device
mpro_backlight: backlight registered: mpro-3-3 (default 128/255)
mpro_touchscreen: touchscreen registered (800x480, 2-finger MT)
```

### Konsoli (fbcon)
Saadaksesi tekstikonsolin näkymään MPro-näytöllä, lisää
kerneli-komentoriville:

```
mpro.fbdev:1
fbcon=map:1
```

Jos järjestelmässä on toinen DRM-laite (kuten `virtio_gpu` tai integroitu
GPU), se varaa fb0:n ja MPro saa fb1:n. Voit halutessasi piilottaa
toissijaisen DRM-laitteen `module_blacklist=virtio_gpu`-parametrilla.

fbdev emulointi estää näytön sammumisen virranhallinnan puolesta, sillä
fbdev pitää näytön pipelinen pysyvästi aktiivisena.

### Graafinen ympäristö

DRM-laite (`/dev/dri/card1` tai vastaava) toimii suoraan Wayland-
kompositoreissa (Weston, Sway, Mutter) ja X.Org:ssa.

```sh
weston --backend=drm --tty=2 --seat=seat1
```

## Sysfs-rajapinnat

### Pää-ajuri (`/sys/bus/usb/drivers/mpro/<usb-id>/`)

| Tiedosto         | Käyttö | Kuvaus |
|------------------|--------|--------|
| `model`          | r--    | Mallin lyhyt nimi (esim. `MPRO-6IN8`) |
| `description`    | r--    | Mallin kuvaus |
| `resolution`     | r--    | `WIDTH HEIGHT` |
| `physical_size`  | r--    | `WIDTH_MM HEIGHT_MM` |
| `version`        | r--    | Firmware-versio (hex) |
| `screen_id`      | r--    | Laitteen Screen ID (hex) |
| `device_id`      | r--    | Laitteen yksilöllinen 8-tavuinen ID |
| `margin`         | r--    | Datan margin tavuissa (0 useimmilla malleilla) |
| `lz4_level`      | rw     | LZ4-pakkaus: `0`=pois, `1`=fast, `2..12`=HC |
| `stats`          | r--    | `submitted=N displayed=N dropped=N` |

### DRM (`/sys/bus/platform/drivers/mpro_drm/<id>/`)

| Tiedosto          | Käyttö | Kuvaus |
|-------------------|--------|--------|
| `rotation`        | rw     | `0`..`7` (0..3=rotate, 4..7=rotate+reflect-x) |
| `brightness`      | rw     | Ohjelmistollinen kirkkaus 0..100 |
| `gamma`           | rw     | Gamma-korjaus `0.50`..`4.00` (oletus `1.00`) |
| `disable_partial` | rw     | `0`/`1` — pakota täyspäivitykset |
| `gamma_lut`       | -w-    | Suora 768-tavun LUT-kirjoitus (R[256] G[256] B[256]) |

### Backlight (`/sys/class/backlight/mpro-<id>/`)

Standardi Linux backlight-rajapinta:

| Tiedosto         | Käyttö | Kuvaus |
|------------------|--------|--------|
| `brightness`     | rw     | Kirkkausarvo 0..255 |
| `max_brightness` | r--    | Maksimi (255) |
| `bl_power`       | rw     | Standardi power state |
| `gamma`          | rw     | Backlight-curve gamma `0.50`..`4.00` |

Yhteensopiva työkalujen kanssa kuten `brightnessctl`, `xbacklight`,
GNOME-paneelit, KDE-paneelit.

```sh
brightnessctl --device='mpro-3-3' set 50%
echo 200 > /sys/class/backlight/mpro-3-3/brightness
```

### Touchscreen (`/dev/input/eventN`)

Standardi Linux input-laite multitouch-protokollalla. Tunnistuu
`evtest`:llä ja libinput:lla nimellä "MPro touchscreen".

```sh
evtest /dev/input/eventN
libinput debug-events --device /dev/input/eventN
```

## Moduuliparametrit

### `mpro`

```sh
# /etc/modprobe.d/mpro.conf
options mpro fbdev=1
options mpro lz4_level=1
```

| Parametri    | Tyyppi | Oletus | Kuvaus |
|--------------|--------|--------|--------|
| `fbdev`      | int    | `0`    | fbdev konsoli.                                                                                     |
| `lz4_level`  | int    | `0`    | LZ4-pakkaus boot-asetuksena. Tukea vain firmware-versioissa ≥ 0.22 ja malleissa ilman marginaalia. |

### `mpro_drm`

| Parametri          | Tyyppi | Oletus | Kuvaus |
|--------------------|--------|--------|--------|
| `disable_partial`  | bool   | `0`    | Pakota täyspäivitykset. Tarvitaan vanhoissa firmware-versioissa (≤ 0.15). |

## Suorituskyky

Pää-ajurin pipeline tukee `latest-wins coalescing`-strategiaa: jos
sovellus tuottaa freimejä nopeammin kuin USB pystyy lähettämään,
vanhemmat freimit pudotetaan automaattisesti. `stats`-sysfs-tiedostosta
näkee kuinka tehokkaasti tämä toimii:

```sh
$ cat /sys/bus/usb/drivers/mpro/3-3:1.0/stats
submitted=17400 displayed=2900 dropped=14500
```

Tässä esimerkissä ~289 fps tuotettiin sovelluksesta, mutta vain ~48 fps
päätyi laitteelle (rajoittavana tekijänä USB 2.0:n läpäisykyky).

LZ4-pakkaus voi parantaa läpimenoa merkittävästi:

```sh
echo 1 > /sys/bus/usb/drivers/mpro/3-3:1.0/lz4_level
```

Tasot:
- `1` = LZ4 default (nopein, kohtuullinen pakkaussuhde — suositeltu)
- `2..12` = LZ4HC (parempi pakkaussuhde, hitaampi)

LZ4 ei toimi marginaalia käyttävissä malleissa (vanhat MPRO-5-yksiköt).

## Virransäästö

Ajuri tukee USB-autosuspendia. Laite suspendoituu automaattisesti kun:
- DRM-pipe on disabled (kompositori ei piirrä)
- Touch-input on suljettu (kukaan ei kuuntele)
- 30 sekuntia kulunut ilman aktiivisuutta

Tarkista nykyinen tila:

```sh
USBDEV=$(readlink -f /sys/bus/usb/drivers/mpro/*)
USBDEV=${USBDEV%/*}    # poista interface-pääte (esim. ":1.0")

cat $USBDEV/power/runtime_status   # active | suspended
cat $USBDEV/power/control          # auto | on
```

Jos `control` on `on`, suspend ei ole sallittu. Aktivoi:

```sh
echo auto > $USBDEV/power/control
```

Useimmissa järjestelmissä `auto` on jo oletusarvo, mutta jotkut
tehohallintaohjelmat (TLP, laptop-mode) voivat muuttaa sen.

## Vianetsintä

### Konsoli ei näy MPro-näytöllä

Lisää boot-parametrit `mpro.fbdev=1` ja `fbcon=map:1`. Jos jo lisätty, tarkista että
muut DRM-ajurit eivät blokkaa MPro:n bind:ia:

```sh
for v in /sys/class/vtconsole/vtcon*; do
    echo "$v: $(cat $v/name) bound=$(cat $v/bind)"
done
```

Voit manuaalisesti vaihtaa konsolin sidoksia:

```sh
echo 0 > /sys/class/vtconsole/vtcon0/bind
echo 1 > /sys/class/vtconsole/vtcon1/bind
```

### Kuva on ylösalaisin tai peilattu

Säädä `rotation`-attribuuttia:

```sh
for i in 0 1 2 3 4 5 6 7; do
    echo "rotation=$i"
    echo $i > /sys/bus/platform/drivers/mpro_drm/*/rotation
    sleep 2
done
```

Arvot 0..3 ovat puhtaita rotaatioita (0°, 90°, 180°, 270°), arvot 4..7
samat + x-peilaus.

Mallin natiivirotation on tallennettu `mpro_model.c`-taulukkoon ja
sovelletaan automaattisesti boottiessa. Jos sinulla on uusi malli joka
kaipaa eri arvoja, ilmoita asia.

### Touchscreen ei reagoi

Tarkista ensin että input-laite on rekisteröitynyt:

```sh
ls /dev/input/event*
cat /proc/bus/input/devices | grep -A4 "MPro touchscreen"
```

Jos ei näy, tarkista `dmesg`. Yleisin syy on että MFD ei pystynyt
lataamaan `mpro_touchscreen`-moduulia automaattisesti — tämä vaatii
`modules.alias`-tiedoston olevan ajantasalla:

```sh
sudo depmod -a
sudo modprobe mpro_touchscreen
```

### LZ4 jumiuttaa näytön

Vanha firmware (< 0.22) ei tue LZ4:ää. Aseta `lz4_level=0`:

```sh
echo 0 > /sys/bus/usb/drivers/mpro/*/lz4_level
```

## Arkkitehtuuri

Ajuri on rakennettu MFD-parent-pohjaiselle (Multi-Function Device)
arkkitehtuurille. Yksi USB-laite altaalla on kolme rinnakkaista
toiminnallisuutta:

```
mpro (USB driver, MFD parent)
├── mpro_drm        (DRM/KMS display)
├── mpro_backlight  (backlight class)
└── mpro_touchscreen (input device)
```

Pää-ajuri (`mpro`) hoitaa USB-rajapinnan, synkroniset kontrolli-
komennot ja asynkronisen frame-pipelinen. Lapsi-moduulit kommunikoivat
pää-ajurin kanssa parent-API:n kautta — ne eivät koskaan kutsu
USB-funktioita suoraan.

### Tiedostorakenne

```
mpro/
├── Makefile, Kbuild
├── mpro.h                      # julkinen API child-drivereille
├── mpro_internal.h             # sisäiset protokolla-määritelmät
├── mpro_core.c                 # USB probe/disconnect/PM, MFD-rekisteröinti
├── mpro_protocol.c             # synkroniset kontrolli-komennot
├── mpro_pipeline.c             # asynkroninen frame-pipeline + LZ4
├── mpro_model.c                # mallitaulukko + tunnistus
├── mpro_screen.c               # screen state -callback-mekanismi
├── mpro_sysfs.c                # parent-tason sysfs
├── drm/
│   ├── Kbuild, mpro_drm.h
│   ├── mpro_drm_main.c         # probe + drm_driver-rekisteröinti
│   ├── mpro_drm_pipe.c         # pipe enable/disable/update, vblank, connector
│   ├── mpro_drm_color.c        # gamma, brightness, formaatti-konversio
│   ├── mpro_drm_sysfs.c        # rotation, brightness, gamma sysfs
│   └── mpro_drm_edid.c         # EDID 1.3 -emulaatio
├── backlight/
│   ├── Kbuild, mpro_backlight.h
│   └── mpro_backlight.c
└── touchscreen/
    ├── Kbuild, mpro_touchscreen.h
    └── mpro_touchscreen.c
```

### Frame-pipeline

DRM-puoli kutsuu yhtä kahdesta funktiosta freimien lähetykseen:

```c
int mpro_send_full_frame(struct mpro_device *mpro,
                         const void *data, size_t len);
int mpro_send_partial_frame(struct mpro_device *mpro,
                            const void *data,
                            u16 x, u16 y, u16 w, u16 h);
```

Pää-ajurin pipeline ottaa freimit vastaan, optionaalisesti pakkaa ne
LZ4:llä, ja lähettää USB:n yli. Yksi siirto on kerrallaan käynnissä;
uudet freimit korvaavat odottavan freimin (latest-wins coalescing).

### Screen state -mekanismi

DRM-puolen `pipe_enable`/`pipe_disable` triggeröi notifikaatiot kaikille
rekisteröityneille child-drivereille:

```c
struct mpro_screen_listener {
    void (*screen_off)(void *priv);
    void (*screen_on)(void *priv);
    void *priv;
};

mpro_screen_listener_register(mpro, &listener);
```

Backlight käyttää tätä sammuttamaan taustavalon kun DRM-pipe menee
blank-tilaan; touchscreen käyttää sitä pysäyttämään URB-pipelinen
turhan virrankäytön välttämiseksi.

## Lisenssi

GPL-2.0. Katso lähdetiedostojen `SPDX-License-Identifier`-rivit.

## Tekijä

Oskari Rauta — `<oskari.rauta@gmail.com>`

Pohjautuu osittain VoCoren alkuperäiseen `mpro_drm`-ajuriin
(<https://github.com/Vonger/mpro_drm>) ja `v2scrctl`-userspace-paketin
LZ4- sekä multitouch-koodiin.
