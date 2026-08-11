/*
   A daemon which maps the side buttons on the Logitech G502 Hero mouse to keyboard modifiers.
   This is done via mapping both the keyboard and mouse to virtual devices, and transmitting the side button events to the keyboard device as modifier key presses.

   Keyboard event device --*               *--> Virtual keyboard device
                            \             /
                             *->  G502  -*
                             *-> Daemon -*
                            /             \
      Mouse event device --*               *--> Virtual mouse device
*/

#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <systemd/sd-device.h>
#include <systemd/sd-event.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

// Look up the keyboard key a mouse button is remapped to.
// Returns 0 when the button should be forwarded to the virtual mouse untouched.
static int mapped_key_for_button(int button_code) {
	switch (button_code) {
#define X(btn, key, btn_scan, key_scan) case btn: return key;
		MODIFIER_BUTTON_MAP
#undef X
		default: return 0;
	}
}

// Look up the keyboard scan code a mouse button's scan code is remapped to.
// Returns 0 when the scan code should be forwarded to the virtual mouse untouched.
static int mapped_scan_for_scan(int mouse_scan) {
	switch (mouse_scan) {
#define X(btn, key, btn_scan, key_scan) case btn_scan: return key_scan;
		MODIFIER_BUTTON_MAP
#undef X
		default: return 0;
	}
}

// Suppress contact bounce from a worn microswitch.
//
// A tired switch chatters around both edges. It can re-close for a few tens of
// milliseconds after you let go, which the host reads as a second click; and it
// can flick open while you are still holding it down, which the host reads as
// the end of a drag. Our G502 produced 22 ms pulses 22-30 ms from the edge.
//
// At the instant a release arrives there is no way to tell a real one from
// chatter, so releases are held back for CLICK_DEBOUNCE_MS. If the button goes
// down again inside that window the release is cancelled and the press is
// swallowed - as far as the host is concerned the button never came up, which
// is the truth in both cases. Presses only get through when the host has
// already seen the matching release.
//
// Dropping the press instead of deferring the release does fix double clicks,
// but it breaks drags: the spurious release goes out, the recovery press is
// eaten as bounce, and the button is left stuck up.
//
// The cost is up to CLICK_DEBOUNCE_MS of latency on release. Presses are never
// delayed. The MSC_SCAN that shares a frame with a deferred or swallowed button
// is still forwarded; libinput only reads a scancode as an annotation on a key
// event in the same frame, so an orphaned one is ignored.
#define DEBOUNCE_TRACKED_BUTTONS 16

typedef struct {
	struct input_event ev; // The held-back release, replayed verbatim
	long long due_us;      // When it should be emitted
	int active;
} pending_release_t;

// Only ever touched by the mouse thread.
static pending_release_t pending_releases[DEBOUNCE_TRACKED_BUTTONS];

static long long event_time_us(const struct input_event* ev) {
	return (long long)ev->time.tv_sec * 1000000LL + (long long)ev->time.tv_usec;
}

static long long now_us(void) {
	struct timespec ts;
	// evdev stamps events on CLOCK_REALTIME, so compare against the same clock
	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000LL;
}

// Index into pending_releases, or -1 for anything we do not debounce.
static int debounce_slot(const struct input_event* ev) {
	if (CLICK_DEBOUNCE_MS <= 0) {
		return -1;
	}
	int slot = ev->code - BTN_MOUSE;
	return (slot >= 0 && slot < DEBOUNCE_TRACKED_BUTTONS) ? slot : -1;
}

// Deadline of the release due out first, or -1 when none are held back.
static long long next_release_due_us(void) {
	long long soonest = -1;
	for (int i = 0; i < DEBOUNCE_TRACKED_BUTTONS; i++) {
		if (!pending_releases[i].active) {
			continue;
		}
		if (soonest < 0 || pending_releases[i].due_us < soonest) {
			soonest = pending_releases[i].due_us;
		}
	}
	return soonest;
}

// Helper function to get errno string
static const char* get_errno_name(int err) {
	switch (err) {
		case EBADF: return "EBADF (Bad file descriptor)";
		case ENODEV: return "ENODEV (No such device)";
		case EINTR: return "EINTR (Interrupted)";
		case EIO: return "EIO (I/O error)";
		case EAGAIN: return "EAGAIN (Would block)";
		case ENOENT: return "ENOENT (No such file)";
		case EACCES: return "EACCES (Permission denied)";
		default: return "UNKNOWN";
	}
}

// Helper function to check if file descriptor is valid
static int is_fd_valid(int fd) {
	return fcntl(fd, F_GETFD) >= 0;
}

// The mice from config.h, in the order they should be tried.
static const struct {
	const char* vendor;
	const char* model;
	const char* label;
} known_mice[] = {
#define X(vendor, model, label) { vendor, model, label },
	MOUSE_DEVICES
#undef X
};
#define KNOWN_MOUSE_COUNT (sizeof(known_mice) / sizeof(known_mice[0]))

