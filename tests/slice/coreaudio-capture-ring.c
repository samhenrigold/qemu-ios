/* Exercise production capture callback/read with no microphone permission or
 * physical input device: wrap, overflow, pause and callback buffer recycling.
 * SLICE audio/coreaudio.m fn coreaudio_capture coreaudio_read
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-parameter
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef void *AudioQueueRef;
typedef struct {
    void *mAudioData;
    unsigned mAudioDataByteSize;
} Buffer;
typedef Buffer *AudioQueueBufferRef;
typedef int AudioTimeStamp;
typedef int AudioStreamPacketDescription;
typedef unsigned UInt32;
typedef struct {
    unsigned unused;
} HWVoiceIn;
typedef struct {
    HWVoiceIn hw;
    pthread_mutex_t mutex;
    uint8_t *ring;
    size_t capacity, head, tail, level;
    bool enabled;
} CoreaudioVoiceIn;
static unsigned recycled;
static int AudioQueueEnqueueBuffer(AudioQueueRef queue,
                                   AudioQueueBufferRef buffer, unsigned packets,
                                   const void *descs)
{
    recycled++;
    return 0;
}
#include "slice.h"
int main(void)
{
    uint8_t ring[8] = {0}, out[16] = {0};
    CoreaudioVoiceIn s = {
        .ring = ring, .capacity = sizeof ring, .enabled = true};
    pthread_mutex_init(&s.mutex, 0);
    uint8_t first[] = {1, 2, 3, 4, 5, 6};
    Buffer b = {first, sizeof first};
    coreaudio_capture(&s, 0, &b, 0, 0, 0);
    assert(s.level == 6 && recycled == 1);
    assert(coreaudio_read(&s.hw, out, 4) == 4 && !memcmp(out, first, 4));
    uint8_t second[] = {7, 8, 9, 10, 11, 12, 13, 14};
    b.mAudioData = second;
    b.mAudioDataByteSize = sizeof second;
    coreaudio_capture(&s, 0, &b, 0, 0, 0);
    assert(s.level == 8 &&
           recycled ==
               2); /* Full ring drops excess; never overwrites unread PCM. */
    uint8_t expected[] = {5, 6, 7, 8, 9, 10, 11, 12};
    assert(coreaudio_read(&s.hw, out, sizeof out) == 8 &&
           !memcmp(out, expected, 8));
    assert(!s.level && !coreaudio_read(&s.hw, out, sizeof out));
    s.enabled = false;
    coreaudio_capture(&s, 0, &b, 0, 0, 0);
    assert(!s.level && recycled == 3);
    pthread_mutex_destroy(&s.mutex);
    puts(
        "PASS: CoreAudio capture ring wrap, unread sample retention, overflow, pause and buffer recycling");
}
