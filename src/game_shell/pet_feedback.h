// Radiotchi — sound + haptic feedback cues (Game Shell, non-pure).
//
// A thin wrapper over the firmware NotificationApp. It holds NO decision logic — the caller
// (do_eat / do_feed) maps an eat-event bitmask / quest unlock to exactly one FeedbackCue. Sound
// and vibration are GATED INDEPENDENTLY (the user toggles each in Settings; both default OFF).
//
// Thread-safety: pet_feedback_play() is fire-and-forget — notification_message() dispatches to
// the notification service's OWN thread, so it never blocks the caller. Call it only from the
// main input-loop thread (do_feed/do_eat), NEVER from the GUI draw/anim callbacks. RX-safe: the
// speaker is independent hardware from the CC1101 Sub-GHz radio and is arbitrated by
// NotificationApp (this module never touches furi_hal_speaker directly), and play() is async — so
// the brief scan blip at sweep start cannot stall or interfere with the RX-only capture.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PetFeedback PetFeedback;

// One cue per game-feel moment. The caller picks exactly one per event.
typedef enum {
    FEEDBACK_FEED_SCAN = 0, // the sweep starts
    FEEDBACK_EAT, //           an ordinary meal
    FEEDBACK_DELICACY, //      a rare/legible/decoded meal
    FEEDBACK_LEVEL_UP, //      a level was crossed (+ short vibro)
    FEEDBACK_EVOLVE, //        an evolution checkpoint (+ long vibro)
    FEEDBACK_QUEST, //         an achievement unlocked (+ short vibro)
    FEEDBACK_NO_SIGNAL, //     a sweep caught nothing
} FeedbackCue;

// Acquire the NotificationApp record; seed the sound/vibro enables from persisted settings.
PetFeedback* pet_feedback_alloc(bool sound, bool vibro);
void pet_feedback_free(PetFeedback* fb);

// Independent runtime toggles (the Settings "Sound" / "Vibro" rows), persisted by the caller.
void pet_feedback_set_sound(PetFeedback* fb, bool on);
bool pet_feedback_sound(const PetFeedback* fb);
void pet_feedback_set_vibro(PetFeedback* fb, bool on);
bool pet_feedback_vibro(const PetFeedback* fb);

// Play a cue's tone (if sound is on) and/or buzz (if vibro is on and the cue has one). No-op when
// fb==NULL or both are off. Non-blocking; main-loop thread only (see the header note).
void pet_feedback_play(PetFeedback* fb, FeedbackCue cue);

#ifdef __cplusplus
}
#endif