// Find the event node of the highest-priority configured mouse that is present.
//
// Matching is on the input device's own id/vendor and id/product sysattrs rather
// than on udev's ID_USB_VENDOR_ID / ID_MODEL_ID properties. Those properties only
// exist for USB devices, so matching on them would find the mouse over the cable
// and the 2.4GHz dongle but never over Bluetooth, where the same mouse arrives on
// the bluetooth bus with no USB properties at all. The sysattrs are populated for
// every bus.
//
// ID_INPUT_MOUSE picks the pointer node out of the several a gaming mouse
// presents under one ID - the Basilisk V3 Pro has four, two of them keyboards.
//
// Returns a malloc'd path, or NULL when no configured mouse is present.
// Position of a device in MOUSE_DEVICES, or KNOWN_MOUSE_COUNT if not listed.
// Lower is better: the order in config.h is a preference order.
static size_t rank_for_device(sd_device* device) {
	sd_device *parent = NULL;
	const char* vendor = NULL;
	const char* product = NULL;
	if (sd_device_get_parent(device, &parent) < 0 ||
	    sd_device_get_sysattr_value(parent, "id/vendor", &vendor) < 0 ||
	    sd_device_get_sysattr_value(parent, "id/product", &product) < 0) {
		return KNOWN_MOUSE_COUNT;
	}
	for (size_t i = 0; i < KNOWN_MOUSE_COUNT; i++) {
		if (strcasecmp(vendor, known_mice[i].vendor) == 0 &&
		    strcasecmp(product, known_mice[i].model) == 0) {
			return i;
		}
	}
	return KNOWN_MOUSE_COUNT;
}

static char* find_mouse_device(size_t* out_rank) {
	struct sd_device_enumerator *enumerator = NULL;
	if (sd_device_enumerator_new(&enumerator) < 0) {
		fprintf(stderr, "Failed to create device enumerator\n");
		return NULL;
	}

	sd_device_enumerator_add_match_subsystem(enumerator, "input", 1);
	sd_device_enumerator_add_match_sysname(enumerator, "event*");
	sd_device_enumerator_add_match_property(enumerator, "ID_INPUT_MOUSE", "1");

	char* best = NULL;
	const char* best_label = NULL;
	size_t best_rank = KNOWN_MOUSE_COUNT;

	for (sd_device *device = sd_device_enumerator_get_device_first(enumerator);
	     device;
	     device = sd_device_enumerator_get_device_next(enumerator)) {
		const char* syspath = NULL;
		if (sd_device_get_syspath(device, &syspath) < 0) {
			continue;
		}
		// Our own virtual mouse also has ID_INPUT_MOUSE.
		if (strncmp(syspath, "/sys/devices/virtual/", 21) == 0) {
			continue;
		}

		// The order in MOUSE_DEVICES decides which mouse wins when several are
		// connected. Ties keep the first node enumerated.
		size_t rank = rank_for_device(device);
		if (rank >= best_rank) {
			continue;
		}
		const char* devnode = NULL;
		if (sd_device_get_devname(device, &devnode) < 0) {
			continue;
		}
		free(best);
		best = strdup(devnode);
		best_rank = rank;
		best_label = known_mice[rank].label;
	}

	sd_device_enumerator_unref(enumerator);

	if (best) {
		fprintf(stderr, "Mouse found: %s (%s:%s) at %s\n", best_label,
			known_mice[best_rank].vendor, known_mice[best_rank].model, best);
		if (out_rank) {
			*out_rank = best_rank;
		}
		return best;
	}

	fprintf(stderr, "No configured mouse is connected. Looked for:\n");
	for (size_t i = 0; i < KNOWN_MOUSE_COUNT; i++) {
		fprintf(stderr, "  %s:%s  %s\n", known_mice[i].vendor,
			known_mice[i].model, known_mice[i].label);
	}
	fprintf(stderr, "Add yours to MOUSE_DEVICES in config.h; `lsusb` prints the IDs,\n");
	fprintf(stderr, "or read id/vendor and id/product under /sys/class/input/eventN/device/.\n");
	return NULL;
}

// A keyboard the daemon has taken over.
// The syspath is the stable identity used to find the device again after a
// disconnect; the /dev/input/eventN number can change across replugs.
#define MAX_KEYBOARDS 16
typedef struct {
	char syspath[256];
	char devnode[64];
	char name[128];
	int active; // 0 once its thread has given up, freeing the slot for reuse
} keyboard_device_t;

// Populate a keyboard record from a udev device.
static void fill_keyboard(sd_device* device, const char* syspath, const char* devnode,
                          keyboard_device_t* kb) {
	snprintf(kb->syspath, sizeof(kb->syspath), "%s", syspath);
	snprintf(kb->devnode, sizeof(kb->devnode), "%s", devnode);

	// NAME is quoted in udev's property list; strip the quotes for logging.
	const char* name = NULL;
	if (sd_device_get_property_value(device, "NAME", &name) < 0 || !name) {
		sd_device *parent = NULL;
		if (sd_device_get_parent(device, &parent) >= 0) {
			sd_device_get_sysattr_value(parent, "name", &name);
		}
	}
	if (name) {
		size_t len = strlen(name);
		if (len >= 2 && name[0] == '"' && name[len - 1] == '"') {
			snprintf(kb->name, sizeof(kb->name), "%.*s", (int)(len - 2), name + 1);
		} else {
			snprintf(kb->name, sizeof(kb->name), "%s", name);
		}
	} else {
		snprintf(kb->name, sizeof(kb->name), "%s", devnode);
	}
}

