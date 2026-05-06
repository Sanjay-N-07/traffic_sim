/*
 * traffic_sim.c — JSON Output Version (Fixed)
 * Compile: gcc -Wall -O2 -o traffic_sim traffic_sim.c
 * Run:     echo '{"location":1,"vehicles":20,"timesteps":15}' | ./traffic_sim
 *
 * Fixes applied:
 *  1. Time step sync: snapshot recorded AFTER all updates. All roads/zones
 *     processed uniformly each step. road_count uses queue_size (active on road).
 *  2. Congestion fix: when queue_size == 0, both raw_congestion and
 *     congestion_index are hard-zeroed. EMA only applied when vehicles exist.
 *  3. Vehicle count control: "vehicles" key in config scales the fleet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MAX_ROADS          15
#define MAX_INTERSECTIONS   8
#define MAX_VEHICLES       300
#define MAX_QUEUE           80
#define SAFE_DISTANCE        2
#define MIN_GREEN_TIME       4
#define MAX_RED_WAIT        15
#define MAX_EMERG_OVERRIDE   2
#define EMA_ALPHA           30
#define MIN_ADAPTIVE_GREEN   3
#define MAX_ADAPTIVE_GREEN  20
#define MAX_STEPS           60

typedef enum { RED = 0, GREEN, YELLOW } SignalState;
typedef enum { NORMAL = 0, EMERGENCY }  VehicleType;

typedef struct {
    int         id, entry_time, source_road, dest_road;
    int         max_speed, current_speed;
    VehicleType type;
    int         waiting_time, travel_time, position;
    int         active, completed, counted_this_step;
} Vehicle;

typedef struct {
    int  id;
    char name[64];
    int  capacity, max_speed, congestion_threshold;
    int  current_vehicle_count, congestion_index, raw_congestion;
    int  max_queue_length, total_delay;
    int  vehicle_queue[MAX_QUEUE];
    int  queue_front, queue_rear, queue_size;
    int  intersection_in, intersection_out;
} Road;

typedef struct {
    int         id;
    char        name[64];
    int         incoming_roads[MAX_ROADS], num_incoming;
    int         outgoing_roads[MAX_ROADS], num_outgoing;
    int         base_cycle, adaptive_cycle;
    SignalState signal_state;
    int         current_green_index, signal_timer;
    int         red_wait_time[MAX_ROADS], emergency_overrides;
    int         total_vehicles_served, phase_switches;
} Intersection;

typedef struct {
    int    total_completed, total_entered, max_queue;
    int    emergency_dispatched, emergency_overrides_used;
    int    deadlocks_detected, spillback_events;
    double avg_wait, avg_travel;
    int    congestion_level[MAX_ROADS];
    double signal_util[MAX_INTERSECTIONS];
    int    green_time[MAX_INTERSECTIONS];
    int    total_steps;
} Metrics;

typedef struct {
    int  step;
    int  road_count[MAX_ROADS];   /* queue_size on road this step */
    int  road_cong[MAX_ROADS];    /* raw_congestion % this step   */
    int  inter_green[MAX_INTERSECTIONS];
    int  inter_timer[MAX_INTERSECTIONS];
    int  inter_adaptive[MAX_INTERSECTIONS];
    char events[2048];
    int  events_len;
    int  active_vehicles;         /* total on all roads this step */
} StepSnapshot;

typedef struct {
    Road         roads[MAX_ROADS];
    Intersection intersections[MAX_INTERSECTIONS];
    Vehicle      vehicles[MAX_VEHICLES];
    Metrics      metrics;
    int          num_roads, num_intersections, num_vehicles, total_time_steps;
    char         location_name[128];
    StepSnapshot steps[MAX_STEPS];
    int          num_steps_recorded;
} SimCtx;

static SimCtx *g_ctx  = NULL;
static int     g_cur_step = 0;

static void evAppend(const char *msg) {
    if (!g_ctx || g_cur_step >= MAX_STEPS) return;
    StepSnapshot *ss = &g_ctx->steps[g_cur_step];
    int rem = (int)sizeof(ss->events) - ss->events_len - 4;
    if (rem <= 0) return;
    int n = snprintf(ss->events + ss->events_len, rem, "%s", msg);
    if (n > 0) ss->events_len += n;
}
static void evFmt(const char *fmt, ...) {
    char buf[256]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    evAppend(buf);
}

/* ---- Queue ops ---- */
static int qEnqueue(SimCtx *s, int rid, int vid) {
    Road *r = &s->roads[rid];
    if (r->queue_size >= r->capacity || r->queue_size >= MAX_QUEUE) return 0;
    r->vehicle_queue[r->queue_rear] = vid;
    r->queue_rear = (r->queue_rear + 1) % MAX_QUEUE;
    r->queue_size++; r->current_vehicle_count++;
    if (r->queue_size > r->max_queue_length) r->max_queue_length = r->queue_size;
    return 1;
}
static int qDequeue(SimCtx *s, int rid) {
    Road *r = &s->roads[rid];
    if (r->queue_size == 0) return -1;
    int vid = r->vehicle_queue[r->queue_front];
    r->queue_front = (r->queue_front + 1) % MAX_QUEUE;
    r->queue_size--; r->current_vehicle_count--;
    return vid;
}
static int qPeek (SimCtx *s, int rid) { return s->roads[rid].queue_size ? s->roads[rid].vehicle_queue[s->roads[rid].queue_front] : -1; }
static int qEmpty(SimCtx *s, int rid) { return s->roads[rid].queue_size == 0; }
static int qFull (SimCtx *s, int rid) { return s->roads[rid].queue_size >= s->roads[rid].capacity; }
static int findV (SimCtx *s, int vid) {
    if (vid >= 0 && vid < s->num_vehicles && s->vehicles[vid].id == vid) return vid;
    for (int i = 0; i < s->num_vehicles; i++) if (s->vehicles[i].id == vid) return i;
    return -1;
}

