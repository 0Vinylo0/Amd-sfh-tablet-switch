# sfh-tablet-switch

Módulo externo para el kernel Linux que obtiene el modo físico de un portátil convertible AMD mediante **AMD Sensor Fusion Hub (AMD SFH)** y publica el estado como un interruptor estándar del subsistema Linux Input:

```text
EV_SW / SW_TABLET_MODE
```

El objetivo es cubrir equipos convertibles en los que el firmware y AMD SFH conocen la posición de la pantalla, pero Linux no expone correctamente un evento `SW_TABLET_MODE` que pueda consumir el espacio de usuario.

El módulo fue desarrollado y probado originalmente sobre un **HP OmniBook convertible con plataforma AMD**.

> [!IMPORTANT]
> Este proyecto no implementa por sí mismo la rotación automática de pantalla, un teclado virtual ni cambios visuales del escritorio. Su función es proporcionar a Linux un evento estándar `SW_TABLET_MODE`. La reacción posterior depende de componentes como libinput y del entorno de escritorio.

---

## Índice

- [Funcionamiento](#funcionamiento)
- [Arquitectura](#arquitectura)
- [Estados interpretados](#estados-interpretados)
- [Filtrado de cambios](#filtrado-de-cambios)
- [Requisitos](#requisitos)
- [Compilación](#compilación)
- [Carga manual](#carga-manual)
- [Comprobación](#comprobación)
- [Pruebas con evtest](#pruebas-con-evtest)
- [Pruebas con libinput](#pruebas-con-libinput)
- [Instalación permanente](#instalación-permanente)
- [Actualizaciones del kernel](#actualizaciones-del-kernel)
- [Descarga del módulo](#descarga-del-módulo)
- [Logs y diagnóstico](#logs-y-diagnóstico)
- [Integración con GNOME y otros escritorios](#integración-con-gnome-y-otros-escritorios)
- [Rotación automática](#rotación-automática)
- [Secure Boot](#secure-boot)
- [Limitaciones](#limitaciones)
- [Estructura del código](#estructura-del-código)
- [Licencia](#licencia)

---

## Funcionamiento

El módulo consulta periódicamente la información proporcionada por AMD SFH mediante:

```c
amd_get_sfh_info(&info, MT_SRA);
```

La interfaz del kernel devuelve una estructura `amd_sfh_info` que incluye, entre otros campos:

```c
struct amd_sfh_info {
    u32 ambient_light;
    u8 user_present;
    u32 platform_type;
    u32 laptop_placement;
};
```

Este proyecto utiliza principalmente:

```text
platform_type
```

para determinar si el portátil está en una postura convencional o en una postura convertible.

Cuando cambia el estado, el módulo genera:

```text
SW_TABLET_MODE = 1
```

para modo tablet y:

```text
SW_TABLET_MODE = 0
```

para modo portátil.

---

## Arquitectura

```text
┌─────────────────────────────┐
│ Sensores físicos / bisagra  │
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│ AMD Sensor Fusion Hub       │
│ AMD SFH / MP2 firmware      │
└──────────────┬──────────────┘
               │
               │ amd_get_sfh_info(..., MT_SRA)
               ▼
┌─────────────────────────────┐
│ sfh_tablet_switch.ko        │
│                             │
│ - lee platform_type         │
│ - filtra cambios            │
│ - determina tablet/laptop   │
└──────────────┬──────────────┘
               │
               │ EV_SW
               │ SW_TABLET_MODE
               ▼
┌─────────────────────────────┐
│ Linux Input subsystem       │
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│ libinput / compositor       │
└──────────────┬──────────────┘
               │
        ┌──────┴───────┐
        ▼              ▼
      GNOME           KDE
      Wayland         otros
```

El dispositivo virtual registrado por el módulo se identifica como:

```text
AMD SFH Tablet Mode Switch
```

con:

```text
phys: amd-sfh/tablet-mode
bus:  BUS_HOST
vendor: 0x1022
```

---

## Estados interpretados

El código define los siguientes valores de `platform_type`:

| Valor | Modo | Estado publicado |
|------:|------|------------------|
| `0` | unknown | portátil |
| `1` | lid closed | portátil |
| `2` | clamshell | portátil |
| `3` | flat | portátil |
| `4` | tent | **tablet** |
| `5` | stand | **tablet** |
| `6` | tablet | **tablet** |
| `7` | book | **tablet** |
| `8` | presentation | ignorado |
| `9` | pull-forward | ignorado |
| `15` | invalid | ignorado |

La clasificación está implementada en:

```c
static int sfh_mode_is_tablet(u32 mode)
```

Los modos convertibles configurados actualmente son:

```c
case SFH_MODE_TENT:
case SFH_MODE_STAND:
case SFH_MODE_TABLET:
case SFH_MODE_BOOK:
    return 1;
```

Los modos convencionales son:

```c
case SFH_MODE_UNKNOWN:
case SFH_MODE_LID_CLOSED:
case SFH_MODE_CLAMSHELL:
case SFH_MODE_FLAT:
    return 0;
```

Los valores no reconocidos no cambian el estado actual:

```c
default:
    return -1;
```

> [!NOTE]
> La correspondencia concreta entre `platform_type` y la postura física puede depender del firmware y del equipo. Antes de utilizar este módulo en otro modelo conviene comprobar los valores observados en cada posición.

---

## Filtrado de cambios

Mover la bisagra de un convertible puede hacer que el firmware atraviese varios estados intermedios en pocos milisegundos.

Para evitar cambios espurios, el módulo no publica un nuevo estado inmediatamente.

Configuración actual:

```c
#define POLL_INTERVAL_MS     200
#define STABLE_READS_NEEDED  3
```

Por tanto, se requieren **tres lecturas consecutivas** que produzcan la misma clasificación.

El tiempo mínimo aproximado de estabilización es:

```text
200 ms × 3 ≈ 600 ms
```

La latencia real puede ser ligeramente superior porque el sondeo se ejecuta mediante una `delayed_work` del kernel.

El mecanismo utiliza:

```c
candidate_state
candidate_reads
reported_state
```

Esto reduce transiciones como:

```text
laptop → tablet → laptop → tablet
```

mientras la pantalla está siendo desplazada físicamente.

---

## Requisitos

### Hardware

Se necesita un equipo AMD en el que:

1. exista soporte AMD SFH;
2. `amd_get_sfh_info()` pueda obtener datos `MT_SRA`;
3. `platform_type` cambie correctamente al mover la bisagra.

Este módulo no puede fabricar información que el firmware no proporcione.

### Kernel

El código utiliza:

```c
#include <linux/amd-pmf-io.h>
```

y la función:

```c
amd_get_sfh_info()
```

Por tanto, el kernel utilizado debe proporcionar esa interfaz.

También depende del módulo:

```text
amd_sfh
```

El propio código declara:

```c
MODULE_SOFTDEP("pre: amd_sfh");
```

### Herramientas de compilación

Se necesitan:

- compilador C;
- GNU Make;
- cabeceras del kernel correspondiente al kernel en ejecución.

Comprueba el kernel actual con:

```bash
uname -r
```

---

## Instalación de dependencias

### Arch Linux / EndeavourOS

Para el kernel estándar:

```bash
sudo pacman -S --needed base-devel linux-headers
```

Para `linux-lts`:

```bash
sudo pacman -S --needed base-devel linux-lts-headers
```

Comprueba que las cabeceras coinciden con el kernel cargado:

```bash
uname -r
ls /usr/lib/modules/$(uname -r)/build
```

### Debian / Ubuntu

```bash
sudo apt install build-essential linux-headers-$(uname -r)
```

---

## Compilación

El `Makefile` es:

```make
obj-m += sfh_tablet_switch.o
```

Compila con el sistema Kbuild del kernel:

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD modules
```

Si el repositorio incluye un `Makefile` con objetivos auxiliares, también puede utilizarse simplemente:

```bash
make
```

El resultado principal es:

```text
sfh_tablet_switch.ko
```

Comprueba sus metadatos con:

```bash
modinfo ./sfh_tablet_switch.ko
```

Debe mostrar información similar a:

```text
name:           sfh_tablet_switch
license:        GPL
description:    AMD SFH automatic tablet mode switch
author:         vinylo
depends:        amd_sfh
```

### Importante: `vermagic`

Comprueba:

```bash
modinfo ./sfh_tablet_switch.ko | grep vermagic
uname -r
```

Un `.ko` debe compilarse para el kernel que va a cargarlo.

El binario original utilizado durante el desarrollo fue compilado para:

```text
6.18.42-1-lts
```

No debe considerarse un binario portable.

---

## Carga manual

### Método recomendado: modprobe

Primero asegúrate de que AMD SFH está disponible:

```bash
sudo modprobe amd_sfh
```

Después carga el módulo:

```bash
sudo insmod ./sfh_tablet_switch.ko
```

Comprueba:

```bash
lsmod | grep -E 'sfh_tablet_switch|amd_sfh'
```

También puedes observar los mensajes del kernel:

```bash
sudo journalctl -kf
```

o:

```bash
sudo dmesg -w
```

Al arrancar debería aparecer:

```text
sfh_tablet_switch: iniciado
```

---

## Comprobación

Puedes buscar el dispositivo input creado por el módulo:

```bash
grep -A8 -B2 "AMD SFH Tablet Mode Switch" /proc/bus/input/devices
```

También:

```bash
cat /proc/bus/input/devices
```

Debería aparecer un dispositivo similar a:

```text
N: Name="AMD SFH Tablet Mode Switch"
P: Phys=amd-sfh/tablet-mode
```

---

## Pruebas con evtest

Instala `evtest` si no está disponible.

### Arch Linux

```bash
sudo pacman -S evtest
```

### Debian / Ubuntu

```bash
sudo apt install evtest
```

Ejecuta:

```bash
sudo evtest
```

Busca:

```text
AMD SFH Tablet Mode Switch
```

Selecciona su `/dev/input/eventX`.

Al entrar en modo tablet debe aparecer un evento equivalente a:

```text
Event: type 5 (EV_SW), code 1 (SW_TABLET_MODE), value 1
```

Al volver a modo portátil:

```text
Event: type 5 (EV_SW), code 1 (SW_TABLET_MODE), value 0
```

En Linux, `SW_TABLET_MODE` está definido como un switch cuyo valor activo significa que el equipo se encuentra en modo tablet.

---

## Pruebas con libinput

Lista los dispositivos conocidos por libinput:

```bash
sudo libinput list-devices
```

Busca:

```text
AMD SFH Tablet Mode Switch
```

También puedes observar eventos en tiempo real:

```bash
sudo libinput debug-events
```

Mueve la pantalla entre las posiciones portátil y tablet y comprueba que se genera el cambio correspondiente.

En sistemas con libinput, `SW_TABLET_MODE` es el mecanismo estándar utilizado para notificar el modo tablet. Dependiendo de la configuración del sistema, libinput puede utilizarlo para inhibir dispositivos internos como teclado, touchpad o pointing stick mientras el convertible está plegado.

---

## Instalación permanente

Una vez probado el módulo, puede copiarse al árbol de módulos del kernel actual.

```bash
sudo install -Dm644 sfh_tablet_switch.ko \
    /usr/lib/modules/$(uname -r)/extra/sfh_tablet_switch.ko
```

Actualiza las dependencias:

```bash
sudo depmod -a
```

Ahora debería poder cargarse mediante:

```bash
sudo modprobe sfh_tablet_switch
```

Comprueba:

```bash
lsmod | grep sfh_tablet_switch
```

### Cargar automáticamente al arrancar

Crea:

```text
/etc/modules-load.d/sfh-tablet-switch.conf
```

con:

```text
sfh_tablet_switch
```

Por ejemplo:

```bash
echo sfh_tablet_switch | sudo tee /etc/modules-load.d/sfh-tablet-switch.conf
```

Después reinicia o carga el módulo manualmente:

```bash
sudo modprobe sfh_tablet_switch
```

La declaración:

```c
MODULE_SOFTDEP("pre: amd_sfh");
```

indica a `modprobe` que `amd_sfh` debe cargarse antes.

> [!NOTE]
> `insmod` carga directamente un archivo `.ko` y no resuelve dependencias de la misma forma que `modprobe`. Para una instalación permanente es preferible `modprobe`.

---

## Actualizaciones del kernel

Este proyecto es un **módulo externo al árbol del kernel**.

El archivo:

```text
sfh_tablet_switch.ko
```

queda vinculado a la ABI y configuración del kernel contra el que se ha compilado.

Después de actualizar el kernel puede ser necesario recompilar:

```bash
make clean
make -C /lib/modules/$(uname -r)/build M=$PWD modules
```

Después vuelve a instalarlo:

```bash
sudo install -Dm644 sfh_tablet_switch.ko \
    /usr/lib/modules/$(uname -r)/extra/sfh_tablet_switch.ko

sudo depmod -a
sudo modprobe sfh_tablet_switch
```

Para despliegues permanentes resulta recomendable convertir el proyecto a **DKMS**, de forma que el módulo se reconstruya automáticamente al instalar un kernel nuevo.

---

## Descarga del módulo

Para descargarlo:

```bash
sudo modprobe -r sfh_tablet_switch
```

También puede utilizarse:

```bash
sudo rmmod sfh_tablet_switch
```

Antes de eliminar el dispositivo input, el módulo publica explícitamente:

```text
SW_TABLET_MODE = 0
```

para dejar el sistema en estado de portátil.

Después cancela el trabajo periódico:

```c
cancel_delayed_work_sync(&poll_work);
```

y elimina el dispositivo mediante:

```c
input_unregister_device(tablet_input);
```

---

## Logs y diagnóstico

### Seguir mensajes en tiempo real

```bash
sudo journalctl -kf | grep --line-buffered sfh_tablet_switch
```

O:

```bash
sudo dmesg -w | grep --line-buffered sfh_tablet_switch
```

Cuando cambia el valor bruto de AMD SFH se registra:

```text
sfh_tablet_switch: raw platform_type=N placement=N
```

Cuando cambia el estado lógico publicado:

```text
sfh_tablet_switch: SW_TABLET_MODE=1 platform_type=6 placement=...
```

o:

```text
sfh_tablet_switch: SW_TABLET_MODE=0 platform_type=2 placement=...
```

### Verificar AMD SFH

```bash
lsmod | grep amd_sfh
```

Si no está cargado:

```bash
sudo modprobe amd_sfh
```

### Comprobar errores de consulta

Si `amd_get_sfh_info()` falla, el módulo utiliza:

```c
pr_warn_ratelimited()
```

por lo que pueden aparecer mensajes del tipo:

```text
sfh_tablet_switch: amd_get_sfh_info error=-N
```

El uso de `pr_warn_ratelimited()` evita inundar el kernel log en caso de error persistente.

---

## Integración con GNOME y otros escritorios

El módulo **no controla directamente GNOME**.

Su responsabilidad termina aquí:

```text
AMD SFH
   ↓
sfh_tablet_switch
   ↓
EV_SW / SW_TABLET_MODE
```

A partir de ahí interviene el espacio de usuario:

```text
SW_TABLET_MODE
   ↓
libinput
   ↓
compositor / entorno de escritorio
```

Esto es importante porque permite usar la infraestructura estándar de Linux en lugar de crear scripts específicos para GNOME, KDE o cada aplicación.

### Qué puede conseguir `SW_TABLET_MODE`

En una pila de entrada correctamente configurada puede utilizarse para que los dispositivos internos no se usen accidentalmente cuando el portátil está plegado como tablet.

Por ejemplo:

```text
modo portátil
    ├── teclado interno activo
    └── touchpad activo

modo tablet
    ├── teclado interno inhibido
    └── touchpad inhibido
```

El comportamiento final depende de libinput, udev, el compositor y la versión/configuración del escritorio.

---

## Rotación automática

`SW_TABLET_MODE` responde a:

> ¿El equipo está funcionando como portátil o como tablet?

No responde a:

> ¿En qué orientación está físicamente la pantalla?

La orientación suele obtenerse mediante un acelerómetro y la infraestructura IIO.

El flujo típico es independiente:

```text
acelerómetro
    ↓
Linux IIO
    ↓
iio-sensor-proxy / compositor
    ↓
orientación
    ↓
rotación de pantalla
```

Por tanto:

```text
sfh_tablet_switch
    → modo laptop/tablet

sensor de orientación
    → normal/left/right/inverted
```

Son dos problemas distintos.

---

## Secure Boot

Si Secure Boot está habilitado, el kernel puede rechazar módulos externos sin una firma confiable.

Un síntoma típico es un error al ejecutar:

```bash
sudo modprobe sfh_tablet_switch
```

o mensajes relacionados con claves/firma en:

```bash
sudo dmesg
```

La solución correcta depende de la distribución y de la política de Secure Boot utilizada. Normalmente implica firmar el módulo con una clave aceptada por el sistema o utilizar el mecanismo MOK correspondiente.

No es recomendable desactivar las verificaciones de seguridad sin entender las implicaciones.

---

## Limitaciones

### 1. Dependencia de AMD SFH

El módulo solo funciona si el hardware y el firmware exponen correctamente los datos mediante AMD SFH.

### 2. Hardware probado limitado

La clasificación actual fue diseñada a partir de los modos observados en el equipo utilizado durante el desarrollo.

En otro portátil convertible AMD los valores pueden necesitar ajustes.

### 3. Polling

Actualmente el módulo consulta AMD SFH cada:

```text
200 ms
```

No utiliza una notificación/evento hardware específico de cambio de postura.

El coste es pequeño, pero conceptualmente un mecanismo dirigido por eventos sería preferible si el hardware/kernel proporcionara una interfaz apropiada.

### 4. API interna del kernel

El proyecto utiliza APIs y cabeceras del kernel Linux que pueden cambiar entre versiones.

No existe garantía de compatibilidad binaria entre kernels.

### 5. No gestiona orientación

No genera información de acelerómetro ni eventos de rotación.

### 6. No modifica dispositivos directamente

El módulo no deshabilita por sí mismo:

- teclado;
- touchpad;
- trackpoint;
- stylus;
- pantalla táctil.

Publica `SW_TABLET_MODE` y deja esa política al espacio de usuario.

### 7. Modos desconocidos

Los modos no clasificados devuelven:

```c
-1
```

por lo que el módulo conserva el último estado publicado en lugar de cambiarlo arbitrariamente.

---

## Estructura del código

### `sfh_mode_is_tablet()`

```c
static int sfh_mode_is_tablet(u32 mode)
```

Convierte el `platform_type` bruto en:

```text
1   tablet
0   portátil
-1  ignorar / mantener estado
```

### `report_tablet_state()`

Publica el switch mediante:

```c
input_report_switch(tablet_input, SW_TABLET_MODE, enabled);
input_sync(tablet_input);
```

### `poll_sfh_mode()`

Es el núcleo del funcionamiento:

1. consulta AMD SFH;
2. registra cambios de `platform_type`;
3. clasifica el estado;
4. aplica estabilización;
5. publica el evento si cambia;
6. programa la siguiente consulta.

La reprogramación se realiza con:

```c
schedule_delayed_work(
    &poll_work,
    msecs_to_jiffies(POLL_INTERVAL_MS)
);
```

### Inicialización

```c
static int __init sfh_tablet_switch_init(void)
```

Reserva un dispositivo input:

```c
tablet_input = input_allocate_device();
```

Configura:

```c
tablet_input->name = "AMD SFH Tablet Mode Switch";
tablet_input->phys = "amd-sfh/tablet-mode";
tablet_input->id.bustype = BUS_HOST;
tablet_input->id.vendor = 0x1022;
```

Declara la capacidad:

```c
input_set_capability(
    tablet_input,
    EV_SW,
    SW_TABLET_MODE
);
```

y registra el dispositivo:

```c
input_register_device(tablet_input);
```

---

## Referencias técnicas

- Linux kernel, `include/linux/amd-pmf-io.h` — definición de `amd_sfh_info`, `MT_SRA` y `amd_get_sfh_info()`:
  https://github.com/torvalds/linux/blob/master/include/linux/amd-pmf-io.h
- Linux kernel, códigos de eventos de entrada — `EV_SW` y `SW_TABLET_MODE`:
  https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h
- libinput, documentación de switches:
  https://wayland.freedesktop.org/libinput/doc/latest/switches.html

---

## Licencia

**GNU General Public License v2.0 (GPL-2.0)**.

Incluye una copia completa de la licencia en el archivo:

```text
LICENSE
```

---

## Autor

**vinylo**

Proyecto experimental para proporcionar soporte de modo tablet en convertibles AMD cuyo estado de postura está disponible mediante AMD SFH pero no se presenta correctamente al subsistema de entrada de Linux.