// Find every real keyboard on the system.
//
// Keyboards are identified by udev's ID_INPUT_KEYBOARD tag rather than by hardcoded
// vendor and model IDs, so this works on any machine without configuration. Devices
// under /sys/devices/virtual are skipped: those are uinput devices, including the
// virtual keyboard this daemon itself creates, and grabbing our own output would
// feed events straight back into the buffer.
static int find_keyboard_devices(keyboard_device_t* out, int max) {
	struct sd_device_enumerator *enumerator = NULL;
	if (sd_device_enumerator_new(&enumerator) < 0) {
		fprintf(stderr, "Failed to create keyboard enumerator\n");
		return 0;
	}

	sd_device_enumerator_add_match_subsystem(enumerator, "input", 1);
	sd_device_enumerator_add_match_sysname(enumerator, "event*");
	sd_device_enumerator_add_match_property(enumerator, "ID_INPUT_KEYBOARD", "1");

	int count = 0;
	for (sd_device *device = sd_device_enumerator_get_device_first(enumerator);
	     device && count < max;
	     device = sd_device_enumerator_get_device_next(enumerator)) {
		const char* syspath = NULL;
		const char* devnode = NULL;
		if (sd_device_get_syspath(device, &syspath) < 0 ||
		    sd_device_get_devname(device, &devnode) < 0) {
			continue;
		}
		// Skip uinput devices, ours included.
		if (strncmp(syspath, "/sys/devices/virtual/", 21) == 0) {
			continue;
		}

		fill_keyboard(device, syspath, devnode, &out[count]);
		count++;
	}

	sd_device_enumerator_unref(enumerator);
	return count;
}

// Re-resolve a keyboard's device node after a disconnect, matching on syspath.
// Returns 0 if the device is back, -1 if it is still gone.
static int refresh_keyboard_devnode(keyboard_device_t* kb) {
	keyboard_device_t found[MAX_KEYBOARDS];
	int n = find_keyboard_devices(found, MAX_KEYBOARDS);
	for (int i = 0; i < n; i++) {
		if (strcmp(found[i].syspath, kb->syspath) == 0) {
			snprintf(kb->devnode, sizeof(kb->devnode), "%s", found[i].devnode);
			return 0;
		}
	}
	return -1;
}

// Keyboards the daemon has taken over, and the thread reading each one.
//
// Entries are only ever appended, never moved or removed, so the pointer handed
// to a running thread stays valid as hotplugged keyboards are added alongside it.
static keyboard_device_t keyboards[MAX_KEYBOARDS];
static pthread_t keyboard_threads[MAX_KEYBOARDS];
static int keyboard_count = 0;
static pthread_mutex_t keyboards_mutex = PTHREAD_MUTEX_INITIALIZER;

// Helper function to open and grab an input device
static int open_and_grab_device(const char* device_path, const char* device_name) {
	int fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s device: %s\n", device_name, device_path);
		return -1;
	}
	
	if (ioctl(fd, EVIOCGRAB, 1) < 0) {
		fprintf(stderr, "Failed to grab %s device\n", device_name);
		close(fd);
		return -1;
	}
	
	return fd;
}

// Rank of the mouse currently held, so the udev monitor can tell whether
// something better has just been connected. KNOWN_MOUSE_COUNT means none.
static _Atomic size_t current_mouse_rank = KNOWN_MOUSE_COUNT;

// Wakes the mouse thread out of poll() when it should re-pick its device.
// Without it, a blocked reader would keep an inferior mouse until that mouse
// produced an error - which for a working mouse is never.
static int mouse_wakeup_pipe[2] = {-1, -1};

// Helper function to find, open and grab the mouse
static int find_open_and_grab_mouse(void) {
	size_t rank = KNOWN_MOUSE_COUNT;
	char* device_path = find_mouse_device(&rank);
	if (!device_path) {
		return -1;
	}
	
	int fd = open_and_grab_device(device_path, "mouse");
	free(device_path);
	if (fd >= 0) {
		current_mouse_rank = rank;
	}
	
	return fd;
}

// Helper function to release and close device
static void release_and_close_device(int fd, const char* device_name) {
	if (fd >= 0) {
		ioctl(fd, EVIOCGRAB, 0);
		close(fd);
		fprintf(stderr, "Released and closed %s device (fd=%d)\n", device_name, fd);
	}
}

// Block until a configured mouse shows up. The daemon is useful the moment one
// is plugged in, so starting without one is a wait rather than an error.
static int wait_for_mouse(void) {
	for (int attempt = 0; ; attempt++) {
		int fd = find_open_and_grab_mouse();
		if (fd >= 0) {
			return fd;
		}
		if (attempt == 0) {
			fprintf(stderr, "Waiting for a configured mouse to be connected...\n");
		}
		sleep(2);
	}
}

// Helper function to reopen the mouse with retry logic.
// The search runs again from scratch, so a wireless mouse that comes back on a
// different model ID - switching between dongle and Bluetooth, say - is picked
// up as long as both IDs are listed in MOUSE_DEVICES.
static int reopen_mouse(int* fd) {
	fprintf(stderr, "mouse fd appears invalid, attempting to reopen device\n");
	
	// Release and close old fd
	release_and_close_device(*fd, "mouse");
	*fd = -1;
	
	// Wait a bit before reopening
	sleep(1);
	
	// Try to find and reopen device
	*fd = find_open_and_grab_mouse();
	if (*fd < 0) {
		fprintf(stderr, "Failed to reopen mouse device, will retry\n");
		return -1;
	}
	
	fprintf(stderr, "Successfully reopened and grabbed mouse device\n");
	return 0;
}

// Shared variables for inter-process communication
sem_t kb_event_sem;

// Ring buffer for keyboard events
// The indices are atomic for weakly ordered architectures (e.g. ARM)
#define EVENT_BUFFER_SIZE (1<<18)
pthread_mutex_t kb_buffer_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex to protect buffer access
struct input_event kb_event_buffer[EVENT_BUFFER_SIZE];
_Atomic size_t head = 0; // Keyboard INPUT thread (write index)
_Atomic size_t tail = 0; // Keyboard OUTPUT thread (read index)