/* ---- Builder helpers ---- */
static void setRoad(SimCtx *s, int i, const char *nm, int cap, int spd, int thr, int ri, int ro) {
    memset(&s->roads[i], 0, sizeof(Road));
    s->roads[i].id = i; strncpy(s->roads[i].name, nm, 63);
    s->roads[i].capacity = cap; s->roads[i].max_speed = spd;
    s->roads[i].congestion_threshold = thr;
    s->roads[i].intersection_in = ri; s->roads[i].intersection_out = ro;
}
static void setInter(SimCtx *s, int i, const char *nm, int cycle) {
    memset(&s->intersections[i], 0, sizeof(Intersection));
    s->intersections[i].id = i; strncpy(s->intersections[i].name, nm, 63);
    s->intersections[i].base_cycle = cycle; s->intersections[i].adaptive_cycle = cycle;
    s->intersections[i].signal_state = GREEN;
}
static void addIn (SimCtx *s, int i, int r) { s->intersections[i].incoming_roads[s->intersections[i].num_incoming++] = r; }
static void addOut(SimCtx *s, int i, int r) { s->intersections[i].outgoing_roads[s->intersections[i].num_outgoing++] = r; }
static void setVeh(SimCtx *s, int i, int id, int et, int src, int dst, int spd, VehicleType tp) {
    Vehicle *v = &s->vehicles[i];
    v->id=id; v->entry_time=et; v->source_road=src; v->dest_road=dst;
    v->max_speed=spd; v->current_speed=spd; v->type=tp;
    v->waiting_time=v->travel_time=v->position=0;
    v->active=v->completed=v->counted_this_step=0;
}

/* ---- Location init ---- */
/*
 * ── THIRUVANMIYUR ─────────────────────────────────────────────
 *
 *  Network topology  (setRoad args: cap, spd, thr, inter_in, inter_out)
 *
 *              [ext]──R1──▶ I0 ──R2──▶ I2 ◀──R6──[ext]
 *              [ext]──R3──▶ I0                ▲
 *              [ext]──R7──▶ I1 ──R4──▶ I0     │
 *                                       └──R5──┘
 *
 *  I0 (Main Signal)  : in=[R1,R3,R4]   out=[R2,R5]
 *  I1 (RGS Merge)    : in=[R7]          out=[R4]   ← feeds I0
 *  I2 (Bus Stand Jn) : in=[R2,R5,R6]   out=[]      ← network exit
 *
 *  Road index → 0-based; displayed as R(index+1) in the UI.
 */
static void initThiruvanmiyur(SimCtx *s) {
    /*
     * FIXED TOPOLOGY (8 roads, 3 intersections):
     *   R2: I3->I1  (was I1->I3)
     *   R6: I3->OUT (was ext->I3)
     *
     *   [ext]--R1--> I1 <--R2-- I3 --R6--> [ext]
     *   [ext]--R3--> I1          ^
     *                I1 --R5-->  I3
     *                ^           ^
     *   I2  --R4--> I1  I2--R8->I3
     *   ^
     *   [ext]--R7-> I2
     *
     *   I1 (Main Signal) : in=[R1,R3,R4,R2]  out=[R5]
     *   I2 (RGS Merge)   : in=[R7]            out=[R4,R8]
     *   I3 (Bus Stand)   : in=[R5,R8]         out=[R2,R6]
     */
    strncpy(s->location_name, "Thiruvanmiyur Signal, Chennai", 127);
    s->num_roads = 8;
    /*           idx  name                               cap spd thr  in  out */
    setRoad(s,0,"OMR Northbound (ext->I1)",               8, 25, 5,  -1,  0);
    setRoad(s,1,"OMR Southbound (I3->I1)",                6, 30, 4,   2,  0); /* CHANGED */
    setRoad(s,2,"Lattice Bridge Road (ext->I1)",          5, 20, 3,  -1,  0);
    setRoad(s,3,"RGS Slip (I2->I1)",                      5, 20, 3,   1,  0);
    setRoad(s,4,"Thiruvanmiyur Main Road (I1->I3)",       6, 25, 4,   0,  2);
    setRoad(s,5,"East Coast Road (I3->OUT)",              5, 30, 3,   2, -1); /* CHANGED */
    setRoad(s,6,"Karpagam Avenue (ext->I2)",              4, 20, 2,  -1,  1);
    setRoad(s,7,"RGS Merge->Bus Stand direct (I2->I3)",   4, 20, 2,   1,  2);

    s->num_intersections = 3;

    /* I1 (Main Signal): in=[R1,R3,R4,R2]  out=[R5] */
    setInter(s,0,"Thiruvanmiyur Main Signal", 9);
    addIn(s,0,0); addIn(s,0,2); addIn(s,0,3); addIn(s,0,1);
    addOut(s,0,4);

    /* I2 (RGS Merge): in=[R7]  out=[R4,R8] */
    setInter(s,1,"Rajiv Gandhi Salai Merge", 6);
    addIn(s,1,6);
    addOut(s,1,3); addOut(s,1,7);

    /* I3 (Bus Stand): in=[R5,R8]  out=[R2,R6] */
    setInter(s,2,"Thiruvanmiyur Bus Stand Junction", 5);
    addIn(s,2,4); addIn(s,2,7);
    addOut(s,2,1); addOut(s,2,5);

    s->num_vehicles = 28; s->total_time_steps = 20;
    /* All vehicles exit via R6(idx5)=I3->OUT.
     * Src roads: R1(0)=ext->I1, R3(2)=ext->I1, R7(6)=ext->I2
     * Path R1/R3: ->I1->R5->I3->R6->exit
     * Path R7:    ->I2 -> R4->I1->R5->I3->R6->exit (or ->R8->I3->R6->exit) */
    setVeh(s, 0, 0, 0,0,5,25,NORMAL);    /* R1->R6(exit via I3) */
    setVeh(s, 1, 1, 0,0,5,25,NORMAL);
    setVeh(s, 2, 2, 0,0,5,20,NORMAL);
    setVeh(s, 3, 3, 1,0,5,25,NORMAL);
    setVeh(s, 4, 4, 1,0,5,20,NORMAL);
    setVeh(s, 5, 5, 2,0,5,25,NORMAL);
    setVeh(s, 6, 6, 2,2,5,20,NORMAL);    /* R3->R6              */
    setVeh(s, 7, 7, 3,6,5,20,NORMAL);    /* R7->R6(via I2->I1)  */
    setVeh(s, 8, 8, 5,6,5,20,NORMAL);
    setVeh(s, 9, 9, 2,0,5,40,EMERGENCY); /* R1->R6 emerg        */
    setVeh(s,10,10, 3,0,5,25,NORMAL);
    setVeh(s,11,11, 3,2,5,20,NORMAL);    /* R3->R6              */
    setVeh(s,12,12, 4,2,5,20,NORMAL);
    setVeh(s,13,13, 4,2,5,20,NORMAL);
    setVeh(s,14,14, 5,6,5,40,EMERGENCY); /* R7->R6 emerg        */
    setVeh(s,15,15, 5,0,5,25,NORMAL);
    setVeh(s,16,16, 6,0,5,25,NORMAL);
    setVeh(s,17,17, 7,0,5,20,NORMAL);
    setVeh(s,18,18, 8,0,5,25,NORMAL);
    setVeh(s,19,19, 9,2,5,20,NORMAL);
    setVeh(s,20,20, 6,6,5,25,NORMAL);    /* R7->R6(via I2)      */
    setVeh(s,21,21, 7,6,5,25,NORMAL);
    setVeh(s,22,22, 8,6,5,30,NORMAL);
    setVeh(s,23,23, 3,6,5,20,NORMAL);
    setVeh(s,24,24, 5,6,5,20,NORMAL);
    setVeh(s,25,25, 2,6,5,20,NORMAL);
    setVeh(s,26,26, 4,6,5,20,NORMAL);
    setVeh(s,27,27, 6,6,5,20,NORMAL);
}

