// ECS tests: handles and generations, archetype moves, the Play in Editor snapshot, and the cost of the hot paths
// (a system over 1M entities, a snapshot and restore of 100k), printed in ms so a regression shows.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ecs.h"
#include "platform.h"
#include "systems.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void test_handles(void) {
    World w;
    CHECK(ecs_init(&w));
    Entity a = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    Entity b = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    CHECK(a != ENTITY_NONE && b != ENTITY_NONE && a != b);
    CHECK(ecs_alive(&w, a) && ecs_alive(&w, b));
    CTransform* t = ecs_get(&w, a, COMP_TRANSFORM);
    CHECK(t && t->scale[0] == 1.0f && t->location[0] == 0.0f);
    CHECK(ecs_get(&w, a, COMP_ROTATING) == NULL);
    snprintf(((CName*)ecs_get(&w, b, COMP_NAME))->text, RE_NAME_MAX, "B");

    ecs_destroy(&w, a); // b moves into a's row
    CHECK(!ecs_alive(&w, a));
    CHECK(ecs_get(&w, a, COMP_NAME) == NULL);
    CHECK(strcmp(((CName*)ecs_get(&w, b, COMP_NAME))->text, "B") == 0);
    Entity c = ecs_create(&w, COMP_BIT(COMP_NAME));
    CHECK(entity_index(c) == entity_index(a) && entity_gen(c) == entity_gen(a) + 1); // slot reused, old handle stale
    CHECK(!ecs_alive(&w, a) && ecs_alive(&w, c));
    CHECK(w.alive == 2);
    ecs_shutdown(&w);
}

static void test_add_remove(void) {
    World w;
    ecs_init(&w);
    Entity e = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    ((CTransform*)ecs_get(&w, e, COMP_TRANSFORM))->location[2] = 42.0f;
    CHECK(ecs_add(&w, e, COMP_ROTATING));
    CHECK(ecs_mask(&w, e) == (COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_ROTATING)));
    CHECK(((CTransform*)ecs_get(&w, e, COMP_TRANSFORM))->location[2] == 42.0f); // kept across the move
    ((CRotating*)ecs_get(&w, e, COMP_ROTATING))->rate[1] = 90.0f;
    system_rotating(&w, 0.5f);
    CHECK(fabsf(((CTransform*)ecs_get(&w, e, COMP_TRANSFORM))->rotation[1] - 45.0f) < 1e-4f);
    CHECK(ecs_remove(&w, e, COMP_ROTATING));
    CHECK(ecs_get(&w, e, COMP_ROTATING) == NULL);
    CHECK(((CTransform*)ecs_get(&w, e, COMP_TRANSFORM))->rotation[1] == 45.0f);
    Entity d = ecs_clone(&w, e);
    CHECK(d != e && ((CTransform*)ecs_get(&w, d, COMP_TRANSFORM))->location[2] == 42.0f);
    ecs_shutdown(&w);
}

static void test_snapshot(void) {
    World w;
    WorldSnapshot s;
    memset(&s, 0, sizeof s);
    ecs_init(&w);
    Entity keep = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_ROTATING));
    Entity gone = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    ((CRotating*)ecs_get(&w, keep, COMP_ROTATING))->rate[1] = 90.0f;
    CHECK(ecs_snapshot_save(&w, &s));

    // "Play": things change, die and are born, a new archetype appears
    system_rotating(&w, 1.0f);
    ecs_destroy(&w, gone);
    Entity born = ecs_create(&w, COMP_BIT(COMP_ROTATING));
    for (int i = 0; i < 1000; i++) ecs_create(&w, COMP_BIT(COMP_TRANSFORM));
    CHECK(((CTransform*)ecs_get(&w, keep, COMP_TRANSFORM))->rotation[1] == 90.0f);

    ecs_snapshot_restore(&w, &s);
    CHECK(((CTransform*)ecs_get(&w, keep, COMP_TRANSFORM))->rotation[1] == 0.0f);
    CHECK(ecs_alive(&w, gone)); // back with the same handle
    CHECK(!ecs_alive(&w, born));
    CHECK(w.alive == 2);
    Entity again = ecs_create(&w, COMP_BIT(COMP_NAME));
    CHECK(ecs_alive(&w, again) && ecs_alive(&w, gone) && ecs_alive(&w, keep));
    ecs_snapshot_free(&s);
    ecs_shutdown(&w);
}