// Clear keyboard buffer by abandoning buffered events (called from INPUT thread)
static void clear_keyboard_buffer(void) {
	pthread_mutex_lock(&kb_buffer_mutex);
	{
		// Jump head forward to abandon buffered events
		// Safe because we only READ tail and WRITE head (which we own)
		head = tail;
		
		// Drain semaphore of stale posts
		int sem_value;
		sem_getvalue(&kb_event_sem, &sem_value);
		while (sem_value > 0) {
			sem_trywait(&kb_event_sem);
			sem_getvalue(&kb_event_sem, &sem_value);
		}
	}
	pthread_mutex_unlock(&kb_buffer_mutex);
	
	fprintf(stderr, "Keyboard event buffer cleared\n");
}

// Function to send an input event to the keyboard event buffer for the OUTPUT thread to process
static void send_input_event_to_keyboard(struct input_event* ev) {
	pthread_mutex_lock(&kb_buffer_mutex);
	{
		// Add the event to the ring buffer
		size_t next_head = (head + 1) % EVENT_BUFFER_SIZE;
		int sem_value;
		sem_getvalue(&kb_event_sem, &sem_value);
		
		if (next_head == tail)
		{
			// Buffer is full, this should never happen
			fprintf(stderr, "Keyboard event buffer overflow detected (head=%zu, tail=%zu, sem_value=%d)\n", head, tail, sem_value);
			fprintf(stderr, "Keyboard event buffer full, (type=%d, code=%d, value=%d)\n",
				ev->type, ev->code, ev->value);
			exit(1); 
			return;
		}

		kb_event_buffer[head] = *ev;
		head = next_head;
	}
	pthread_mutex_unlock(&kb_buffer_mutex);

	// Signal that a new event is available
	if (sem_post(&kb_event_sem) != 0)
	{
		fprintf(stderr, "sem_post failed when sending keyboard event\n");
	}
}

// Thread that will handle mouse INPUT and OUTPUT events
typedef struct {
	const int out_fd;
} mouse_thread_args_t;
// Send a mouse button event to whichever virtual device it belongs on.
static void emit_button_event(int out_fd, const struct input_event* ev) {
	struct input_event out = *ev;
	int mapped_key = mapped_key_for_button(out.code);
	if (mapped_key) {
		// Convert side buttons to modifier keys
		out.code = mapped_key;
		send_input_event_to_keyboard(&out);
	} else {
		ssize_t written = write(out_fd, &out, sizeof(out));
		if (written != (ssize_t)sizeof(out)) {
			fprintf(stderr, "Failed to write mouse button event: wrote %zd/%zu bytes\n",
				written, sizeof(out));
		}
	}
}

// Close the frame a deferred release was emitted in. Mirrors the EV_SYN case
// in the read loop, which feeds both virtual devices.
static void emit_syn_report(int out_fd, const struct input_event* ref) {
	struct input_event syn = *ref;
	syn.type = EV_SYN;
	syn.code = SYN_REPORT;
	syn.value = 0;

	send_input_event_to_keyboard(&syn);
	ssize_t written = write(out_fd, &syn, sizeof(syn));
	if (written != (ssize_t)sizeof(syn)) {
		fprintf(stderr, "Failed to write deferred SYN event: wrote %zd/%zu bytes\n",
			written, sizeof(syn));
	}
}

// Emit any held-back releases whose window has expired. Pass force to flush
// everything regardless, so a disappearing device cannot leave a button down.
static void flush_due_releases(int out_fd, int force) {
	long long now = now_us();
	for (int i = 0; i < DEBOUNCE_TRACKED_BUTTONS; i++) {
		pending_release_t* pending = &pending_releases[i];
		if (!pending->active || (!force && now < pending->due_us)) {
			continue;
		}
		pending->active = 0;
		emit_button_event(out_fd, &pending->ev);
		emit_syn_report(out_fd, &pending->ev);
	}
}

// Returns 1 when the event has been dealt with and must not be forwarded.
static int debounce_button_event(const struct input_event* ev) {
	int slot = debounce_slot(ev);
	if (slot < 0) {
		return 0;
	}
	pending_release_t* pending = &pending_releases[slot];

	if (ev->value == 0) {
		// Hold it back; this may yet turn out to be chatter
		pending->ev = *ev;
		pending->due_us = event_time_us(ev) + (long long)CLICK_DEBOUNCE_MS * 1000LL;
		pending->active = 1;
		return 1;
	}

	if (ev->value == 1 && pending->active) {
		// Back down inside the window, so it never really came up
		long long released_us = pending->due_us - (long long)CLICK_DEBOUNCE_MS * 1000LL;
		fprintf(stderr, "Debounce: swallowed bounce on button 0x%x, %lld ms after release\n",
			ev->code, (event_time_us(ev) - released_us) / 1000);
		pending->active = 0;
		return 1;
	}

	return 0;
}

