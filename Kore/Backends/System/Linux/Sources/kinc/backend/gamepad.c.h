#include "gamepad.h"

#include <kinc/input/gamepad.h>

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Backend de gamepads sobre evdev (/dev/input/event*), reemplaza al viejo
// backend joydev (/dev/input/js*). Emite el "layout canónico" del juego, que
// es la numeración sintética del path XInput del backend de Windows:
//
//   botones: 0..3 caras por POSICIÓN física (0=sur, 1=este, 2=oeste, 3=norte),
//            4/5 = hombros, 6/7 = gatillos (valor analógico), 8/9 = back/start,
//            10/11 = click de sticks, 12..15 = dpad (up/down/left/right)
//   ejes:    0/1 = stick izq, 2/3 = stick der (normalizados a [-1,1] con el
//            absinfo real del driver; +derecha / +ABAJO, convención de
//            pantalla), 4/5 = gatillos en [0,1]
//
// Los drivers modernos del kernel (xpad, xpadneo, hid-sony, hid-playstation,
// hid-nintendo, uinput de Steam) convergen todos en la misma tabla de ejes
// (X/Y, RX/RY, Z/RZ o BRAKE/GAS, HAT0 para dpad). La única divergencia real
// son los botones de cara 0x133/0x134: la familia Xbox mapea por etiqueta
// (BTN_X=0x133 es el botón OESTE físico) y el resto por posición (0x133 =
// norte). Se resuelve con el flag xbox_labels por dispositivo.
//
// Dedup con Steam Input: cuando Steam remapea un mando crea un pad virtual
// ("Steam Virtual Gamepad", VID 0x28de) y publica los VID/PID de los mandos
// físicos capturados en SDL_GAMECONTROLLER_IGNORE_DEVICES (mismo mecanismo
// que usan los juegos SDL). Honrar esa variable evita el doble input. Sin
// Steam, la variable no existe y se abre todo — el modo desarrollo nativo
// usa exactamente este mismo código.

#define EVDEV_MAX_PADS 4 // Kha pollea/escucha los índices 0..3

#define EVDEV_BITS_PER_LONG (sizeof(unsigned long) * 8)
#define EVDEV_NLONGS(x) (((x) + EVDEV_BITS_PER_LONG - 1) / EVDEV_BITS_PER_LONG)
#define EVDEV_TEST_BIT(bit, array) (((array)[(bit) / EVDEV_BITS_PER_LONG] >> ((bit) % EVDEV_BITS_PER_LONG)) & 1)

struct EvdevGamepad {
	int fd; // -1 = slot libre
	char devnode[64];
	char name[128];
	char vendor_str[16]; // "vvvv:pppp" para kinc_gamepad_vendor
	uint16_t vid, pid;
	bool xbox_labels; // familia Xbox: 0x133 = oeste físico, 0x134 = norte físico
	struct input_absinfo abs[ABS_HAT0Y + 1];
	bool has_abs[ABS_HAT0Y + 1];
	// Estado para flancos (dpad por hat) y para el resync tras SYN_DROPPED
	int hat_x, hat_y;
	bool btn_state[16];
	float axis_state[6];
	bool pending_resync;
};

static struct EvdevGamepad evdev_pads[EVDEV_MAX_PADS];

struct HIDGamepadUdevHelper {
	struct udev *udevPtr;
	struct udev_monitor *udevMonitorPtr;
	int udevMonitorFD;
};

static struct HIDGamepadUdevHelper udev_helper;

// -------------------------------------------------------------------------
// Tablas de mapeo al layout canónico
// -------------------------------------------------------------------------

