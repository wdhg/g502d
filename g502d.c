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

#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <systemd/sd-device.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>

#include "config.h"

// Shared variables for inter-process communication
sem_t kb_event_sem;

// Ring buffer for keyboard events
// The indices are atomic for weakly ordered architectures (e.g. ARM)
#define EVENT_BUFFER_SIZE 256
struct input_event kb_event_buffer[EVENT_BUFFER_SIZE]; // Shared buffer
atomic_size_t head = 0; // Keyboard INPUT thread (producer) only
atomic_size_t tail = 0; // Keyboard OUTPUT thread (consumer) only

// Function to send an input event to the keyboard event buffer for the OUTPUT thread to process
static void send_input_event_to_keyboard(struct input_event* ev) {
	// Add the event to the ring buffer
	size_t next_head = (head + 1) % EVENT_BUFFER_SIZE;
	if (next_head == tail) {
		// Buffer is full, drop the event
		fprintf(stderr, "Keyboard event buffer full, dropping event\n");
		return;
	}
	kb_event_buffer[head] = *ev;
	head = next_head;

	// Signal that a new event is available
	sem_post(&kb_event_sem);
}

// Thread that will handle mouse INPUT and OUTPUT events
typedef struct {
	const char* event_device_path;
	const int out_fd;
} mouse_thread_args_t;
void* mouse_thread_io_func(void* args_void) {
	mouse_thread_args_t* args = (mouse_thread_args_t*)args_void;

	// Open the mouse event device
	int mouse_fd = open(args->event_device_path, O_RDONLY);
	if (mouse_fd < 0) {
		fprintf(stderr, "Failed to open mouse event device: %s\n", args->event_device_path);
		pthread_exit(NULL);
	}
	// Capture events
	ioctl(mouse_fd, EVIOCGRAB, 1);

	// Accumulators for scaled movement
	float accum_x = 0.0;
	float accum_y = 0.0;
	
	// Read events in a loop
	while (1) {
		struct input_event ev;
		ssize_t n = read(mouse_fd, &ev, sizeof(ev));
		if (n != sizeof(ev)) {
			fprintf(stderr, "Failed to read mouse event\n");
			continue;
		}

		// Process the mouse event here
		if (ev.type == EV_KEY) {
			// Map side buttons to keyboard modifiers
			if (ev.code == BTN_SIDE) {
				// Map to left shift
				struct input_event kb_ev = ev;
				kb_ev.code = KEY_LEFTSHIFT;
				send_input_event_to_keyboard(&kb_ev);
				continue; // Do not forward the original event
			} else if (ev.code == BTN_EXTRA) {
				// Map to left ctrl
				struct input_event kb_ev = ev;
				kb_ev.code = KEY_LEFTCTRL;
				send_input_event_to_keyboard(&kb_ev);
				continue; // Do not forward the original event
			}
		} else if (ev.type == EV_REL) {
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
		}

		// Forward the event to the virtual mouse device
		write(args->out_fd, &ev, sizeof(ev));
	}

	// Release the mouse device
	ioctl(mouse_fd, EVIOCGRAB, 0);
	close(mouse_fd);

	pthread_exit(NULL);
}