void* mouse_thread_io_func(void* args_void) {
	mouse_thread_args_t* args = (mouse_thread_args_t*)args_void;

	// Find, open and grab the mouse event device
	int mouse_fd = wait_for_mouse();

	// Accumulators for scaled movement
	float accum_x = 0.0;
	float accum_y = 0.0;
	
	const size_t ev_size = sizeof(struct input_event);

	// Read events in a loop
	int consecutive_failures = 0;
	while (1) {
		// Wait for input, waking early if a held-back release falls due.
		// A blocking read would sit here indefinitely on an idle mouse and
		// strand the release that ends a click.
		int timeout_ms = -1;
		long long due_us = next_release_due_us();
		if (due_us >= 0) {
			long long remaining_us = due_us - now_us();
			timeout_ms = remaining_us > 0 ? (int)((remaining_us + 999) / 1000) : 0;
		}

		struct pollfd pfds[2] = {
			{ .fd = mouse_fd,             .events = POLLIN, .revents = 0 },
			{ .fd = mouse_wakeup_pipe[0], .events = POLLIN, .revents = 0 },
		};
		int nfds = mouse_wakeup_pipe[0] >= 0 ? 2 : 1;
		int ready = poll(pfds, nfds, timeout_ms);
		if (ready < 0 && errno == EINTR) {
			continue;
		}

		// A better mouse was connected; drop this one and re-pick.
		if (nfds == 2 && (pfds[1].revents & POLLIN)) {
			char drain[64];
			while (read(mouse_wakeup_pipe[0], drain, sizeof(drain)) > 0) {
				// Coalesce: several devices can appear at once
			}
			fprintf(stderr, "A preferred mouse was connected, switching\n");
			flush_due_releases(args->out_fd, 1);
			release_and_close_device(mouse_fd, "mouse");
			mouse_fd = wait_for_mouse();
			accum_x = 0.0;
			accum_y = 0.0;
			continue;
		}

		// A press arriving in this same wake-up is still inside its window,
		// so it gets to cancel the release before this flush can see it.
		flush_due_releases(args->out_fd, 0);

		if (ready == 0) {
			continue; // Timed out: nothing to read, releases already flushed
		}

		// Read an event
		struct input_event ev = {0};
		ssize_t n = ready < 0 ? -1 : read(mouse_fd, &ev, ev_size);
		if (n != ev_size) {
			int err = errno;
			fprintf(stderr, "Failed to read mouse event: read returned %zd bytes, errno=%d (%s)\n",
				n, err, get_errno_name(err));
			fprintf(stderr, "  Mouse fd=%d, is_valid=%d, consecutive_failures=%d\n", 
				mouse_fd, is_fd_valid(mouse_fd), consecutive_failures);
			
			// Let go of anything held back before the device goes away,
			// or a button caught mid-window would stay down forever
			flush_due_releases(args->out_fd, 1);

			// Always try to reopen on any read error
			if (reopen_mouse(&mouse_fd) == 0) {
				consecutive_failures = 0;
				// Reset accumulators on reconnect
				accum_x = 0.0;
				accum_y = 0.0;
			} else {
				consecutive_failures++;
				sleep(5); // Wait before retrying
			}
			continue;
		}
		
		// Reset failure counter on successful read
		consecutive_failures = 0;

		switch (ev.type)
		{
		case EV_KEY:
		{
			// Defer releases and swallow bounce before anything is forwarded
			if (debounce_button_event(&ev)) {
				break;
			}
			emit_button_event(args->out_fd, &ev);
		} break;
		case EV_REL:
		{
			// Scale mouse movement
			if (ev.code == REL_X) {
				accum_x += ev.value * DPI_SCALE;
				int int_move = (int)roundf(accum_x);
				accum_x -= int_move;
				ev.value = int_move;
			} else if (ev.code == REL_Y) {
				accum_y += ev.value * DPI_SCALE;
				int int_move = (int)roundf(accum_y);
				accum_y -= int_move;
				ev.value = int_move;
			}
			ssize_t written = write(args->out_fd, &ev, ev_size);
			if (written != ev_size) {
				fprintf(stderr, "Failed to write mouse REL event: wrote %zd/%zu bytes\n", written, ev_size);
			}
		} break;
		case EV_MSC:
		{
			if (ev.code == MSC_SCAN) {
				int mapped_scan = mapped_scan_for_scan(ev.value);
				if (mapped_scan) {
					ev.value = mapped_scan;
					send_input_event_to_keyboard(&ev);
				} else {
					ssize_t written = write(args->out_fd, &ev, ev_size);
					if (written != ev_size) {
						fprintf(stderr, "Failed to write mouse MSC scan event: wrote %zd/%zu bytes\n", written, ev_size);
					}
				}
			} else {
				ssize_t written = write(args->out_fd, &ev, ev_size);
				if (written != ev_size) {
					fprintf(stderr, "Failed to write mouse MSC event: wrote %zd/%zu bytes\n", written, ev_size);
				}
			}
		} break;
		case EV_SYN:
		{
			// Write event to both buffers
			send_input_event_to_keyboard(&ev);
			ssize_t written = write(args->out_fd, &ev, ev_size);
			if (written != ev_size) {
				fprintf(stderr, "Failed to write mouse SYN event: wrote %zd/%zu bytes\n", written, ev_size);
			}
		} break;
		default:
		{
			// Forward other events to mouse
			ssize_t written = write(args->out_fd, &ev, ev_size);
			if (written != ev_size) {
				fprintf(stderr, "Failed to write mouse event (type=%d, code=%d): wrote %zd/%zu bytes\n", 
					ev.type, ev.code, written, ev_size);
			}
		} break;
		}
	}

	// Release and close the mouse device
	release_and_close_device(mouse_fd, "mouse");

	pthread_exit(NULL);
}


// Thread that will handle INPUT keyboard events.
// One of these runs per physical keyboard; they all feed the same event buffer.
// How long a vanished keyboard is waited for before its thread gives up, in
// one-second attempts. A device that is unplugged and replugged comes back under
// a *new* syspath, so refresh_keyboard_devnode() can never find it again and the
// udev monitor adds the replacement separately. Retrying forever just spins.
#define KEYBOARD_REOPEN_ATTEMPTS 30