static int evdev_map_key(const struct EvdevGamepad *pad, uint16_t code) {
	switch (code) {
	case BTN_SOUTH:
		return 0;
	case BTN_EAST:
		return 1;
	// BTN_NORTH (0x133) es alias de BTN_X y BTN_WEST (0x134) de BTN_Y. La
	// familia Xbox emite por etiqueta (X = oeste físico); los drivers que
	// siguen la spec de gamepad del kernel emiten por posición.
	case BTN_NORTH:
		return pad->xbox_labels ? 2 : 3;
	case BTN_WEST:
		return pad->xbox_labels ? 3 : 2;
	case BTN_TL:
		return 4;
	case BTN_TR:
		return 5;
	case BTN_TL2:
		return 6;
	case BTN_TR2:
		return 7;
	case BTN_SELECT:
		return 8;
	case BTN_START:
		return 9;
	case BTN_THUMBL:
		return 10;
	case BTN_THUMBR:
		return 11;
	case BTN_DPAD_UP:
		return 12;
	case BTN_DPAD_DOWN:
		return 13;
	case BTN_DPAD_LEFT:
		return 14;
	case BTN_DPAD_RIGHT:
		return 15;
	default:
		return -1; // BTN_MODE (guía) y cualquier código desconocido
	}
}

static int evdev_map_abs(uint16_t code) {
	switch (code) {
	case ABS_X:
		return 0;
	case ABS_Y:
		return 1;
	case ABS_RX:
		return 2;
	case ABS_RY:
		return 3;
	case ABS_Z:
	case ABS_BRAKE: // gatillo izq en mandos Xbox por Bluetooth (hid genérico)
		return 4;
	case ABS_RZ:
	case ABS_GAS:
		return 5;
	default:
		return -1;
	}
}

static float evdev_normalize(const struct input_absinfo *info, int value, bool is_trigger) {
	if (info->maximum == info->minimum) {
		return 0.0f;
	}
	float f = (value - info->minimum) / (float)(info->maximum - info->minimum); // 0..1
	if (is_trigger) {
		return f;
	}
	return f * 2.0f - 1.0f;
}

// -------------------------------------------------------------------------
// Emisión con estado (dedup + flancos limpios en el resync)
// -------------------------------------------------------------------------

static void evdev_emit_button(struct EvdevGamepad *pad, int idx, int button, bool down) {
	if (button < 0 || button >= 16 || pad->btn_state[button] == down) {
		return;
	}
	pad->btn_state[button] = down;
	kinc_internal_gamepad_trigger_button(idx, button, down ? 1.0f : 0.0f);
}

static void evdev_emit_axis(struct EvdevGamepad *pad, int idx, int axis, float value) {
	if (axis < 0 || axis >= 6 || pad->axis_state[axis] == value) {
		return;
	}
	pad->axis_state[axis] = value;
	kinc_internal_gamepad_trigger_axis(idx, axis, value);
	// Los gatillos se emiten además como botones 6/7 con su valor analógico,
	// espejo exacto del path XInput de Windows.
	if (axis == 4 || axis == 5) {
		kinc_internal_gamepad_trigger_button(idx, axis + 2, value);
		pad->btn_state[axis + 2] = value >= 0.5f;
	}
}

static void evdev_emit_hat(struct EvdevGamepad *pad, int idx, uint16_t code, int value) {
	if (code == ABS_HAT0X) {
		if (value != pad->hat_x) {
			evdev_emit_button(pad, idx, 14, value < 0);
			evdev_emit_button(pad, idx, 15, value > 0);
			pad->hat_x = value;
		}
	}
	else { // ABS_HAT0Y: +1 = abajo (convención evdev)
		if (value != pad->hat_y) {
			evdev_emit_button(pad, idx, 12, value < 0);
			evdev_emit_button(pad, idx, 13, value > 0);
			pad->hat_y = value;
		}
	}
}

static void evdev_process_abs(struct EvdevGamepad *pad, int idx, uint16_t code, int value) {
	if (code == ABS_HAT0X || code == ABS_HAT0Y) {
		evdev_emit_hat(pad, idx, code, value);
		return;
	}
	int axis = evdev_map_abs(code);
	if (axis < 0 || code > ABS_HAT0Y || !pad->has_abs[code]) {
		return;
	}
	evdev_emit_axis(pad, idx, axis, evdev_normalize(&pad->abs[code], value, axis >= 4));
}

