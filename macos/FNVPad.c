#include "FNVPad.h"
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FNVPadState physical;
static int connected;
static uint16_t rumble_left;
static uint16_t rumble_right;
static int rumble_dirty;

void fnv_pad_physical_state(const FNVPadState *state, int is_connected) {
    pthread_mutex_lock(&lock);
    connected = !!is_connected;
    if (connected && state) {
        if (memcmp((const char *)state + 4, (const char *)&physical + 4, sizeof(FNVPadState) - 4) != 0) {
            uint32_t next_pkt = physical.packet + 1;
            physical = *state;
            physical.packet = next_pkt;
        }
    } else {
        uint32_t next_pkt = physical.packet + 1;
        memset(&physical, 0, sizeof physical);
        physical.packet = next_pkt;
        if (rumble_left != 0 || rumble_right != 0) {
            rumble_left = 0;
            rumble_right = 0;
            rumble_dirty = 1;
        }
    }
    pthread_mutex_unlock(&lock);
}

int fnv_pad_connected(void) {
    pthread_mutex_lock(&lock);
    int c = connected;
    pthread_mutex_unlock(&lock);
    return c;
}

int fnv_analog_enabled(void) {
    return fnv_pad_connected();
}

void fnv_pad_snapshot(FNVPadState *state) {
    pthread_mutex_lock(&lock);
    *state = physical;
    pthread_mutex_unlock(&lock);
}

void fnv_pad_reset(void) {
    pthread_mutex_lock(&lock);
    uint32_t next_pkt = physical.packet + 1;
    memset(&physical, 0, sizeof physical);
    physical.packet = next_pkt;
    rumble_left = 0;
    rumble_right = 0;
    rumble_dirty = 1;
    pthread_mutex_unlock(&lock);
}

void fnv_pad_set_rumble(uint16_t left_motor, uint16_t right_motor) {
    pthread_mutex_lock(&lock);
    if (rumble_left != left_motor || rumble_right != right_motor) {
        rumble_left = left_motor;
        rumble_right = right_motor;
        rumble_dirty = 1;
    }
    pthread_mutex_unlock(&lock);
}

int fnv_pad_consume_rumble(uint16_t *left_motor, uint16_t *right_motor) {
    pthread_mutex_lock(&lock);
    int dirty = rumble_dirty;
    if (dirty) {
        if (left_motor) *left_motor = rumble_left;
        if (right_motor) *right_motor = rumble_right;
        rumble_dirty = 0;
    }
    pthread_mutex_unlock(&lock);
    return dirty;
}