// Mark a keyboard's slot free so a later device can reuse it.
static void release_keyboard_slot(keyboard_device_t* kb) {
	pthread_mutex_lock(&keyboards_mutex);
	kb->active = 0;
	pthread_mutex_unlock(&keyboards_mutex);
}

void* keyboard_process_i(void* args_void) {
	keyboard_device_t* kb = (keyboard_device_t*)args_void;

	// Open and grab the keyboard event device
	int kb_fd = open_and_grab_device(kb->devnode, kb->name);
	if (kb_fd < 0) {
		release_keyboard_slot(kb);
		pthread_exit(NULL);
	}
	fprintf(stderr, "Grabbed keyboard: %s (%s)\n", kb->name, kb->devnode);

	// Read events in a loop
	while (1) {
		struct input_event ev;
		ssize_t n = read(kb_fd, &ev, sizeof(ev));
		if (n == sizeof(ev)) {
			send_input_event_to_keyboard(&ev);
			continue;
		}

		int err = errno;
		fprintf(stderr, "Failed to read from keyboard %s: read returned %zd bytes, errno=%d (%s)\n",
			kb->name, n, err, get_errno_name(err));

		// Clear the buffer before reopening to avoid stale events
		clear_keyboard_buffer();
		release_and_close_device(kb_fd, kb->name);
		kb_fd = -1;

		for (int attempt = 1; kb_fd < 0; attempt++) {
			sleep(1);
			if (refresh_keyboard_devnode(kb) == 0) {
				kb_fd = open_and_grab_device(kb->devnode, kb->name);
			}
			if (kb_fd >= 0) {
				fprintf(stderr, "Reopened keyboard: %s (%s)\n", kb->name, kb->devnode);
				break;
			}
			if (attempt >= KEYBOARD_REOPEN_ATTEMPTS) {
				fprintf(stderr, "Keyboard %s did not come back after %d attempts; releasing it\n",
					kb->name, attempt);
				release_keyboard_slot(kb);
				pthread_exit(NULL);
			}
		}
	}

	pthread_exit(NULL);
}

// Thread that will handle OUTPUT keyboard events
typedef struct {
	const int out_fd;
} keyboard_output_thread_args_t;
void* keyboard_process_o(void* args_void) {
	keyboard_output_thread_args_t* args = (keyboard_output_thread_args_t*)args_void;

	// Write events in a loop
	while (1) {
		// Wait for an event to be available
		if (sem_wait(&kb_event_sem) != 0)
		{
			fprintf(stderr, "sem_wait failed in keyboard output thread\n");
			continue;
		}
		// int sem_value;
		// sem_getvalue(&kb_event_sem, &sem_value);
		// fprintf(stderr, "Keyboard output thread woke up, semaphore value=%d\n", sem_value);

		// Get the next event from the buffer
		size_t current_tail = atomic_load(&tail);
		struct input_event ev = kb_event_buffer[current_tail];
		atomic_store(&tail, (current_tail + 1) % EVENT_BUFFER_SIZE);

		// Forward the event to the virtual keyboard device
		ssize_t written = write(args->out_fd, &ev, sizeof(ev));
		if (written != sizeof(ev)) {
			int err = errno;
			fprintf(stderr, "Failed to write keyboard event (type=%d, code=%d, value=%d): wrote %zd/%zu bytes, errno=%d (%s)\n",
				ev.type, ev.code, ev.value, written, sizeof(ev), err, get_errno_name(err));
		}
	}

	pthread_exit(NULL);
}



// Take over a keyboard and start reading it, unless we already are.
// Safe to call from the udev monitor thread while other keyboard threads run.
static void add_keyboard(const keyboard_device_t* found) {
	pthread_mutex_lock(&keyboards_mutex);

	int slot = -1;
	for (int i = 0; i < keyboard_count; i++) {
		if (keyboards[i].active && strcmp(keyboards[i].syspath, found->syspath) == 0) {
			pthread_mutex_unlock(&keyboards_mutex);
			return; // Already ours
		}
		if (!keyboards[i].active && slot < 0) {
			slot = i; // A thread that gave up left this free
		}
	}
	if (slot < 0) {
		if (keyboard_count >= MAX_KEYBOARDS) {
			fprintf(stderr, "Ignoring keyboard %s: already tracking %d\n",
				found->name, MAX_KEYBOARDS);
			pthread_mutex_unlock(&keyboards_mutex);
			return;
		}
		slot = keyboard_count++;
	}

	keyboards[slot] = *found;
	keyboards[slot].active = 1;
	if (pthread_create(&keyboard_threads[slot], NULL,
	                   keyboard_process_i, &keyboards[slot]) != 0) {
		fprintf(stderr, "Failed to create input thread for keyboard %s\n", found->name);
		keyboards[slot].active = 0;
		pthread_mutex_unlock(&keyboards_mutex);
		return;
	}
	// Detached: these threads are never joined, and slots get reused.
	pthread_detach(keyboard_threads[slot]);

	pthread_mutex_unlock(&keyboards_mutex);
}