// Thread that will handle INPUT keyboard events
typedef struct {
	const char* event_device_path;
} keyboard_thread_args_t;
void* keyboard_process_i(void* args_void) {
	keyboard_thread_args_t* args = (keyboard_thread_args_t*)args_void;

	// Open the keyboard event device
	int kb_fd = open(args->event_device_path, O_RDONLY);
	if (kb_fd < 0) {
		fprintf(stderr, "Failed to open keyboard event device: %s\n", args->event_device_path);
		pthread_exit(NULL);
	}
	// Capture events
	ioctl(kb_fd, EVIOCGRAB, 1);

	// Read events in a loop
	while (1) {
		struct input_event ev;
		ssize_t n = read(kb_fd, &ev, sizeof(ev));
		if (n != sizeof(ev)) {
			fprintf(stderr, "Failed to read keyboard event\n");
			continue;
		}
		// Process the keyboard event here
		send_input_event_to_keyboard(&ev);
	}

	// Release the keyboard device
	ioctl(kb_fd, EVIOCGRAB, 0);
	close(kb_fd);

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
		sem_wait(&kb_event_sem);

		// Get the next event from the buffer
		struct input_event ev = kb_event_buffer[tail];
		tail = (tail + 1) % EVENT_BUFFER_SIZE;

		// Forward the event to the virtual keyboard device
		write(args->out_fd, &ev, sizeof(ev));
	}

	pthread_exit(NULL);
}