static void initAdyar(SimCtx *s) {
    /*
     * FIXED TOPOLOGY (9 roads, 3 intersections):
     *   R4: I1->OUT (was ext->I1)
     *   R6: I2->OUT (was ext->I2)
     *
     *   [ext]--R1--> I1 --R2--> I2 --R6--> [ext]
     *   [ext]--R3--> I1  R4->[ext]
     *                I1 --R5--> I3 --R9--> [ext]
     *   [ext]--R7--> I2
     *   [ext]--R8--> I3
     *
     *   I1 (Adyar Main) : in=[R1,R3]      out=[R2,R4,R5]
     *   I2 (Depot Jn)   : in=[R2,R7]      out=[R6]
     *   I3 (Bus Stand)  : in=[R5,R8]      out=[R9]
     */
    strncpy(s->location_name, "Adyar Signal, Chennai", 127);
    s->num_roads = 9;
    /*           idx  name                               cap spd thr  in  out */
    setRoad(s,0,"Adyar Bridge Road (ext->I1)",           8, 30, 5,  -1,  0);
    setRoad(s,1,"LB Road East (I1->I2)",                 6, 25, 4,   0,  1);
    setRoad(s,2,"LB Road West (ext->I1)",                7, 25, 5,  -1,  0);
    setRoad(s,3,"Kasturba Nagar slip (I1->OUT)",         4, 20, 2,   0, -1); /* CHANGED */
    setRoad(s,4,"Adyar River svc road (I1->I3)",         5, 20, 3,   0,  2);
    setRoad(s,5,"Gandhi Nagar Main Rd (I2->OUT)",        6, 25, 4,   1, -1); /* CHANGED */
    setRoad(s,6,"Adyar Depot Road (ext->I2)",            5, 20, 3,  -1,  1);
    setRoad(s,7,"Lattice Bridge South (ext->I3)",        6, 30, 4,  -1,  2);
    setRoad(s,8,"Bus Stand exit road (I3->ext)",         5, 20, 3,   2, -1);

    s->num_intersections = 3;

    /* I1 (Adyar Main): in=[R1,R3]  out=[R2,R4,R5] */
    setInter(s,0,"Adyar Main Signal", 10);
    addIn(s,0,0); addIn(s,0,2);
    addOut(s,0,1); addOut(s,0,3); addOut(s,0,4);

    /* I2 (Depot Jn): in=[R2,R7]  out=[R6] */
    setInter(s,1,"Adyar Depot Junction", 7);
    addIn(s,1,1); addIn(s,1,6);
    addOut(s,1,5);

    /* I3 (Bus Stand): in=[R5,R8]  out=[R9] */
    setInter(s,2,"Adyar Bus Stand Junction", 6);
    addIn(s,2,4); addIn(s,2,7);
    addOut(s,2,8);

    s->num_vehicles = 22; s->total_time_steps = 15;
    /* Valid paths: R1->I1->R2->I2->R6->exit
     *             R1->I1->R4->exit
     *             R1->I1->R5->I3->R9->exit
     *             R7->I2->R6->exit  */
    setVeh(s, 0, 0, 0,0,1,30,NORMAL);    /* R1->R2(->I2->R6)  */
    setVeh(s, 1, 1, 0,0,1,30,NORMAL);
    setVeh(s, 2, 2, 0,0,4,25,NORMAL);    /* R1->R5(->I3->R9)  */
    setVeh(s, 3, 3, 2,0,1,25,NORMAL);    /* R3->R2            */
    setVeh(s, 4, 4, 2,0,4,25,NORMAL);    /* R3->R5            */
    setVeh(s, 5, 5, 2,2,1,25,NORMAL);    /* R3->R2            */
    setVeh(s, 6, 6, 2,0,3,20,NORMAL);    /* R1->R4(->exit)    */
    setVeh(s, 7, 7, 2,0,4,20,NORMAL);    /* R1->R5            */
    setVeh(s, 8, 8, 2,0,4,30,EMERGENCY); /* R1->R5 emerg      */
    setVeh(s, 9, 9, 3,2,1,30,NORMAL);    /* R3->R2            */
    setVeh(s,10,10, 3,2,1,25,NORMAL);
    setVeh(s,11,11, 2,6,5,25,NORMAL);    /* R7->R6(I2->exit)  */
    setVeh(s,12,12, 2,6,5,25,NORMAL);
    setVeh(s,13,13, 3,6,5,25,NORMAL);
    setVeh(s,14,14, 4,6,5,25,EMERGENCY); /* R7->R6 emerg      */
    setVeh(s,15,15, 5,7,8,25,NORMAL);    /* R8->R9            */
    setVeh(s,16,16, 5,0,1,30,NORMAL);    /* R1->R2            */
    setVeh(s,17,17, 5,2,4,25,NORMAL);    /* R3->R5            */
    setVeh(s,18,18, 6,0,3,20,NORMAL);    /* R1->R4            */
    setVeh(s,19,19, 6,7,8,30,NORMAL);    /* R8->R9            */
    setVeh(s,20,20, 7,7,4,30,NORMAL);    /* R8->R5            */
    setVeh(s,21,21, 7,2,1,25,NORMAL);    /* R3->R2            */
}