// The blob (undo history, .rlevel): a world written, changed and read back is the world written, handles included
static void test_blob(void) {
    World w;
    ecs_init(&w);
    Entity a = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    Entity b = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    Entity gone = ecs_create(&w, COMP_BIT(COMP_LIGHT));
    ecs_destroy(&w, gone);
    snprintf(((CName*)ecs_get(&w, b, COMP_NAME))->text, RE_NAME_MAX, "Rock");
    ((CTransform*)ecs_get(&w, a, COMP_TRANSFORM))->location[0] = 123.0f;
    size_t n = ecs_serialized_size(&w);
    static uint8_t blob[1 << 16];
    CHECK(n < sizeof blob && ecs_serialize(&w, blob) == n);

    ecs_destroy(&w, a);
    Entity c = ecs_create(&w, COMP_BIT(COMP_CAMERA)); // a new archetype after the blob
    ((CStaticMesh*)ecs_get(&w, b, COMP_STATIC_MESH))->mesh = 3;
    CHECK(ecs_deserialize(&w, blob, n));
    CHECK(ecs_alive(&w, a) && ecs_alive(&w, b) && !ecs_alive(&w, c) && !ecs_alive(&w, gone));
    CHECK(((CTransform*)ecs_get(&w, a, COMP_TRANSFORM))->location[0] == 123.0f);
    CHECK(strcmp(((CName*)ecs_get(&w, b, COMP_NAME))->text, "Rock") == 0);
    CHECK(((CStaticMesh*)ecs_get(&w, b, COMP_STATIC_MESH))->mesh == 0);
    CHECK(w.alive == 2);
    // a damaged blob is refused and leaves the world alone
    blob[8] ^= 0xFF;
    CHECK(!ecs_deserialize(&w, blob, n));
    CHECK(!ecs_deserialize(&w, blob, 10));
    CHECK(w.alive == 2 && ecs_alive(&w, a));
    // into a fresh world (opening a level)
    blob[8] ^= 0xFF;
    World w2;
    ecs_init(&w2);
    CHECK(ecs_deserialize(&w2, blob, n));
    CHECK(ecs_alive(&w2, a) && ecs_alive(&w2, b) && w2.alive == 2);
    Entity d = ecs_create(&w2, COMP_BIT(COMP_NAME));
    CHECK(ecs_alive(&w2, d) && d != a && d != b);
    ecs_shutdown(&w2);
    ecs_shutdown(&w);
}

static void bench(void) {
    World w;
    WorldSnapshot s;
    memset(&s, 0, sizeof s);
    ecs_init(&w);
    const int n = 1000000;
    double t0 = re_time_seconds();
    for (int i = 0; i < n; i++) ecs_create(&w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_ROTATING));
    double t1 = re_time_seconds();
    for (int i = 0; i < 10; i++) system_rotating(&w, 0.016f);
    double t2 = re_time_seconds();
    CHECK(ecs_snapshot_save(&w, &s));
    double t3 = re_time_seconds();
    ecs_snapshot_restore(&w, &s);
    double t4 = re_time_seconds();
    printf("bench: create 1M %.2f ms, rotating system over 1M %.3f ms/frame, snapshot %.2f ms (%.1f MB), restore %.2f ms\n",
           (t1 - t0) * 1e3, (t2 - t1) * 1e3 / 10, (t3 - t2) * 1e3, s.bytes / 1048576.0, (t4 - t3) * 1e3);
    ecs_snapshot_free(&s);
    ecs_shutdown(&w);
}

int main(void) {
    test_handles();
    test_add_remove();
    test_snapshot();
    test_blob();
    bench();
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