// udev callback: pick up keyboards plugged in after startup.
static int on_input_device_event(sd_device_monitor* monitor, sd_device* device, void* userdata) {
	(void)monitor;
	(void)userdata;

	sd_device_action_t action;
	if (sd_device_get_action(device, &action) < 0 || action != SD_DEVICE_ADD) {
		return 0; // Removals are handled by the keyboard threads themselves
	}

	const char* sysname = NULL;
	if (sd_device_get_sysname(device, &sysname) < 0 || strncmp(sysname, "event", 5) != 0) {
		return 0;
	}

	// A higher-priority mouse than the one we hold: ask the mouse thread to
	// re-pick. Falling back to a lesser mouse is expected when the preferred one
	// is unplugged; staying on it afterwards is not.
	const char* mouse = NULL;
	if (sd_device_get_property_value(device, "ID_INPUT_MOUSE", &mouse) >= 0 &&
	    strcmp(mouse, "1") == 0) {
		if (rank_for_device(device) < current_mouse_rank && mouse_wakeup_pipe[1] >= 0) {
			ssize_t ignored = write(mouse_wakeup_pipe[1], "1", 1);
			(void)ignored;
		}
	}

	const char* keyboard = NULL;
	if (sd_device_get_property_value(device, "ID_INPUT_KEYBOARD", &keyboard) < 0 ||
	    strcmp(keyboard, "1") != 0) {
		return 0;
	}

	const char* syspath = NULL;
	const char* devnode = NULL;
	if (sd_device_get_syspath(device, &syspath) < 0 ||
	    sd_device_get_devname(device, &devnode) < 0) {
		return 0;
	}
	// Never grab our own output; see find_keyboard_devices().
	if (strncmp(syspath, "/sys/devices/virtual/", 21) == 0) {
		return 0;
	}

	keyboard_device_t kb;
	memset(&kb, 0, sizeof(kb));
	fill_keyboard(device, syspath, devnode, &kb);
	fprintf(stderr, "Keyboard connected: %s (%s)\n", kb.name, kb.devnode);
	add_keyboard(&kb);
	return 0;
}

// Watch udev for input devices appearing.
//
// Enumeration only happens once at startup, so without this a keyboard plugged
// in later is never grabbed and its keys bypass the virtual keyboard entirely -
// which for a mouse that sends keystrokes means its side buttons stop working
// as modifiers until the daemon is restarted.
static void* device_monitor_thread(void* unused) {
	(void)unused;

	sd_event* event = NULL;
	sd_device_monitor* monitor = NULL;

	if (sd_event_new(&event) < 0) {
		fprintf(stderr, "Failed to create event loop for device monitor\n");
		pthread_exit(NULL);
	}
	if (sd_device_monitor_new(&monitor) < 0) {
		fprintf(stderr, "Failed to create device monitor\n");
		sd_event_unref(event);
		pthread_exit(NULL);
	}
	if (sd_device_monitor_filter_add_match_subsystem_devtype(monitor, "input", NULL) < 0 ||
	    sd_device_monitor_attach_event(monitor, event) < 0 ||
	    sd_device_monitor_start(monitor, on_input_device_event, NULL) < 0) {
		fprintf(stderr, "Failed to start device monitor\n");
		sd_device_monitor_unref(monitor);
		sd_event_unref(event);
		pthread_exit(NULL);
	}

	fprintf(stderr, "Watching for input devices\n");
	sd_event_loop(event);

	sd_device_monitor_unref(monitor);
	sd_event_unref(event);
	pthread_exit(NULL);
}