int main()
{

	// Generic variables.
	struct sd_device_enumerator *enumerator = NULL;
	int r = 0;
	sd_device *device = NULL;

	// Disable stdout buffering for real-time logging.
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("Starting G502 daemon...\n");
	sleep(1);




	// Holders for device paths.
	// e.g. "/dev/input/eventX"
	char* g502_event_device_path = NULL;
	char* keyboard_event_device_path = NULL;




	// First we must find which event devices corresponds to the G502.
	enumerator = NULL;
	r = sd_device_enumerator_new(&enumerator);
	if (r < 0)
	{
		fprintf(stderr, "Failed to create device enumerator\n");
		return 1;
	}

	// To find these values, use `udevadm info --query=all --name=/dev/input/eventX` where X is the event number.
	sd_device_enumerator_add_match_subsystem(enumerator, "input", 1);
	sd_device_enumerator_add_match_sysname(enumerator, "event*");
	sd_device_enumerator_add_match_property(enumerator, "ID_USB_VENDOR_ID", G502_USB_VENDOR_ID_S);
	sd_device_enumerator_add_match_property(enumerator, "ID_MODEL_ID", G502_MODEL_ID_S);

	device = sd_device_enumerator_get_device_first(enumerator);
	if (!device)
	{
		fprintf(stderr, "G502 device not found\n");
		sd_device_enumerator_unref(enumerator);
		return 1;
	}

	sd_device_get_devname(device, (const char**)&g502_event_device_path);
	g502_event_device_path = strdup(g502_event_device_path); // Duplicate the string to avoid issues later
	printf("G502 device found: %s\n", g502_event_device_path);
	sd_device_enumerator_unref(enumerator);




	// Next we find the keyboard device.
	enumerator = NULL;
	r = sd_device_enumerator_new(&enumerator);
	if (r < 0)
	{
		fprintf(stderr, "Failed to create device enumerator\n");
		return 1;
	}

	sd_device_enumerator_add_match_subsystem(enumerator, "input", 1);
	sd_device_enumerator_add_match_sysname(enumerator, "event*");
	sd_device_enumerator_add_match_property(enumerator, "ID_USB_VENDOR_ID", KB_USB_VENDOR_ID_S);
	sd_device_enumerator_add_match_property(enumerator, "ID_MODEL_ID", KB_MODEL_ID_S);

	device = sd_device_enumerator_get_device_first(enumerator);
	if (!device)
	{
		fprintf(stderr, "Keyboard device not found\n");
		sd_device_enumerator_unref(enumerator);
		return 1;
	}

	sd_device_get_devname(device, (const char**)&keyboard_event_device_path);
	keyboard_event_device_path = strdup(keyboard_event_device_path); // Duplicate the string to avoid issues later
	printf("Keyboard device found: %s\n", keyboard_event_device_path);
	sd_device_enumerator_unref(enumerator);




	// Now we have the event devices, we can create the virtual devices
	
	// First the virtual G502 device
	int v_g502_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (v_g502_fd < 0)
	{
		fprintf(stderr, "Failed to open /dev/uinput for virtual G502\n");
		return 1;
	}
	// Enable button events
	ioctl(v_g502_fd, UI_SET_EVBIT, EV_KEY);
	ioctl(v_g502_fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(v_g502_fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(v_g502_fd, UI_SET_KEYBIT, BTN_MIDDLE);
	// Enable relative events
	ioctl(v_g502_fd, UI_SET_EVBIT, EV_REL);
	ioctl(v_g502_fd, UI_SET_RELBIT, REL_X);
	ioctl(v_g502_fd, UI_SET_RELBIT, REL_Y);
	ioctl(v_g502_fd, UI_SET_RELBIT, REL_WHEEL);
	// Enable left shift and left ctrl keys
	ioctl(v_g502_fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
	ioctl(v_g502_fd, UI_SET_KEYBIT, KEY_LEFTCTRL);

	struct uinput_setup v_g502_setup = {
		.id = {
			.bustype = BUS_USB,
			.vendor  = G502_USB_VENDOR_ID,
			.product = G502_MODEL_ID,
		},
		.name = "Virtual G502 Hero",
	};
	ioctl(v_g502_fd, UI_DEV_SETUP, &v_g502_setup);
	ioctl(v_g502_fd, UI_DEV_CREATE);
	printf("Virtual G502 device created\n");




	// Then the virtual keyboard device
	int v_kb_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (v_kb_fd < 0)
	{
		fprintf(stderr, "Failed to open /dev/uinput for virtual keyboard\n");
		return 1;
	}
	// Enable key events (all keys from 0 to 255)
	// This is a simple way to enable all keys, but not the most efficient.
	// A more efficient way would be to only enable the keys we need.
	// But for simplicity, we enable all keys here.
	ioctl(v_kb_fd, UI_SET_EVBIT, EV_KEY);
	for (int code = 0; code < 255; code++)
	{
		ioctl(v_kb_fd, UI_SET_KEYBIT, code);
	}
	struct uinput_setup v_kb_setup = {
		.id = {
			.bustype = BUS_USB,
			.vendor  = KB_USB_VENDOR_ID,
			.product = KB_MODEL_ID,
		},
		.name = "Virtual Keyboard",
	};
	ioctl(v_kb_fd, UI_DEV_SETUP, &v_kb_setup);
	ioctl(v_kb_fd, UI_DEV_CREATE);
	printf("Virtual keyboard device created\n");



	// Initialize semaphore for keyboard event buffer
	sem_init(&kb_event_sem, 0, 0);


	// Start keyboard OUTPUT thread
	pthread_t kb_output_thread;
	keyboard_output_thread_args_t kb_output_args = {
		.out_fd = v_kb_fd,
	};
	pthread_create(&kb_output_thread, NULL, keyboard_process_o, &kb_output_args);

	// Start keyboard INPUT thread
	pthread_t kb_input_thread;
	keyboard_thread_args_t kb_input_args = {
		.event_device_path = keyboard_event_device_path,
	};
	pthread_create(&kb_input_thread, NULL, keyboard_process_i, &kb_input_args);

	// Start mouse IO thread
	pthread_t mouse_io_thread;
	mouse_thread_args_t mouse_io_args = {
		.event_device_path = g502_event_device_path,
		.out_fd = v_g502_fd,
	};
	pthread_create(&mouse_io_thread, NULL, mouse_thread_io_func, &mouse_io_args);

	// Wait for threads to finish (they won't, this is just to keep the main thread alive)
	pthread_join(mouse_io_thread, NULL);
	pthread_join(kb_input_thread, NULL);
	pthread_join(kb_output_thread, NULL);

	// Cleanup and exit
	ioctl(v_kb_fd, EVIOCGRAB, 0); // Release the keyboard device
	close(v_kb_fd); // Close virtual keyboard device
	ioctl(v_g502_fd, EVIOCGRAB, 0); // Release the keyboard device
	close(v_g502_fd); // Close virtual G502 device

	return 0;
}