static void initVelachery(SimCtx *s) {
    /*
     * FIXED TOPOLOGY (10 roads, 3 intersections):
     *   R3: I1->OUT (was ext->I1)
     *   R5: I2->I1  (was ext->I2)
     *   R8: I3->OUT (was ext->I3)
     *
     *   [ext]--R1--> I1 --R2--> I2 --R6--> I3 --R9--> [ext]
     *   [ext]--R4--> I1  R3->[ext]   R5->I1  R10->[ext]
     *                           ^    R7->[ext]
     *                I2--R5-->  I1
     *
     *   I1 (Main Signal)  : in=[R1,R4,R6]    out=[R2,R3]
     *   I2 (Phoenix Mall) : in=[R2,R7]        out=[R5,R7,R10]
     *   I3 (South Jn)     : in=[R7,R9]        out=[R8]
     *
     *   NOTE: road idx vs display:
     *     idx0=R1 idx1=R2 idx2=R3 idx3=R4 idx4=R5
     *     idx5=R6 idx6=R7 idx7=R8 idx8=R9 idx9=R10
     */
    strncpy(s->location_name, "Velachery Main Road Junction, Chennai", 127);
    s->num_roads = 10;
    /*           idx  name                               cap spd thr  in  out */
    setRoad(s,0,"Velachery Main Rd North (ext->I1)",     8, 30, 5,  -1,  0);
    setRoad(s,1,"Velachery Main Rd (I1->I2)",            7, 30, 5,   0,  1);
    setRoad(s,2,"100 Feet Road (I1->OUT)",               6, 25, 4,   0, -1); /* CHANGED */
    setRoad(s,3,"Inner Ring Road West (ext->I1)",        7, 30, 5,  -1,  0);
    setRoad(s,4,"Phoenix Mall access (I1->I2)",          5, 20, 3,   0,  1);
    setRoad(s,5,"Taramani Link Rd (I2->I1)",             6, 25, 4,   1,  0); /* CHANGED */
    setRoad(s,6,"Velachery-Taramani (I2->I3)",           5, 20, 3,   1,  2);
    setRoad(s,7,"Velachery Lake Road (ext->I3)",         4, 20, 2,  -1,  2);
    setRoad(s,8,"MRTS feeder road (I3->OUT)",            5, 25, 3,   2, -1); /* CHANGED */
    setRoad(s,9,"Velachery exit Guindy (I2->ext)",       6, 30, 4,   1, -1);

    s->num_intersections = 3;

    /* I1 (Main Signal): in=[R1,R4,R6]  out=[R2,R3]
     *   idx: R1=0, R4=3, R6=5   R2=1, R3=2         */
    setInter(s,0,"Velachery Main Signal", 10);
    addIn(s,0,0); addIn(s,0,3); addIn(s,0,4);
    addOut(s,0,1); addOut(s,0,2);

    /* I2 (Phoenix Mall): in=[R2,R6]  out=[R5,R7,R10]
     *   idx: R2=1, R6=5   R5=4, R7=6, R10=9        */
    setInter(s,1,"Phoenix Mall Junction", 8);
    addIn(s,1,1); addIn(s,1,5);
    addOut(s,1,4); addOut(s,1,6); addOut(s,1,9);

    /* I3 (South Jn): in=[R7,R8]  out=[R9]
     *   idx: R7=6, R8=7   R9=8                      */
    setInter(s,2,"Velachery South Junction", 6);
    addIn(s,2,6); addIn(s,2,7);
    addOut(s,2,8);

    s->num_vehicles = 28; s->total_time_steps = 20;
    /* Exit roads: R3(idx2)=I1->OUT, R9(idx8)=I3->OUT, R10(idx9)=I2->ext
     * I1.out=[R2(1),R3(2),R5(4)]  I2.out=[R6(5),R7(6),R10(9)]  I3.out=[R9(8)]
     * dst=9(R10): I1->I2->R10->exit | dst=2(R3): I1->R3->exit | dst=8(R9): I3->R9->exit */
    setVeh(s, 0, 0, 0,0,9,30,NORMAL);    /* R1->R10(I1->I2->exit)  */
    setVeh(s, 1, 1, 0,0,9,30,NORMAL);
    setVeh(s, 2, 2, 0,0,2,25,NORMAL);    /* R1->R3(I1->exit)       */
    setVeh(s, 3, 3, 0,3,9,30,NORMAL);    /* R4->R10                */
    setVeh(s, 4, 4, 1,3,9,30,NORMAL);
    setVeh(s, 5, 5, 1,3,2,30,NORMAL);    /* R4->R3                 */
    setVeh(s, 6, 6, 1,0,9,30,NORMAL);    /* R1->R10                */
    setVeh(s, 7, 7, 1,0,2,25,NORMAL);    /* R1->R3                 */
    setVeh(s, 8, 8, 1,0,9,30,EMERGENCY); /* R1->R10 emerg          */
    setVeh(s, 9, 9, 1,5,9,25,NORMAL);    /* R6(I2->I1)->R10        */
    setVeh(s,10,10, 1,5,9,25,NORMAL);
    setVeh(s,11,11, 2,5,9,25,NORMAL);
    setVeh(s,12,12, 2,5,9,25,NORMAL);
    setVeh(s,13,13, 2,5,9,25,EMERGENCY);
    setVeh(s,14,14, 2,0,9,30,NORMAL);    /* R1->R10                */
    setVeh(s,15,15, 2,0,9,30,NORMAL);
    setVeh(s,16,16, 3,7,8,25,NORMAL);    /* R8(ext->I3)->R9->exit  */
    setVeh(s,17,17, 3,7,8,25,NORMAL);
    setVeh(s,18,18, 3,0,9,30,NORMAL);    /* R1->R10                */
    setVeh(s,19,19, 3,0,2,25,NORMAL);    /* R1->R3                 */
    setVeh(s,20,20, 4,0,9,30,NORMAL);    /* R1->R10                */
    setVeh(s,21,21, 4,5,9,25,NORMAL);    /* R6->R10                */
    setVeh(s,22,22, 5,7,8,25,NORMAL);    /* R8->R9                 */
    setVeh(s,23,23, 5,0,2,25,NORMAL);    /* R1->R3                 */
    setVeh(s,24,24, 6,0,9,30,NORMAL);    /* R1->R10                */
    setVeh(s,25,25, 6,5,9,25,NORMAL);    /* R6->R10                */
    setVeh(s,26,26, 7,7,8,20,NORMAL);    /* R8->R9                 */
    setVeh(s,27,27, 8,0,9,30,NORMAL);    /* R1->R10                */
}

