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
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <systemd/sd-device.h>
#include <unistd.h>

#include "config.h"

// Magic scan codes for mouse side buttons
#define SCAN_BTN_SIDE  0x90004
#define SCAN_BTN_EXTRA 0x90005
#define SCAN_KEY_SHIFT 0x70004
#define SCAN_KEY_CTRL  0x70005

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

// Helper function to find event device by vendor and model IDs
static char* find_event_device(const char* vendor_id, const char* model_id, const char* device_name) {
	struct sd_device_enumerator *enumerator = NULL;
	int r = sd_device_enumerator_new(&enumerator);
	if (r < 0) {
		fprintf(stderr, "Failed to create device enumerator for %s\n", device_name);
		return NULL;
	}

	sd_device_enumerator_add_match_subsystem(enumerator, "input", 1);
	sd_device_enumerator_add_match_sysname(enumerator, "event*");
	sd_device_enumerator_add_match_property(enumerator, "ID_USB_VENDOR_ID", vendor_id);
	sd_device_enumerator_add_match_property(enumerator, "ID_MODEL_ID", model_id);

	sd_device *device = sd_device_enumerator_get_device_first(enumerator);
	if (!device) {
		fprintf(stderr, "%s device not found\n", device_name);
		sd_device_enumerator_unref(enumerator);
		return NULL;
	}

	const char* device_path = NULL;
	sd_device_get_devname(device, &device_path);
	char* result = strdup(device_path);
	sd_device_enumerator_unref(enumerator);
	
	fprintf(stderr, "%s device found: %s\n", device_name, result);
	return result;
}

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