// Tras un SYN_DROPPED (desborde del buffer del kernel) el estado intermedio se
// perdió: re-leer el estado completo con ioctls o quedan botones/ejes pegados.
static void evdev_resync(struct EvdevGamepad *pad, int idx) {
	unsigned long keys[EVDEV_NLONGS(KEY_CNT)];
	memset(keys, 0, sizeof(keys));
	if (ioctl(pad->fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
		static const uint16_t key_codes[] = {BTN_SOUTH, BTN_EAST,  BTN_NORTH,  BTN_WEST,      BTN_TL,        BTN_TR,        BTN_TL2,
		                                     BTN_TR2,   BTN_SELECT, BTN_START, BTN_THUMBL,    BTN_THUMBR,    BTN_DPAD_UP,   BTN_DPAD_DOWN,
		                                     BTN_DPAD_LEFT, BTN_DPAD_RIGHT};
		for (size_t i = 0; i < sizeof(key_codes) / sizeof(key_codes[0]); ++i) {
			evdev_emit_button(pad, idx, evdev_map_key(pad, key_codes[i]), EVDEV_TEST_BIT(key_codes[i], keys));
		}
	}
	for (uint16_t code = 0; code <= ABS_HAT0Y; ++code) {
		if (!pad->has_abs[code]) {
			continue;
		}
		struct input_absinfo info;
		if (ioctl(pad->fd, EVIOCGABS(code), &info) >= 0) {
			evdev_process_abs(pad, idx, code, info.value);
		}
	}
}

// -------------------------------------------------------------------------
// Dedup con Steam Input (SDL_GAMECONTROLLER_IGNORE_DEVICES)
// -------------------------------------------------------------------------

static bool evdev_device_in_env_list(const char *env, uint16_t vid, uint16_t pid) {
	if (env == NULL) {
		return false;
	}
	const char *p = env;
	while (*p != '\0') {
		int v = 0, prod = 0;
		if (sscanf(p, "%i/%i", &v, &prod) == 2 && v == vid && prod == pid) {
			return true;
		}
		const char *comma = strchr(p, ',');
		if (comma == NULL) {
			break;
		}
		p = comma + 1;
	}
	return false;
}

static bool evdev_steam_ignores_device(uint16_t vid, uint16_t pid) {
	const char *except = getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT");
	if (except != NULL && except[0] != '\0') {
		return !evdev_device_in_env_list(except, vid, pid);
	}
	return evdev_device_in_env_list(getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES"), vid, pid);
}

// -------------------------------------------------------------------------
// Detección de la familia Xbox (caras mapeadas por etiqueta)
// -------------------------------------------------------------------------

static bool evdev_str_contains_nocase(const char *haystack, const char *needle) {
	size_t nlen = strlen(needle);
	for (const char *h = haystack; *h != '\0'; ++h) {
		size_t i = 0;
		while (i < nlen && h[i] != '\0' && (h[i] | 0x20) == (needle[i] | 0x20)) {
			++i;
		}
		if (i == nlen) {
			return true;
		}
	}
	return false;
}

static bool evdev_faces_use_xbox_labels(struct udev_device *dev, const char *name, uint16_t vid) {
	// Steam Virtual Gamepad (emula un X-Box 360 pad de xpad)
	if (vid == 0x28de) {
		return true;
	}
	// driver xpad/xpadneo en la cadena de parents (cubre mandos de terceros
	// tipo Logitech F310 cuyo nombre no dice "Xbox")
	struct udev_device *p = dev;
	while (p != NULL) {
		const char *drv = udev_device_get_driver(p);
		if (drv != NULL && (strcmp(drv, "xpad") == 0 || strcmp(drv, "xpadneo") == 0)) {
			return true;
		}
		p = udev_device_get_parent(p); // los parents pertenecen al hijo, no se unref-ean
	}
	if (evdev_str_contains_nocase(name, "xbox") || evdev_str_contains_nocase(name, "x-box")) {
		return true;
	}
	return false;
}

// -------------------------------------------------------------------------
// Apertura / cierre de dispositivos
// -------------------------------------------------------------------------

static void evdev_close_pad(struct EvdevGamepad *pad, int idx) {
	if (pad->fd < 0) {
		return;
	}
	close(pad->fd);
	pad->fd = -1;
	pad->devnode[0] = '\0';
	kinc_internal_gamepad_trigger_disconnect(idx);
}

static void evdev_try_open(struct udev_device *dev, const char *devnode) {
	// ¿ya está abierto? (udev puede re-anunciar un dispositivo existente)
	for (int i = 0; i < EVDEV_MAX_PADS; ++i) {
		if (evdev_pads[i].fd >= 0 && strcmp(evdev_pads[i].devnode, devnode) == 0) {
			return;
		}
	}
	int slot = -1;
	for (int i = 0; i < EVDEV_MAX_PADS; ++i) {
		if (evdev_pads[i].fd < 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return; // 4 mandos ya conectados
	}

	int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return; // sin permisos o desapareció; no es fatal
	}

	// Solo gamepads de verdad: exigir BTN_SOUTH descarta wheels/throttles y
	// pads HID genéricos sin driver (botones en el rango BTN_TRIGGER), que no
	// podrían manejar el juego de todas formas. Bajo Steam, esos llegan como
	// pad virtual xpad y sí pasan este filtro.
	unsigned long keybits[EVDEV_NLONGS(KEY_CNT)];
	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0 || !EVDEV_TEST_BIT(BTN_SOUTH, keybits)) {
		close(fd);
		return;
	}

	struct input_id id;
	memset(&id, 0, sizeof(id));
	ioctl(fd, EVIOCGID, &id);
	if (evdev_steam_ignores_device(id.vendor, id.product)) {
		close(fd); // Steam Input capturó este mando; llegará como pad virtual
		return;
	}

	struct EvdevGamepad *pad = &evdev_pads[slot];
	memset(pad, 0, sizeof(*pad));
	pad->fd = fd;
	strncpy(pad->devnode, devnode, sizeof(pad->devnode) - 1);
	pad->vid = id.vendor;
	pad->pid = id.product;
	snprintf(pad->vendor_str, sizeof(pad->vendor_str), "%04x:%04x", id.vendor, id.product);
	if (ioctl(fd, EVIOCGNAME(sizeof(pad->name)), pad->name) < 0) {
		strncpy(pad->name, "Unknown", sizeof(pad->name) - 1);
	}
	pad->xbox_labels = evdev_faces_use_xbox_labels(dev, pad->name, pad->vid);

	unsigned long absbits[EVDEV_NLONGS(ABS_CNT)];
	memset(absbits, 0, sizeof(absbits));
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	for (uint16_t code = 0; code <= ABS_HAT0Y; ++code) {
		if (EVDEV_TEST_BIT(code, absbits) && ioctl(fd, EVIOCGABS(code), &pad->abs[code]) >= 0) {
			pad->has_abs[code] = true;
		}
	}

	kinc_internal_gamepad_trigger_connect(slot);
	// Publicar el estado inicial (por si se conecta con algo ya pulsado o con
	// ejes fuera de reposo).
	evdev_resync(pad, slot);
}

// -------------------------------------------------------------------------
// udev: enumeración inicial + hot-plug
// -------------------------------------------------------------------------

static void evdev_process_udev_device(struct udev_device *dev) {
	if (dev == NULL) {
		return;
	}
	const char *devnode = udev_device_get_devnode(dev);
	if (devnode != NULL && strstr(devnode, "/event") != NULL) {
		const char *action = udev_device_get_action(dev);
		if (action == NULL) {
			action = "add";
		}
		if (strcmp(action, "add") == 0) {
			// En el add las propiedades de udev están pobladas; filtrar por
			// joystick evita abrir teclados/ratones/touchpads.
			const char *is_joystick = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
			if (is_joystick != NULL && strcmp(is_joystick, "1") == 0) {
				evdev_try_open(dev, devnode);
			}
		}
		else if (strcmp(action, "remove") == 0) {
			// En el remove las propiedades pueden faltar: matchear por devnode.
			for (int i = 0; i < EVDEV_MAX_PADS; ++i) {
				if (evdev_pads[i].fd >= 0 && strcmp(evdev_pads[i].devnode, devnode) == 0) {
					evdev_close_pad(&evdev_pads[i], i);
				}
			}
		}
	}
	udev_device_unref(dev);
}

static void HIDGamepadUdevHelper_init(struct HIDGamepadUdevHelper *helper) {
	struct udev *udevPtrNew = udev_new();

	// enumerar dispositivos ya conectados
	struct udev_enumerate *enumerate = udev_enumerate_new(udevPtrNew);
	udev_enumerate_add_match_subsystem(enumerate, "input");
	udev_enumerate_add_match_property(enumerate, "ID_INPUT_JOYSTICK", "1");
	udev_enumerate_scan_devices(enumerate);

	struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
	struct udev_list_entry *entry;
	udev_list_entry_foreach(entry, devices) {
		const char *path = udev_list_entry_get_name(entry);
		evdev_process_udev_device(udev_device_new_from_syspath(udevPtrNew, path));
	}
	udev_enumerate_unref(enumerate);

	// monitor para hot-plug
	helper->udevMonitorPtr = udev_monitor_new_from_netlink(udevPtrNew, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(helper->udevMonitorPtr, "input", NULL);
	udev_monitor_enable_receiving(helper->udevMonitorPtr);
	helper->udevMonitorFD = udev_monitor_get_fd(helper->udevMonitorPtr);

	helper->udevPtr = udevPtrNew;
}

static void HIDGamepadUdevHelper_update(struct HIDGamepadUdevHelper *helper) {
	// el fd del monitor es no-bloqueante: drenar todos los eventos pendientes
	struct udev_device *dev;
	while ((dev = udev_monitor_receive_device(helper->udevMonitorPtr)) != NULL) {
		evdev_process_udev_device(dev);
	}
}

static void HIDGamepadUdevHelper_close(struct HIDGamepadUdevHelper *helper) {
	udev_unref(helper->udevPtr);
}

// -------------------------------------------------------------------------
// Lectura por frame
// -------------------------------------------------------------------------

static void evdev_update_pad(struct EvdevGamepad *pad, int idx) {
	if (pad->fd < 0) {
		return;
	}
	struct input_event e;
	ssize_t n;
	while ((n = read(pad->fd, &e, sizeof(e))) == (ssize_t)sizeof(e)) {
		if (e.type == EV_KEY) {
			evdev_emit_button(pad, idx, evdev_map_key(pad, e.code), e.value != 0);
		}
		else if (e.type == EV_ABS) {
			evdev_process_abs(pad, idx, e.code, e.value);
		}
		else if (e.type == EV_SYN && e.code == SYN_DROPPED) {
			pad->pending_resync = true;
		}
	}
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
		// El dispositivo se fue sin evento de udev todavía (ENODEV típico)
		evdev_close_pad(pad, idx);
		return;
	}
	if (pad->pending_resync) {
		pad->pending_resync = false;
		evdev_resync(pad, idx);
	}
}

// -------------------------------------------------------------------------
// API del backend (llamada desde system.c.h, firmas intactas)
// -------------------------------------------------------------------------

void kinc_linux_initHIDGamepads() {
	for (int i = 0; i < EVDEV_MAX_PADS; ++i) {
		evdev_pads[i].fd = -1;
		evdev_pads[i].devnode[0] = '\0';
	}
	HIDGamepadUdevHelper_init(&udev_helper);
}

void kinc_linux_updateHIDGamepads() {
	HIDGamepadUdevHelper_update(&udev_helper);
	for (int i = 0; i < EVDEV_MAX_PADS; ++i) {
		evdev_update_pad(&evdev_pads[i], i);
	}
}

void kinc_linux_closeHIDGamepads() {
	for (int i = 0; i < EVDEV_MAX_PADS; ++i) {
		if (evdev_pads[i].fd >= 0) {
			close(evdev_pads[i].fd);
			evdev_pads[i].fd = -1;
		}
	}
	HIDGamepadUdevHelper_close(&udev_helper);
}

const char *kinc_gamepad_vendor(int gamepad) {
	if (gamepad >= 0 && gamepad < EVDEV_MAX_PADS && evdev_pads[gamepad].fd >= 0) {
		return evdev_pads[gamepad].vendor_str;
	}
	return "";
}

const char *kinc_gamepad_product_name(int gamepad) {
	if (gamepad >= 0 && gamepad < EVDEV_MAX_PADS && evdev_pads[gamepad].fd >= 0) {
		return evdev_pads[gamepad].name;
	}
	return "";
}

bool kinc_gamepad_connected(int gamepad) {
	return gamepad >= 0 && gamepad < EVDEV_MAX_PADS && evdev_pads[gamepad].fd >= 0;
}

void kinc_gamepad_rumble(int gamepad, float left, float right) {}