static void scaleVehicles(SimCtx *s, int target) {
    int base = s->num_vehicles;
    if (target <= 0 || target == base) return;
    if (target > MAX_VEHICLES) target = MAX_VEHICLES;

    if (target < base) {
        s->num_vehicles = target;
        return;
    }

    int extra = target - base;
    int spread = s->total_time_steps > 1 ? s->total_time_steps : 15;

    for (int i = 0; i < extra; i++) {
        Vehicle *src = &s->vehicles[i % base];
        Vehicle *dst = &s->vehicles[base + i];
        *dst = *src;
        dst->id            = base + i;
        dst->entry_time    = (i * spread) / extra;
        dst->active        = 0;
        dst->completed     = 0;
        dst->counted_this_step = 0;
        dst->waiting_time  = 0;
        dst->travel_time   = 0;
        dst->position      = 0;
    }
    s->num_vehicles = target;
}

/* ---- Simulation logic ---- */
static int hasEmergency(SimCtx *s, int rid) {
    Road *r = &s->roads[rid];
    for (int i = 0; i < r->queue_size; i++) {
        int idx = (r->queue_front + i) % MAX_QUEUE;
        int vi  = findV(s, r->vehicle_queue[idx]);
        if (vi >= 0 && s->vehicles[vi].type == EMERGENCY) return 1;
    }
    return 0;
}

static int computeAdaptiveCycle(SimCtx *s, int inter) {
    Intersection *in = &s->intersections[inter];
    int busiest = 0;
    for (int r = 0; r < in->num_incoming; r++) {
        int q = s->roads[in->incoming_roads[r]].queue_size;
        if (q > busiest) busiest = q;
    }
    int cycle = in->base_cycle + busiest * 2;
    if (cycle < MIN_ADAPTIVE_GREEN) cycle = MIN_ADAPTIVE_GREEN;
    if (cycle > MAX_ADAPTIVE_GREEN) cycle = MAX_ADAPTIVE_GREEN;
    return cycle;
}

static void updateSignals(SimCtx *s) {
    for (int i = 0; i < s->num_intersections; i++) {
        Intersection *in = &s->intersections[i];
        int emerg_road = -1;
        for (int r = 0; r < in->num_incoming; r++)
            if (hasEmergency(s, in->incoming_roads[r])) { emerg_road = r; break; }
        if (emerg_road >= 0 && in->current_green_index != emerg_road) {
            evFmt("[EMERG-PRIORITY] I%d->R%d GREEN|", i+1, in->incoming_roads[emerg_road]+1);
            in->current_green_index = emerg_road;
            in->signal_state = GREEN; in->signal_timer = 0;
            in->red_wait_time[emerg_road] = 0; in->phase_switches++;
            s->metrics.green_time[i]++;
            continue;
        }
        in->signal_timer++;
        for (int r = 0; r < in->num_incoming; r++)
            if (r != in->current_green_index) in->red_wait_time[r]++;

        int force = -1;
        for (int r = 0; r < in->num_incoming; r++)
            if (r != in->current_green_index && in->red_wait_time[r] >= MAX_RED_WAIT)
                { force = r; break; }

        if (force < 0 && qEmpty(s, in->incoming_roads[in->current_green_index])) {
            for (int r = 0; r < in->num_incoming; r++)
                if (!qEmpty(s, in->incoming_roads[r])) {
                    evFmt("[SMART-SWITCH] I%d->R%d|", i+1, in->incoming_roads[r]+1);
                    in->current_green_index = r; in->signal_timer = 0;
                    in->red_wait_time[r] = 0; in->phase_switches++; break;
                }
        }
        in->adaptive_cycle = computeAdaptiveCycle(s, i);
        if (in->signal_timer >= in->adaptive_cycle || force >= 0) {
            int next = in->current_green_index;
            if (force >= 0) {
                next = force;
            } else {
                for (int r = 1; r <= in->num_incoming; r++) {
                    int idx = (in->current_green_index + r) % in->num_incoming;
                    if (!qEmpty(s, in->incoming_roads[idx])) { next = idx; break; }
                }
            }
            if (next != in->current_green_index) in->phase_switches++;
            in->current_green_index = next; in->signal_state = GREEN;
            in->signal_timer = 0; in->emergency_overrides = 0;
            in->red_wait_time[next] = 0;
        }
        s->metrics.green_time[i]++;
    }
}

static void addVehicles(SimCtx *s, int t) {
    for (int i = 0; i < s->num_vehicles; i++) {
        if (s->vehicles[i].entry_time > t || s->vehicles[i].active || s->vehicles[i].completed) continue;
        int src = s->vehicles[i].source_road;
        if (!qFull(s, src)) {
            qEnqueue(s, src, s->vehicles[i].id);
            s->vehicles[i].active = 1; s->metrics.total_entered++;
            const char *tp = s->vehicles[i].type == EMERGENCY ? "EMERG" : "NORMAL";
            evFmt("[ENTER] V%d(%s)->R%d|", s->vehicles[i].id+1, tp, src+1);
            if (s->vehicles[i].type == EMERGENCY) s->metrics.emergency_dispatched++;
        } else {
            if (!s->vehicles[i].counted_this_step) {
                s->vehicles[i].waiting_time++; s->vehicles[i].counted_this_step = 1;
            }
            evFmt("[WAIT] V%d R%d FULL|", s->vehicles[i].id+1, src+1);
        }
    }
}

