// (Q27.10) The city's routines (notas-dev/cidade.md): GTA 6's living city. A clock (the hour of the day, game seconds a
// real one), Smart Objects (an Interactable with Smart Object Slots: a shop's counter, a bench, a kiosk; its opening
// hours), and the world's events (a crime seen, a witness's call to the police) for the AI and the UI. The crowds with
// City Routine go to the open ones and leave when they close; a crime reported panics the nearest and one of them calls
// the police a few seconds later.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

typedef struct CitySO {
    Entity e;
    float pos[3];
    uint32_t slots;     // visitors at once
    uint32_t visitors;  // the crowds' agents there now (crowd.c)
    bool open;          // by its hours and the clock
} CitySO;

enum { CITY_EV_CRIME = 1, CITY_EV_POLICE_CALLED, CITY_EV_COUNT };
typedef struct CityEvent {
    uint32_t type;
    float pos[3];
    double hour;        // the clock's when it happened
} CityEvent;

typedef struct CityStats {
    uint32_t objects, open, visitors, events, police_calls;
    double ms;
} CityStats;

// Every frame (crowd_update calls it): the clock while playing, the Smart Objects gathered, the calls made
void city_update(World* w, float dt, bool playing);
float city_hour(void);                        // 0..24
float city_time_scale(void);                  // game seconds a real one
void city_set_time(float hour, float scale);  // scale <= 0: keeps it
uint32_t city_smart_objects(CitySO** out);    // this frame's (the crowds fill .visitors)
bool city_is_open(Entity e);                  // an Interactable with slots, open now
uint32_t city_visitors(Entity e);
// A crime at a point: an event, the agents within radius panic (crowd_alarm), the nearest becomes the witness who calls
// the police 2-4 s later (CITY_EV_POLICE_CALLED)
void city_report_crime(const float at[3], float radius);
uint32_t city_police_calls(void);
uint32_t city_events(const CityEvent** out);  // the last 64, oldest first
const CityStats* city_stats(void);
void city_reset(void);                        // the Play's start: no events, no calls pending
