// Traffic (Q27.2, notas-dev/transito.md): cars that drive a city's roads by themselves. A road is a Spline actor with
// Road Lanes > 0 (CRail's road_ fields: lanes each way, one way, speed limit, density, lane width, signals); its lanes
// follow the curve, offset to the right of their way. Road ends within 6 m of another's start join them (a junction);
// a junction where three roads or more meet has signals when its roads ask (green per axis of approach, 2 s all red).
// The cars are arrays (structure of arrays): each one a driver profile (desired speed, acceleration, comfortable
// braking, time headway, standstill gap: Take-Two's traffic patent's per driver characteristics) following the car
// ahead by the Intelligent Driver Model (IDM: Treiber 2000), stopping at a red stop line, at a junction taking one of
// its lane's next lanes. Far ones (past 150 m) step every other frame. Drawn as actors (a Static Mesh each), placed by
// the curve every frame; kinematic (the player's vehicles are vehicle.c's).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

typedef struct TrafficStats {
    uint32_t roads, lanes, junctions, signals, cars;
    uint32_t stopped;          // cars under 1 km/h now
    uint32_t passed;           // junctions crossed since the play began
    uint32_t red_runs;         // stop lines crossed on red with room to stop (should stay 0)
    float min_gap;             // cm: the smallest bumper-to-bumper gap now (< 0: an overlap)
    float mean_speed;          // km/h
    double ms;                 // this frame's update (CPU)
} TrafficStats;

void traffic_begin_play(World* w);
void traffic_end_play(World* w);
void traffic_update(World* w, float dt, const float camera[3]);
const TrafficStats* traffic_stats(void);