static void enforceSafeDistance(SimCtx *s, int rid) {
    Road *r = &s->roads[rid];
    if (r->queue_size < 2) return;
    for (int pos = 0; pos < r->queue_size; pos++) {
        int idx = (r->queue_front + pos) % MAX_QUEUE;
        int vi  = findV(s, r->vehicle_queue[idx]);
        if (vi < 0) continue;
        s->vehicles[vi].position = pos;
        if (pos > 0) {
            int aidx = (r->queue_front + pos - 1) % MAX_QUEUE;
            int ai   = findV(s, r->vehicle_queue[aidx]);
            if (ai < 0) continue;
            int gap = pos - s->vehicles[ai].position;
            if (gap < SAFE_DISTANCE) {
                s->vehicles[vi].current_speed = s->vehicles[vi].max_speed * gap / SAFE_DISTANCE;
                if (!s->vehicles[vi].counted_this_step) {
                    s->vehicles[vi].waiting_time++; s->vehicles[vi].counted_this_step = 1;
                }
            } else {
                s->vehicles[vi].current_speed = s->vehicles[vi].max_speed;
            }
        }
    }
}

static void handleEmergency(SimCtx *s, int vid) {
    int vi = findV(s, vid);
    if (vi < 0) return;
    int src = s->vehicles[vi].source_road;
    int oi  = s->roads[src].intersection_out;
    if (oi < 0) return;
    Intersection *in = &s->intersections[oi];
    if (in->emergency_overrides >= MAX_EMERG_OVERRIDE) return;
    for (int r = 0; r < in->num_incoming; r++) {
        if (in->incoming_roads[r] == src && in->current_green_index != r && in->signal_timer >= MIN_GREEN_TIME) {
            evFmt("[EMERG-OVERRIDE] I%d V%d|", oi+1, vid+1);
            in->current_green_index = r; in->signal_state = GREEN;
            in->signal_timer = 0; in->emergency_overrides++;
            in->red_wait_time[r] = 0; in->phase_switches++;
            s->metrics.emergency_overrides_used++;
            break;
        }
    }
}

static void detectSpillback(SimCtx *s, int rid) {
    int oi = s->roads[rid].intersection_out;
    if (oi < 0) return;
    Intersection *in = &s->intersections[oi];
    for (int o = 0; o < in->num_outgoing; o++) {
        int nr = in->outgoing_roads[o];
        if (qFull(s, nr)) {
            evFmt("[SPILLBACK] R%d->R%d|", nr+1, rid+1);
            s->roads[rid].raw_congestion = 100;
            s->metrics.spillback_events++;
        }
    }
}

/*
 * FIX 2: Hard-zero congestion when road is empty.
 * EMA would previously keep congestion > 0 even after all vehicles left.
 */
static void updateCongestion(SimCtx *s, int rid) {
    Road *r = &s->roads[rid];
    if (r->capacity == 0) return;
    int cnt = r->queue_size;   /* use queue_size = vehicles physically on road */

    if (cnt == 0) {
        r->raw_congestion   = 0;
        r->congestion_index = 0;
        return;
    }

    r->raw_congestion = (cnt * 100) / r->capacity;
    if (r->raw_congestion > 100) r->raw_congestion = 100;
    r->congestion_index = (EMA_ALPHA * r->raw_congestion + (100 - EMA_ALPHA) * r->congestion_index) / 100;

    if (cnt >= r->congestion_threshold) {
        int excess = cnt - r->congestion_threshold;
        int red = excess * 20; if (red > 85) red = 85;
        for (int pos = 0; pos < r->queue_size; pos++) {
            int idx = (r->queue_front + pos) % MAX_QUEUE;
            int vi  = findV(s, r->vehicle_queue[idx]);
            if (vi < 0) continue;
            s->vehicles[vi].current_speed = s->vehicles[vi].max_speed * (100 - red) / 100;
            if (!s->vehicles[vi].counted_this_step) {
                s->vehicles[vi].waiting_time++; s->vehicles[vi].counted_this_step = 1; r->total_delay++;
            }
        }
        evFmt("[CONG] R%d:%d%%|", rid+1, r->congestion_index);
    }
}

static int detectDeadlock(SimCtx *s) {
    int total = 0;
    for (int i = 0; i < s->num_roads; i++) total += s->roads[i].queue_size;
    if (!total) return 0;
    for (int i = 0; i < s->num_intersections; i++) {
        int gr = s->intersections[i].incoming_roads[s->intersections[i].current_green_index];
        if (!qEmpty(s, gr)) return 0;
        int hw = 0;
        for (int r = 0; r < s->intersections[i].num_incoming; r++)
            if (r != s->intersections[i].current_green_index && !qEmpty(s, s->intersections[i].incoming_roads[r]))
                { hw = 1; break; }
        if (!hw) return 0;
    }
    return 1;
}

static void resolveDeadlock(SimCtx *s) {
    evFmt("[DEADLOCK] Forced rotation|");
    s->metrics.deadlocks_detected++;
    for (int i = 0; i < s->num_intersections; i++) {
        Intersection *in = &s->intersections[i];
        int found = 0;
        for (int r = 1; r <= in->num_incoming; r++) {
            int idx = (in->current_green_index + r) % in->num_incoming;
            if (!qEmpty(s, in->incoming_roads[idx])) { in->current_green_index = idx; found = 1; break; }
        }
        if (!found) in->current_green_index = (in->current_green_index + 1) % in->num_incoming;
        in->signal_timer = 0; in->signal_state = GREEN;
    }
}

/*
 * FIX 1: moveVehicles — all roads/zones processed uniformly each step.
 * Pass 1: advance one vehicle per intersection (green road).
 * Pass 2: accumulate waiting time on ALL red roads across ALL zones.
 */
