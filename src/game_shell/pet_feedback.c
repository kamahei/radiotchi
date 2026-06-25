// Radiotchi — sound + haptic feedback cues (implementation; Flipper-only).

#include "pet_feedback.h"

#include <furi.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

struct PetFeedback {
    NotificationApp* notif;
    bool sound;
    bool vibro;
};

// --- cue sound sequences (a note turns the tone ON; message_sound_off ends it) -------------
static const NotificationSequence seq_scan = {
    &message_note_c5, &message_delay_50, &message_sound_off, NULL};
static const NotificationSequence seq_eat = {
    &message_note_c5, &message_delay_50, &message_sound_off,
    &message_note_a4, &message_delay_50, &message_sound_off, NULL};
static const NotificationSequence seq_delicacy = {
    &message_note_e5, &message_delay_50, &message_sound_off,
    &message_note_g5, &message_delay_100, &message_sound_off, NULL};
static const NotificationSequence seq_level = {
    &message_note_c5, &message_delay_50, &message_sound_off,
    &message_note_e5, &message_delay_50, &message_sound_off,
    &message_note_g5, &message_delay_100, &message_sound_off, NULL};
static const NotificationSequence seq_evolve = {
    &message_note_c5, &message_delay_50, &message_sound_off,
    &message_note_e5, &message_delay_50, &message_sound_off,
    &message_note_g5, &message_delay_50, &message_sound_off,
    &message_note_c6, &message_delay_100, &message_sound_off, NULL};
static const NotificationSequence seq_quest = {
    &message_note_g5, &message_delay_50, &message_sound_off,
    &message_note_c6, &message_delay_100, &message_sound_off, NULL};
static const NotificationSequence seq_no_signal = {
    &message_note_a4, &message_delay_100, &message_sound_off, NULL};

// --- shared vibro sequences (milestone cues only) ------------------------------------------
static const NotificationSequence seq_vibro_short = {
    &message_vibro_on, &message_delay_50, &message_vibro_off, NULL};
static const NotificationSequence seq_vibro_long = {
    &message_vibro_on, &message_delay_100, &message_vibro_off, NULL};

static const NotificationSequence* sound_seq_for(FeedbackCue cue) {
    switch(cue) {
    case FEEDBACK_FEED_SCAN: return &seq_scan;
    case FEEDBACK_EAT: return &seq_eat;
    case FEEDBACK_DELICACY: return &seq_delicacy;
    case FEEDBACK_LEVEL_UP: return &seq_level;
    case FEEDBACK_EVOLVE: return &seq_evolve;
    case FEEDBACK_QUEST: return &seq_quest;
    case FEEDBACK_NO_SIGNAL: return &seq_no_signal;
    default: return NULL;
    }
}

static const NotificationSequence* vibro_seq_for(FeedbackCue cue) {
    switch(cue) {
    case FEEDBACK_LEVEL_UP:
    case FEEDBACK_QUEST: return &seq_vibro_short;
    case FEEDBACK_EVOLVE: return &seq_vibro_long;
    default: return NULL; // ordinary feed / scan / no-signal don't buzz
    }
}

PetFeedback* pet_feedback_alloc(bool sound, bool vibro) {
    PetFeedback* fb = malloc(sizeof(PetFeedback));
    if(fb == NULL) return NULL;
    fb->notif = furi_record_open(RECORD_NOTIFICATION);
    fb->sound = sound;
    fb->vibro = vibro;
    return fb;
}

void pet_feedback_free(PetFeedback* fb) {
    if(fb == NULL) return;
    furi_record_close(RECORD_NOTIFICATION);
    free(fb);
}

void pet_feedback_set_sound(PetFeedback* fb, bool on) {
    if(fb != NULL) fb->sound = on;
}

bool pet_feedback_sound(const PetFeedback* fb) {
    return fb != NULL && fb->sound;
}

void pet_feedback_set_vibro(PetFeedback* fb, bool on) {
    if(fb != NULL) fb->vibro = on;
}

bool pet_feedback_vibro(const PetFeedback* fb) {
    return fb != NULL && fb->vibro;
}

void pet_feedback_play(PetFeedback* fb, FeedbackCue cue) {
    if(fb == NULL) return;
    // Vibro and sound are independent; a milestone with both on buzzes, then jingles.
    if(fb->vibro) {
        const NotificationSequence* v = vibro_seq_for(cue);
        if(v != NULL) notification_message(fb->notif, v);
    }
    if(fb->sound) {
        const NotificationSequence* s = sound_seq_for(cue);
        if(s != NULL) notification_message(fb->notif, s);
    }
}