int main()
{
	fprintf(stderr, "Starting G502 daemon...\n");
	sleep(1);

	// Not fatal if absent: the mouse thread waits for one to be connected.
	free(find_mouse_device(NULL));

	// Non-blocking so the mouse thread can drain it without stalling.
	if (pipe2(mouse_wakeup_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		fprintf(stderr, "Failed to create mouse wakeup pipe; the daemon will not "
		                "switch to a preferred mouse until the current one errors\n");
		mouse_wakeup_pipe[0] = mouse_wakeup_pipe[1] = -1;
	}

	// Discover every keyboard on the system. This must happen before the virtual
	// keyboard is created, otherwise we would find and then grab our own output.
	keyboard_device_t found[MAX_KEYBOARDS];
	int found_count = find_keyboard_devices(found, MAX_KEYBOARDS);
	fprintf(stderr, "Found %d keyboard%s:\n", found_count, found_count == 1 ? "" : "s");
	for (int i = 0; i < found_count; i++) {
		fprintf(stderr, "  %s (%s)\n", found[i].name, found[i].devnode);
	}

	// Now we have verified the devices exist, create the virtual devices
	
	// Create virtual G502 device
	int v_g502_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (v_g502_fd < 0) {
		fprintf(stderr, "Failed to open /dev/uinput for virtual G502\n");
		return 1;
	}
	// Enable button events.
	// uinput silently discards any event whose code was not declared here, so the
	// virtual mouse must advertise every button the real one can report. The G502
	// descriptor declares 16 buttons, which the kernel maps to BTN_MOUSE + (n - 1),
	// i.e. BTN_LEFT (0x110) through 0x11f. Declaring the whole range means a button
	// that is later rebound in the onboard profile starts working without a rebuild.
	if (ioctl(v_g502_fd, UI_SET_EVBIT, EV_KEY) < 0) {
		fprintf(stderr, "Failed to set virtual G502 event bit\n");
		close(v_g502_fd);
		return 1;
	}
	for (int code = BTN_MOUSE; code < BTN_MOUSE + 16; code++) {
		if (ioctl(v_g502_fd, UI_SET_KEYBIT, code) < 0) {
			fprintf(stderr, "Failed to set virtual G502 key bit %d\n", code);
			close(v_g502_fd);
			return 1;
		}
	}
	// Enable relative events.
	// REL_HWHEEL carries the wheel tilt (the descriptor's AC Pan usage); without it
	// every horizontal scroll is dropped. The kernel also emits high-resolution
	// wheel deltas alongside the classic detented ones, and libinput prefers them
	// for smooth scrolling, so mirror those too. This set matches the real device's
	// REL bitmask (0x1943) exactly.
	if (ioctl(v_g502_fd, UI_SET_EVBIT, EV_REL) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_X) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_Y) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_WHEEL) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_HWHEEL) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_WHEEL_HI_RES) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES) < 0) {
		fprintf(stderr, "Failed to set virtual G502 relative bits\n");
		close(v_g502_fd);
		return 1;
	}
	// Enable MSC events, otherwise the forwarded MSC_SCAN codes are dropped too.
	if (ioctl(v_g502_fd, UI_SET_EVBIT, EV_MSC) < 0 ||
	    ioctl(v_g502_fd, UI_SET_MSCBIT, MSC_SCAN) < 0) {
		fprintf(stderr, "Failed to set virtual G502 MSC bits\n");
		close(v_g502_fd);
		return 1;
	}

	struct uinput_setup v_g502_setup = {
		.id = {
			.bustype = BUS_USB,
			.vendor  = V_MOUSE_VENDOR,
			.product = V_MOUSE_MODEL,
		},
		.name = V_MOUSE_NAME,
	};
	if (ioctl(v_g502_fd, UI_DEV_SETUP, &v_g502_setup) < 0 ||
	    ioctl(v_g502_fd, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "Failed to create virtual G502 device\n");
		close(v_g502_fd);
		return 1;
	}
	fprintf(stderr, "Virtual G502 device created\n");	// Create virtual keyboard device
	int v_kb_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (v_kb_fd < 0) {
		fprintf(stderr, "Failed to open /dev/uinput for virtual keyboard\n");
		close(v_g502_fd);
		return 1;
	}
	// Enable key events (all keys from 0 to 255)
	if (ioctl(v_kb_fd, UI_SET_EVBIT, EV_KEY) < 0) {
		fprintf(stderr, "Failed to set virtual keyboard event bit\n");
		close(v_kb_fd);
		close(v_g502_fd);
		return 1;
	}
	for (int code = 0; code < 255; code++) {
		ioctl(v_kb_fd, UI_SET_KEYBIT, code);
	}
	// Enable MSC events for scan codes
	if (ioctl(v_kb_fd, UI_SET_EVBIT, EV_MSC) < 0 ||
	    ioctl(v_kb_fd, UI_SET_MSCBIT, MSC_SCAN) < 0) {
		fprintf(stderr, "Failed to set virtual keyboard MSC bits\n");
		close(v_kb_fd);
		close(v_g502_fd);
		return 1;
	}
	struct uinput_setup v_kb_setup = {
		.id = {
			.bustype = BUS_USB,
			.vendor  = KB_USB_VENDOR_ID,
			.product = KB_MODEL_ID,
		},
		.name = "Virtual Keyboard",
	};
	if (ioctl(v_kb_fd, UI_DEV_SETUP, &v_kb_setup) < 0 ||
	    ioctl(v_kb_fd, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "Failed to create virtual keyboard device\n");
		close(v_kb_fd);
		close(v_g502_fd);
		return 1;
	}
	fprintf(stderr, "Virtual keyboard device created\n");	// Initialize semaphore for keyboard event buffer
	if (sem_init(&kb_event_sem, 0, 0) != 0) {
		fprintf(stderr, "Failed to initialize semaphore\n");
		close(v_kb_fd);
		close(v_g502_fd);
		return 1;
	}

	// Start keyboard OUTPUT thread
	pthread_t kb_output_thread;
	keyboard_output_thread_args_t kb_output_args = {
		.out_fd = v_kb_fd,
	};
	if (pthread_create(&kb_output_thread, NULL, keyboard_process_o, &kb_output_args) != 0) {
		fprintf(stderr, "Failed to create keyboard output thread\n");
		sem_destroy(&kb_event_sem);
		close(v_kb_fd);
		close(v_g502_fd);
		return 1;
	}

	// Start one INPUT thread per keyboard. They share the event buffer, which is
	// what merges every keyboard into the single virtual keyboard.
	for (int i = 0; i < found_count; i++) {
		add_keyboard(&found[i]);
	}

	// Pick up keyboards connected later. Enumeration above happens once, so
	// without this a keyboard plugged in afterwards is never grabbed.
	pthread_t monitor_thread;
	if (pthread_create(&monitor_thread, NULL, device_monitor_thread, NULL) != 0) {
		fprintf(stderr, "Failed to start device monitor; hotplug will not work\n");
	}

	// Start mouse IO thread
	pthread_t mouse_io_thread;
	mouse_thread_args_t mouse_io_args = {
		.out_fd = v_g502_fd,
	};
	if (pthread_create(&mouse_io_thread, NULL, mouse_thread_io_func, &mouse_io_args) != 0) {
		fprintf(stderr, "Failed to create mouse IO thread\n");
		sem_destroy(&kb_event_sem);
		close(v_kb_fd);
		close(v_g502_fd);
		return 1;
	}

	// Wait for threads to finish (they won't, this is just to keep the main thread alive)
	// Keyboard threads are not joined: the set grows at runtime, and none of
	// these ever return anyway.
	pthread_join(mouse_io_thread, NULL);
	pthread_join(kb_output_thread, NULL);

	// Cleanup and exit
	sem_destroy(&kb_event_sem);
	close(v_kb_fd);
	close(v_g502_fd);

	return 0;
}