static void moveVehicles(SimCtx *s) {
    /* Pass 1: move green-road head vehicle at every intersection */
    for (int i = 0; i < s->num_intersections; i++) {
        Intersection *in = &s->intersections[i];
        int groad = in->incoming_roads[in->current_green_index];
        if (qEmpty(s, groad)) continue;
        int vid = qPeek(s, groad);
        int vi  = findV(s, vid);
        if (vi < 0) continue;
        if (s->vehicles[vi].type == EMERGENCY) handleEmergency(s, vid);
        int dest   = s->vehicles[vi].dest_road;
        int target = -1;
        for (int o = 0; o < in->num_outgoing; o++)
            if (in->outgoing_roads[o] == dest) { target = dest; break; }
        if (target < 0 && in->num_outgoing > 0) target = in->outgoing_roads[0];
        if (in->num_outgoing == 0) {
            qDequeue(s, groad);
            s->vehicles[vi].active = 0; s->vehicles[vi].completed = 1;
            s->metrics.total_completed++; in->total_vehicles_served++;
            evFmt("[ARRIVED] V%d I%d|", vid+1, i+1);
            continue;
        }
        if (target < 0) continue;
        detectSpillback(s, groad);
        if (qFull(s, target)) {
            evFmt("[BLOCKED] V%d R%d|", vid+1, target+1);
            if (!s->vehicles[vi].counted_this_step) {
                s->vehicles[vi].waiting_time++; s->vehicles[vi].counted_this_step = 1;
                s->roads[groad].total_delay++;
            }
            continue;
        }
        qDequeue(s, groad);
        if (s->roads[target].intersection_out < 0) {
            s->vehicles[vi].active = 0; s->vehicles[vi].completed = 1;
            s->metrics.total_completed++; in->total_vehicles_served++;
            evFmt("[ARRIVED] V%d R%d|", vid+1, target+1);
        } else {
            qEnqueue(s, target, vid);
            evFmt("[MOVED] V%d R%d->R%d|", vid+1, groad+1, target+1);
        }
    }

    /* Pass 2: waiting time for all vehicles on red roads — all zones equally */
    for (int i = 0; i < s->num_roads; i++) {
        int oi = s->roads[i].intersection_out;
        if (oi < 0) continue;
        Intersection *in = &s->intersections[oi];
        if (in->incoming_roads[in->current_green_index] == i) continue;  /* skip green */
        for (int pos = 0; pos < s->roads[i].queue_size; pos++) {
            int idx = (s->roads[i].queue_front + pos) % MAX_QUEUE;
            int vi  = findV(s, s->roads[i].vehicle_queue[idx]);
            if (vi >= 0 && s->vehicles[vi].active && !s->vehicles[vi].counted_this_step) {
                s->vehicles[vi].waiting_time++;
                s->vehicles[vi].counted_this_step = 1;
                s->roads[i].total_delay++;
            }
        }
    }
}

static void recordMetrics(SimCtx *s, int t) {
    for (int i = 0; i < s->num_roads; i++) {
        if (s->roads[i].max_queue_length > s->metrics.max_queue)
            s->metrics.max_queue = s->roads[i].max_queue_length;
        s->metrics.congestion_level[i] = s->roads[i].congestion_index;
    }
    for (int i = 0; i < s->num_intersections; i++)
        if (t > 0) s->metrics.signal_util[i] = (double)s->metrics.green_time[i] / t * 100.0;
    for (int i = 0; i < s->num_vehicles; i++)
        if (s->vehicles[i].active) s->vehicles[i].travel_time++;
}

/*
 * FIX 1 (cont): snapshot records queue_size (actual on-road count) and
 * raw_congestion (true %), not the EMA-lagged congestion_index.
 */
static void recordSnapshot(SimCtx *s, int t) {
    if (t >= MAX_STEPS) return;
    StepSnapshot *ss = &s->steps[t];
    ss->step = t;
    int active_total = 0;
    for (int i = 0; i < s->num_roads; i++) {
        ss->road_count[i] = s->roads[i].queue_size;
        ss->road_cong[i]  = s->roads[i].raw_congestion;
        active_total += s->roads[i].queue_size;
    }
    ss->active_vehicles = active_total;
    for (int i = 0; i < s->num_intersections; i++) {
        ss->inter_green[i]    = s->intersections[i].incoming_roads[s->intersections[i].current_green_index];
        ss->inter_timer[i]    = s->intersections[i].signal_timer;
        ss->inter_adaptive[i] = s->intersections[i].adaptive_cycle;
    }
    s->num_steps_recorded = t + 1;
}

static void jsonEscapeStr(const char *s, char *out, int outlen) {
    int j = 0;
    for (int i = 0; s[i] && j < outlen - 2; i++) {
        if      (s[i] == '"')  { out[j++]='\\'; out[j++]='"'; }
        else if (s[i] == '\\') { out[j++]='\\'; out[j++]='\\'; }
        else if (s[i] == '\n') { out[j++]='\\'; out[j++]='n'; }
        else                   { out[j++] = s[i]; }
    }
    out[j] = 0;
}