// Helper function to find, open and grab an input device by vendor and model IDs
static int find_open_and_grab_device(const char* vendor_id, const char* model_id, const char* device_name) {
	char* device_path = find_event_device(vendor_id, model_id, device_name);
	if (!device_path) {
		return -1;
	}
	
	int fd = open_and_grab_device(device_path, device_name);
	free(device_path);
	
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

// Helper function to reopen device with retry logic
static int reopen_device(int* fd, const char* vendor_id, const char* model_id, const char* device_name) {
	fprintf(stderr, "%s fd appears invalid, attempting to reopen device\n", device_name);
	
	// Release and close old fd
	release_and_close_device(*fd, device_name);
	*fd = -1;
	
	// Wait a bit before reopening
	sleep(1);
	
	// Try to find and reopen device
	*fd = find_open_and_grab_device(vendor_id, model_id, device_name);
	if (*fd < 0) {
		fprintf(stderr, "Failed to reopen %s device, will retry\n", device_name);
		return -1;
	}
	
	fprintf(stderr, "Successfully reopened and grabbed %s device\n", device_name);
	return 0;
}

// Shared variables for inter-process communication
sem_t kb_event_sem;

// Ring buffer for keyboard events
// The indices are atomic for weakly ordered architectures (e.g. ARM)
#define EVENT_BUFFER_SIZE (1<<18)
pthread_mutex_t kb_buffer_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex to protect buffer access
struct input_event kb_event_buffer[EVENT_BUFFER_SIZE];
size_t head = 0; // Keyboard INPUT thread (write index)
size_t tail = 0; // Keyboard OUTPUT thread (read index)

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
	const char* vendor_id;
	const char* model_id;
	const int out_fd;
} mouse_thread_args_t;
void* mouse_thread_io_func(void* args_void) {
	mouse_thread_args_t* args = (mouse_thread_args_t*)args_void;

	// Find, open and grab the mouse event device
	int mouse_fd = find_open_and_grab_device(args->vendor_id, args->model_id, "mouse");
	if (mouse_fd < 0)
	{
		pthread_exit(NULL);
	}

	// Accumulators for scaled movement
	float accum_x = 0.0;
	float accum_y = 0.0;
	
	const size_t ev_size = sizeof(struct input_event);

	// Read events in a loop
	int consecutive_failures = 0;
	while (1) {
		// Read an event
		struct input_event ev = {0};
		ssize_t n = read(mouse_fd, &ev, ev_size);
		if (n != ev_size) {
			int err = errno;
			fprintf(stderr, "Failed to read mouse event: read returned %zd bytes, errno=%d (%s)\n",
				n, err, get_errno_name(err));
			fprintf(stderr, "  Mouse fd=%d, is_valid=%d, consecutive_failures=%d\n", 
				mouse_fd, is_fd_valid(mouse_fd), consecutive_failures);
			
			// Always try to reopen on any read error
			if (reopen_device(&mouse_fd, args->vendor_id, args->model_id, "mouse") == 0) {
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
			// Only send side buttons to keyboard, forward others to mouse
			if (ev.code == BTN_SIDE || ev.code == BTN_EXTRA) {
				// Convert side buttons to modifier keys
				if      (ev.code == BTN_SIDE) ev.code = KEY_LEFTSHIFT;
				else if (ev.code == BTN_EXTRA) ev.code = KEY_LEFTCTRL;
				send_input_event_to_keyboard(&ev);
			} else {
				ssize_t written = write(args->out_fd, &ev, ev_size);
				if (written != ev_size) {
					fprintf(stderr, "Failed to write mouse button event: wrote %zd/%zu bytes\n", written, ev_size);
				}
			}
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
				if (ev.value == SCAN_BTN_SIDE) {
					ev.value = SCAN_KEY_SHIFT;
					send_input_event_to_keyboard(&ev);
				} else if (ev.value == SCAN_BTN_EXTRA) {
					ev.value = SCAN_KEY_CTRL;
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


// Thread that will handle INPUT keyboard events
typedef struct {
	const char* vendor_id;
	const char* model_id;
} keyboard_thread_args_t;
void* keyboard_process_i(void* args_void) {
	keyboard_thread_args_t* args = (keyboard_thread_args_t*)args_void;

	// Find, open and grab the keyboard event device
	int kb_fd = find_open_and_grab_device(args->vendor_id, args->model_id, "keyboard");
	if (kb_fd < 0) {
		pthread_exit(NULL);
	}

	// Read events in a loop
	int consecutive_failures = 0;
	while (1) {
		struct input_event ev;
		ssize_t n = read(kb_fd, &ev, sizeof(ev));
		if (n != sizeof(ev)) {
			int err = errno;
			fprintf(stderr, "Failed to read keyboard event: read returned %zd bytes, errno=%d (%s)\n", 
				n, err, get_errno_name(err));
			fprintf(stderr, "  Keyboard fd=%d, is_valid=%d\n", kb_fd, is_fd_valid(kb_fd));
			
			// Clear the buffer before reopening to avoid stale events
			clear_keyboard_buffer();
			
			// Always try to reopen on any read error
			if (reopen_device(&kb_fd, args->vendor_id, args->model_id, "keyboard") == 0) {
				consecutive_failures = 0;
			} else {
				consecutive_failures++;
				sleep(5); // Wait before retrying
			}
			continue;
		}
		
		// Reset failure counter on successful read
		consecutive_failures = 0;
		
		// Process the keyboard event here
		send_input_event_to_keyboard(&ev);
	}

	// Release and close the keyboard device
	release_and_close_device(kb_fd, "keyboard");

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



int main()
{
	fprintf(stderr, "Starting G502 daemon...\n");
	sleep(1);

	char* g502_event_device_path = find_event_device(G502_USB_VENDOR_ID_S, G502_MODEL_ID_S, "G502");
	if (!g502_event_device_path) {
		return 1;
	}
	free(g502_event_device_path);

	char* keyboard_event_device_path = find_event_device(KB_USB_VENDOR_ID_S, KB_MODEL_ID_S, "Keyboard");
	if (!keyboard_event_device_path) {
		return 1;
	}
	free(keyboard_event_device_path);

	// Now we have verified the devices exist, create the virtual devices
	
	// Create virtual G502 device
	int v_g502_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (v_g502_fd < 0) {
		fprintf(stderr, "Failed to open /dev/uinput for virtual G502\n");
		return 1;
	}
	// Enable button events
	if (ioctl(v_g502_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(v_g502_fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
	    ioctl(v_g502_fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
	    ioctl(v_g502_fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0 ||
	    ioctl(v_g502_fd, UI_SET_KEYBIT, KEY_LEFTSHIFT) < 0 ||
	    ioctl(v_g502_fd, UI_SET_KEYBIT, KEY_LEFTCTRL) < 0) {
		fprintf(stderr, "Failed to set virtual G502 key bits\n");
		close(v_g502_fd);
		return 1;
	}
	// Enable relative events
	if (ioctl(v_g502_fd, UI_SET_EVBIT, EV_REL) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_X) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_Y) < 0 ||
	    ioctl(v_g502_fd, UI_SET_RELBIT, REL_WHEEL) < 0) {
		fprintf(stderr, "Failed to set virtual G502 relative bits\n");
		close(v_g502_fd);
		return 1;
	}

	struct uinput_setup v_g502_setup = {
		.id = {
			.bustype = BUS_USB,
			.vendor  = G502_USB_VENDOR_ID,
			.product = G502_MODEL_ID,
		},
		.name = "Virtual G502 Hero",
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

	// Start keyboard INPUT thread
	pthread_t kb_input_thread;
	keyboard_thread_args_t kb_input_args = {
		.vendor_id = KB_USB_VENDOR_ID_S,
		.model_id = KB_MODEL_ID_S,
	};
	if (pthread_create(&kb_input_thread, NULL, keyboard_process_i, &kb_input_args) != 0) {
		fprintf(stderr, "Failed to create keyboard input thread\n");
		sem_destroy(&kb_event_sem);
		close(v_kb_fd);
		close(v_g502_fd);
		return 1;
	}

	// Start mouse IO thread
	pthread_t mouse_io_thread;
	mouse_thread_args_t mouse_io_args = {
		.vendor_id = G502_USB_VENDOR_ID_S,
		.model_id = G502_MODEL_ID_S,
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
	pthread_join(mouse_io_thread, NULL);
	pthread_join(kb_input_thread, NULL);
	pthread_join(kb_output_thread, NULL);

	// Cleanup and exit
	sem_destroy(&kb_event_sem);
	close(v_kb_fd);
	close(v_g502_fd);

	return 0;
}