static void outputJSON(SimCtx *s) {
    long tw = 0, tt = 0;
    for (int i = 0; i < s->num_vehicles; i++) { tw += s->vehicles[i].waiting_time; tt += s->vehicles[i].travel_time; }
    s->metrics.avg_wait   = s->num_vehicles ? (double)tw / s->num_vehicles : 0;
    s->metrics.avg_travel = s->metrics.total_completed ? (double)tt / s->metrics.total_completed : 0;

    char esc[256];
    printf("{");
    jsonEscapeStr(s->location_name, esc, sizeof(esc));
    printf("\"location\":\"%s\",", esc);
    printf("\"summary\":{\"total_vehicles\":%d,\"entered\":%d,\"completed\":%d,"
           "\"avg_wait\":%.2f,\"avg_travel\":%.2f,\"max_queue\":%d,"
           "\"emergency_dispatched\":%d,\"emergency_overrides\":%d,"
           "\"deadlocks\":%d,\"spillbacks\":%d,\"total_steps\":%d},",
           s->num_vehicles, s->metrics.total_entered, s->metrics.total_completed,
           s->metrics.avg_wait, s->metrics.avg_travel, s->metrics.max_queue,
           s->metrics.emergency_dispatched, s->metrics.emergency_overrides_used,
           s->metrics.deadlocks_detected, s->metrics.spillback_events, s->metrics.total_steps);

    printf("\"roads\":[");
    for (int i = 0; i < s->num_roads; i++) {
        Road *r = &s->roads[i];
        jsonEscapeStr(r->name, esc, sizeof(esc));
        printf("{\"id\":%d,\"name\":\"%s\",\"capacity\":%d,\"max_speed\":%d,"
               "\"threshold\":%d,\"max_queue\":%d,\"congestion\":%d,\"delay\":%d}%s",
               i+1, esc, r->capacity, r->max_speed, r->congestion_threshold,
               r->max_queue_length, r->congestion_index, r->total_delay,
               i < s->num_roads-1 ? "," : "");
    }
    printf("],");

    printf("\"intersections\":[");
    for (int i = 0; i < s->num_intersections; i++) {
        Intersection *in = &s->intersections[i];
        jsonEscapeStr(in->name, esc, sizeof(esc));
        printf("{\"id\":%d,\"name\":\"%s\",\"cycle\":%d,\"served\":%d,\"switches\":%d,\"util\":%.1f,\"incoming\":[",
               i+1, esc, in->base_cycle, in->total_vehicles_served, in->phase_switches, s->metrics.signal_util[i]);
        for (int r = 0; r < in->num_incoming; r++) printf("%d%s", in->incoming_roads[r]+1, r<in->num_incoming-1?",":"");
        printf("],\"outgoing\":[");
        for (int r = 0; r < in->num_outgoing; r++) printf("%d%s", in->outgoing_roads[r]+1, r<in->num_outgoing-1?",":"");
        printf("]}%s", i < s->num_intersections-1 ? "," : "");
    }
    printf("],");

    printf("\"vehicles\":[");
    for (int i = 0; i < s->num_vehicles; i++) {
        Vehicle *v = &s->vehicles[i];
        const char *tp = (v->type == EMERGENCY) ? "EMERG" : "NORMAL";
        const char *st = v->completed ? "DONE" : (v->active ? "ACTIVE" : "WAITING");
        printf("{\"id\":%d,\"type\":\"%s\",\"src\":%d,\"dst\":%d,\"wait\":%d,\"travel\":%d,\"status\":\"%s\"}%s",
               v->id+1, tp, v->source_road+1, v->dest_road+1, v->waiting_time, v->travel_time, st,
               i < s->num_vehicles-1 ? "," : "");
    }
    printf("],");

    printf("\"steps\":[");
    for (int t = 0; t < s->num_steps_recorded; t++) {
        StepSnapshot *ss = &s->steps[t];
        printf("{\"step\":%d,\"active_vehicles\":%d,\"roads\":[", ss->step, ss->active_vehicles);
        for (int i = 0; i < s->num_roads; i++)
            printf("{\"count\":%d,\"cong\":%d}%s", ss->road_count[i], ss->road_cong[i], i<s->num_roads-1?",":"");
        printf("],\"intersections\":[");
        for (int i = 0; i < s->num_intersections; i++)
            printf("{\"green\":%d,\"timer\":%d,\"adaptive\":%d}%s",
                   ss->inter_green[i]+1, ss->inter_timer[i], ss->inter_adaptive[i], i<s->num_intersections-1?",":"");
        jsonEscapeStr(ss->events, esc, sizeof(esc));
        printf("],\"events\":\"%s\"}%s", esc, t < s->num_steps_recorded-1 ? "," : "");
    }
    printf("],");

    printf("\"heatmap\":[");
    for (int i = 0; i < s->num_roads; i++)
        printf("%d%s", s->metrics.congestion_level[i], i<s->num_roads-1?",":"");
    printf("]}\n");
}

static int jsonGetInt(const char *json, const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
}

int main(void) {
    char config[1024] = {0};
    if (!fgets(config, sizeof(config), stdin))
        strcpy(config, "{\"location\":1,\"timesteps\":15}");

    int location      = jsonGetInt(config, "location",  1);
    int timesteps     = jsonGetInt(config, "timesteps", 15);
    int vehicle_count = jsonGetInt(config, "vehicles",  0);

    SimCtx *s = (SimCtx *)calloc(1, sizeof(SimCtx));
    if (!s) { fprintf(stderr, "{\"error\":\"OOM\"}\n"); return 1; }
    g_ctx = s;

    switch (location) {
        case 2: initAdyar(s);       break;
        case 3: initVelachery(s);   break;
        default: initThiruvanmiyur(s); break;
    }

    if (timesteps > 0 && timesteps <= MAX_STEPS) s->total_time_steps = timesteps;
    if (vehicle_count > 0) scaleVehicles(s, vehicle_count);

    int max_entry = 0;
    for (int i = 0; i < s->num_vehicles; i++)
        if (s->vehicles[i].entry_time > max_entry) max_entry = s->vehicles[i].entry_time;
    if (max_entry >= s->total_time_steps) s->total_time_steps = max_entry + 2;
    if (s->total_time_steps > MAX_STEPS) s->total_time_steps = MAX_STEPS;
    s->metrics.total_steps = s->total_time_steps;

    for (int t = 0; t < s->total_time_steps && t < MAX_STEPS; t++) {
        g_cur_step = t;
        for (int i = 0; i < s->num_vehicles; i++) s->vehicles[i].counted_this_step = 0;

        updateSignals(s);
        addVehicles(s, t);
        for (int i = 0; i < s->num_roads; i++) enforceSafeDistance(s, i);
        moveVehicles(s);
        for (int i = 0; i < s->num_roads; i++) { updateCongestion(s, i); detectSpillback(s, i); }
        recordMetrics(s, t + 1);
        if (detectDeadlock(s)) resolveDeadlock(s);
        recordSnapshot(s, t);   /* after all updates — consistent state */

        int pending = 0;
        for (int i = 0; i < s->num_vehicles; i++)
            if (s->vehicles[i].active ||
                (!s->vehicles[i].completed && s->vehicles[i].entry_time <= t))
                pending++;
        if (!pending) { s->metrics.total_steps = t + 1; break; }
    }

    outputJSON(s);
    free(s);
    return 0;
}
